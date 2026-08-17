// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/08/17

// HTTP API 集成测试：临时端口起真实 brpc server，经 HTTP 覆盖 collections
// CRUD、文档生命周期、LWW、向量检索与认证/租户隔离（DESIGN.md §10）。

#include <brpc/channel.h>
#include <brpc/server.h>
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "cpp/pl/minisearch/auth/console_auth.h"
#include "cpp/pl/minisearch/auth/keystore.h"
#include "cpp/pl/minisearch/server/context.h"
#include "cpp/pl/minisearch/server/http_api.h"

namespace {

struct HttpReply {
    int status = 0;
    std::string body;
    std::string location; // Location 响应头（重定向断言用）
};

// 共享装配：auth 开关决定鉴权路径；auth 开启时 bootstrap 一把 admin key。
class HttpApiTestFixture : public ::testing::Test {
protected:
    explicit HttpApiTestFixture(bool auth_on, std::string web_dir = "")
        : web_dir_(std::move(web_dir)), auth_on_(auth_on) {}

    void SetUp() override {
        // bootstrap 会写 <dir>/bootstrap.key，用独立临时目录避免污染环境
        std::string tmpl = "/tmp/minisearch_http_api_test_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        ASSERT_NE(nullptr, ::mkdtemp(buf.data()));
        tmp_dir_ = buf.data();

        keys_ = std::make_unique<pl::minisearch::auth::KeyStore>("");
        if (auth_on_) {
            admin_key_ = keys_->BootstrapIfNeeded(tmp_dir_).value();
        }
        // console auth: bootstrap admin/changeme for console tests
        pl::minisearch::auth::ConsoleAuth::Options console_opts;
        console_opts.path = tmp_dir_ + "/console_users.json";
        console_opts.bootstrap_user = "admin";
        console_opts.bootstrap_password = "changeme";
        console_auth_ =
            std::make_unique<pl::minisearch::auth::ConsoleAuth>(std::move(console_opts));
        context_ = std::make_unique<pl::minisearch::server::TenantContext>("", "Flat");
        service_ = std::make_unique<pl::minisearch::server::HttpApiService>(context_.get(),
                                                                            keys_.get(),
                                                                            auth_on_,
                                                                            nullptr,
                                                                            nullptr,
                                                                            console_auth_.get(),
                                                                            web_dir_.c_str());
        ASSERT_EQ(0,
                  server_.AddService(service_.get(),
                                     brpc::SERVER_DOESNT_OWN_SERVICE,
                                     "/api/v2/* => default_method,"
                                     "/healthz => default_method,"
                                     "/console/* => default_method,"
                                     "/ => default_method"));
        ASSERT_EQ(0, server_.Start("127.0.0.1:0", nullptr));

        char addr[64];
        snprintf(addr, sizeof(addr), "127.0.0.1:%d", server_.listen_address().port);
        brpc::ChannelOptions options;
        options.protocol = "http";
        ASSERT_EQ(0, channel_.Init(addr, "", &options));
        base_ = std::string("http://") + addr;
    }

    void TearDown() override {
        server_.Stop(0);
        if (!tmp_dir_.empty()) {
            std::string cmd = "rm -rf '" + tmp_dir_ + "'";
            EXPECT_EQ(0, system(cmd.c_str()));
        }
    }

    // 从签发信息响应中提取 "key":"msk_..." 明文。
    static std::string extract(const std::string& body, const std::string& field) {
        const std::string needle = "\"" + field + "\":\"";
        const size_t pos = body.find(needle);
        if (pos == std::string::npos) {
            return "";
        }
        const size_t start = pos + needle.size();
        return body.substr(start, body.find('"', start) - start);
    }

    HttpReply Request(const std::string& method,
                      const std::string& path,
                      const std::string& body = "",
                      const std::string& bearer = "") {
        brpc::Controller cntl;
        brpc::HttpMethod http_method = brpc::HTTP_METHOD_GET;
        if (method == "PUT") {
            http_method = brpc::HTTP_METHOD_PUT;
        } else if (method == "POST") {
            http_method = brpc::HTTP_METHOD_POST;
        } else if (method == "DELETE") {
            http_method = brpc::HTTP_METHOD_DELETE;
        }
        cntl.http_request().set_method(http_method);
        cntl.http_request().uri() = base_ + path;
        if (!bearer.empty()) {
            cntl.http_request().SetHeader("Authorization", "Bearer " + bearer);
        }
        if (!body.empty()) {
            cntl.request_attachment().append(body);
        }
        channel_.CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
        HttpReply reply;
        reply.status = cntl.http_response().status_code();
        reply.body = cntl.response_attachment().to_string();
        const std::string* loc = cntl.http_response().GetHeader("Location");
        if (loc != nullptr) {
            reply.location = *loc;
        }
        return reply;
    }

    // Schema: title text, tags keyword, vec vector(4, client mode).
    std::string collection_body(const std::string& name) {
        return R"({"name":")" + name +
               R"(","default_analyzer":"cjk_jieba","fields":[)"
               R"({"name":"title","type":"text","indexed":true,"stored":true},)"
               R"({"name":"tags","type":"keyword","indexed":true,"stored":true},)"
               R"({"name":"vec","type":"vector","indexed":false,"stored":true,)"
               R"("dims":4,"metric":"cosine","mode":"client"}]})";
    }

    const std::string doc1 =
        R"({"version":1,"fields":{"title":{"s":"presto 调优"},"tags":{"s":"wiki"},)"
        R"("vec":{"v":{"data":[1.0,0.0,0.0,0.0]}}}})";
    const std::string doc2 =
        R"({"version":1,"fields":{"title":{"s":"loom 架构"},"tags":{"s":"wiki"},)"
        R"("vec":{"v":{"data":[0.0,1.0,0.0,0.0]}}}})";

    brpc::Server server_;
    brpc::Channel channel_;
    std::string base_;
    std::string tmp_dir_;
    std::unique_ptr<pl::minisearch::auth::KeyStore> keys_;
    std::unique_ptr<pl::minisearch::auth::ConsoleAuth> console_auth_;
    std::string web_dir_;
    std::unique_ptr<pl::minisearch::server::TenantContext> context_;
    std::unique_ptr<pl::minisearch::server::HttpApiService> service_;
    bool auth_on_;
    pl::minisearch::auth::KeyStore::Issued admin_key_;
};

class HttpApiTest : public HttpApiTestFixture {
protected:
    HttpApiTest() : HttpApiTestFixture(/*auth_on=*/false) {}
};

TEST_F(HttpApiTest, CollectionLifecycle) {
    auto create = Request("POST", "/api/v2/collections", collection_body("kb"));
    EXPECT_EQ(create.status, 200);
    EXPECT_NE(create.body.find("\"ok\":true"), std::string::npos) << create.body;

    auto duplicate = Request("POST", "/api/v2/collections", collection_body("kb"));
    EXPECT_EQ(duplicate.status, 409);

    auto list = Request("GET", "/api/v2/collections");
    EXPECT_EQ(list.status, 200);
    EXPECT_NE(list.body.find("\"kb\""), std::string::npos);

    auto drop_no_confirm = Request("DELETE", "/api/v2/collections/kb");
    EXPECT_EQ(drop_no_confirm.status, 400);
    auto drop = Request("DELETE", "/api/v2/collections/kb?confirm=kb");
    EXPECT_EQ(drop.status, 200);
    auto list_after = Request("GET", "/api/v2/collections");
    EXPECT_EQ(list_after.body.find("\"kb\""), std::string::npos);
}

TEST_F(HttpApiTest, InvalidSchemaRejected) {
    std::string body = R"({"name":"bad","fields":[{"name":"v","type":"vector",)"
                       R"("dims":0,"mode":"client"}]})";
    auto reply = Request("POST", "/api/v2/collections", body);
    EXPECT_EQ(reply.status, 400);
}

TEST_F(HttpApiTest, DocumentLifecycleAndSearch) {
    ASSERT_EQ(Request("POST", "/api/v2/collections", collection_body("kb")).status, 200);

    EXPECT_EQ(Request("PUT", "/api/v2/kb/documents/doc1", doc1).status, 200);
    EXPECT_EQ(Request("PUT", "/api/v2/kb/documents/doc2", doc2).status, 200);

    auto fetched = Request("GET", "/api/v2/kb/documents/doc1");
    EXPECT_EQ(fetched.status, 200);
    EXPECT_NE(fetched.body.find("presto 调优"), std::string::npos);

    auto search =
        Request("POST", "/api/v2/kb/search", R"({"embedding":[0.9,0.1,0.0,0.0],"top_k":1})");
    EXPECT_EQ(search.status, 200);
    EXPECT_NE(search.body.find("doc1"), std::string::npos) << search.body;
    EXPECT_EQ(search.body.find("doc2"), std::string::npos);

    // LWW: re-upserting with a non-increasing version is rejected with 409.
    EXPECT_EQ(Request("PUT", "/api/v2/kb/documents/doc1", doc1).status, 409);

    auto deleted = Request("DELETE", "/api/v2/kb/documents/doc2");
    EXPECT_EQ(deleted.status, 200);
    EXPECT_EQ(Request("GET", "/api/v2/kb/documents/doc2").status, 404);

    // Deleted document drops out of search results.
    auto search_after =
        Request("POST", "/api/v2/kb/search", R"({"embedding":[0.0,1.0,0.0,0.0],"top_k":5})");
    EXPECT_EQ(search_after.status, 200);
    EXPECT_EQ(search_after.body.find("doc2"), std::string::npos);
}

TEST_F(HttpApiTest, UnknownCollectionAndRoute) {
    EXPECT_EQ(Request("GET", "/api/v2/missing/documents/x").status, 404);
    EXPECT_EQ(Request("POST", "/api/v2/kb/nope", "{}").status, 404);
}

TEST_F(HttpApiTest, Healthz) {
    auto reply = Request("GET", "/healthz");
    EXPECT_EQ(reply.status, 200);
    EXPECT_NE(reply.body.find("ok"), std::string::npos);
}

TEST_F(HttpApiTest, ChineseHybridSearch) {
    ASSERT_EQ(Request("POST", "/api/v2/collections", collection_body("kb")).status, 200);

    const std::string doc1 =
        R"({"version":1,"fields":{"title":{"s":"presto 查询性能调优指南"},"tags":{"s":"wiki"},)"
        R"("vec":{"v":{"data":[1.0,0.0,0.0,0.0]}}}})";
    const std::string doc2 =
        R"({"version":1,"fields":{"title":{"s":"loom 会话架构设计"},"tags":{"s":"wiki"},)"
        R"("vec":{"v":{"data":[0.0,1.0,0.0,0.0]}}}})";
    ASSERT_EQ(Request("PUT", "/api/v2/kb/documents/doc1", doc1).status, 200);
    ASSERT_EQ(Request("PUT", "/api/v2/kb/documents/doc2", doc2).status, 200);

    // queries:analyze：jieba 分词结果
    auto analyzed = Request("POST", "/api/v2/kb/queries:analyze", R"({"text":"presto 调优"})");
    EXPECT_EQ(analyzed.status, 200);
    EXPECT_NE(analyzed.body.find("\"term\":\"presto\""), std::string::npos) << analyzed.body;
    EXPECT_NE(analyzed.body.find("\"term\":\"调优\""), std::string::npos) << analyzed.body;

    // 纯文本查询：embedding 未配置时 BM25 路独立命中（降级语义 §7.5）
    auto search = Request("POST", "/api/v2/kb/search", R"({"text":"查询性能调优","top_k":5})");
    EXPECT_EQ(search.status, 200);
    EXPECT_NE(search.body.find("doc1"), std::string::npos) << search.body;
    EXPECT_EQ(search.body.find("doc2"), std::string::npos);

    // 中文关键词检索（"调优"只命中 doc1）
    auto keyword = Request("POST", "/api/v2/kb/search", R"({"text":"调优","top_k":5})");
    EXPECT_EQ(keyword.status, 200);
    EXPECT_NE(keyword.body.find("doc1"), std::string::npos) << keyword.body;
    EXPECT_EQ(keyword.body.find("doc2"), std::string::npos);
}

// ---------------------------------------------------------------------------
// auth / tenancy（DESIGN.md §10）
// ---------------------------------------------------------------------------

class AuthApiTest : public HttpApiTestFixture {
protected:
    AuthApiTest() : HttpApiTestFixture(/*auth_on=*/true) {}
};

TEST_F(AuthApiTest, HealthzOnlyUnauthenticatedEndpoint) {
    EXPECT_EQ(Request("GET", "/healthz").status, 200);
    EXPECT_EQ(Request("GET", "/api/v2/collections").status, 401);
    EXPECT_EQ(Request("GET", "/api/v2/collections", "", "msk_bogus").status, 401);
}

TEST_F(AuthApiTest, AdminKeyManagesTenantsAndKeys) {
    const std::string admin = admin_key_.key;
    EXPECT_EQ(Request("GET", "/api/v2/collections", "", admin).status, 200);

    // 签发 team-a 的 writer key（白名单 kb）
    auto issued = Request("POST",
                          "/api/v2/admin/tenants/team-a/keys",
                          R"({"role":"writer","collections":["kb"]})",
                          admin);
    ASSERT_EQ(issued.status, 200);
    std::string team_key;
    {
        // 从响应中提取 "key":"msk_..."
        const size_t pos = issued.body.find("\"key\":\"");
        ASSERT_NE(pos, std::string::npos) << issued.body;
        const size_t start = pos + 7;
        const size_t end = issued.body.find('"', start);
        team_key = issued.body.substr(start, end - start);
    }

    // team-a 的 writer：可写文档，不能建 collection（403）。
    // admin 用 ?tenant=team-a 在 team-a 命名空间建 kb。
    ASSERT_EQ(
        Request("POST", "/api/v2/collections?tenant=team-a", collection_body("kb"), admin).status,
        200);
    EXPECT_EQ(Request("PUT", "/api/v2/kb/documents/doc1", doc1, team_key).status, 200);
    EXPECT_EQ(Request("POST", "/api/v2/collections", collection_body("other"), team_key).status,
              403);

    // 租户隔离：default 命名空间（admin 默认）看不到 team-a 的 kb
    auto default_list = Request("GET", "/api/v2/collections", "", admin);
    EXPECT_EQ(default_list.body.find("\"kb\""), std::string::npos) << default_list.body;

    // reader key 只读：写 403
    auto reader =
        Request("POST", "/api/v2/admin/tenants/team-a/keys", R"({"role":"reader"})", admin);
    ASSERT_EQ(reader.status, 200);
    const size_t rpos = reader.body.find("\"key\":\"");
    const size_t rstart = rpos + 7;
    const std::string reader_key =
        reader.body.substr(rstart, reader.body.find('"', rstart) - rstart);
    ASSERT_EQ(Request("POST", "/api/v2/collections", collection_body("kb"), reader_key).status,
              403);

    // 吊销 writer key 后立即 401
    const size_t kpos = issued.body.find("\"key_id\":\"");
    const size_t kstart = kpos + 10;
    const std::string key_id = issued.body.substr(kstart, issued.body.find('"', kstart) - kstart);
    auto revoke = Request("DELETE", "/api/v2/admin/tenants/team-a/keys/" + key_id, "", admin);
    EXPECT_EQ(revoke.status, 200);
    EXPECT_EQ(Request("GET", "/api/v2/collections", "", team_key).status, 401);
}

TEST_F(AuthApiTest, TenantAdminCannotCrossTenants) {
    const std::string admin = admin_key_.key;
    auto issued =
        Request("POST", "/api/v2/admin/tenants/team-a/keys", R"({"role":"tenant_admin"})", admin);
    ASSERT_EQ(issued.status, 200);
    const std::string ta_key = extract(issued.body, "key");
    ASSERT_FALSE(ta_key.empty());

    // tenant_admin 可签本租户 key，不能签其他租户的
    EXPECT_EQ(
        Request("POST", "/api/v2/admin/tenants/team-a/keys", R"({"role":"reader"})", ta_key).status,
        200);
    EXPECT_EQ(
        Request("POST", "/api/v2/admin/tenants/team-b/keys", R"({"role":"reader"})", ta_key).status,
        403);
    // 非 admin 不能列全部租户
    EXPECT_EQ(Request("GET", "/api/v2/admin/tenants", "", ta_key).status, 403);
}

TEST_F(AuthApiTest, TenantAdminCannotIssueTenantAdmin) {
    const std::string admin = admin_key_.key;
    auto issued =
        Request("POST", "/api/v2/admin/tenants/team-a/keys", R"({"role":"tenant_admin"})", admin);
    ASSERT_EQ(issued.status, 200);
    const std::string ta_key = extract(issued.body, "key");
    ASSERT_FALSE(ta_key.empty());

    // tenant_admin 只能签发 writer/reader（§10.2），不能复制自己
    EXPECT_EQ(
        Request("POST", "/api/v2/admin/tenants/team-a/keys", R"({"role":"tenant_admin"})", ta_key)
            .status,
        400);
    EXPECT_EQ(
        Request("POST", "/api/v2/admin/tenants/team-a/keys", R"({"role":"writer"})", ta_key).status,
        200);
    // admin 可以签发 tenant_admin
    EXPECT_EQ(
        Request("POST", "/api/v2/admin/tenants/team-a/keys", R"({"role":"tenant_admin"})", admin)
            .status,
        200);
}

TEST_F(AuthApiTest, WriterWhitelistEnforcedOnWriteAndRead) {
    const std::string admin = admin_key_.key;
    // admin 在 team-a 命名空间建 kb 与 other 两个 collection
    ASSERT_EQ(
        Request("POST", "/api/v2/collections?tenant=team-a", collection_body("kb"), admin).status,
        200);
    ASSERT_EQ(Request("POST", "/api/v2/collections?tenant=team-a", collection_body("other"), admin)
                  .status,
              200);

    auto issued = Request("POST",
                          "/api/v2/admin/tenants/team-a/keys",
                          R"({"role":"writer","collections":["kb"]})",
                          admin);
    ASSERT_EQ(issued.status, 200);
    const std::string writer = extract(issued.body, "key");
    ASSERT_FALSE(writer.empty());

    // 白名单内：写/读/检索放行
    EXPECT_EQ(Request("PUT", "/api/v2/kb/documents/doc1", doc1, writer).status, 200);
    EXPECT_EQ(Request("GET", "/api/v2/kb/documents/doc1", "", writer).status, 200);
    EXPECT_EQ(Request("POST", "/api/v2/kb/search", R"({"text":"presto"})", writer).status, 200);
    // 白名单外：写与读都拒绝（writer 白名单对写路径同样生效）
    EXPECT_EQ(Request("PUT", "/api/v2/other/documents/doc1", doc1, writer).status, 403);
    EXPECT_EQ(Request("GET", "/api/v2/other/documents/doc1", "", writer).status, 403);
    EXPECT_EQ(Request("POST", "/api/v2/other/search", R"({"text":"presto"})", writer).status, 403);
}

TEST_F(AuthApiTest, InvalidTenantNameRejected) {
    const std::string admin = admin_key_.key;
    // 资源名只允许 [A-Za-z0-9_-]，点号/斜杠/空格等一律拒绝
    EXPECT_EQ(Request("POST", "/api/v2/admin/tenants/bad..name/keys", R"({"role":"reader"})", admin)
                  .status,
              400);
    EXPECT_EQ(Request("GET", "/api/v2/collections?tenant=bad..name", "", admin).status, 400);
}

// ---------------------------------------------------------------------------
// M1 请求面：degraded / filter / weights / stats / 斜杠文档 id
// ---------------------------------------------------------------------------

TEST_F(HttpApiTest, InvalidCollectionRequests) {
    // 非法名称
    EXPECT_EQ(Request("POST", "/api/v2/collections", collection_body("bad/name")).status, 400);
    EXPECT_EQ(Request("POST", "/api/v2/collections", collection_body("with space")).status, 400);
    // 重复字段名
    const std::string dup =
        R"({"name":"dup","fields":[{"name":"t","type":"text"},{"name":"t","type":"keyword"}]})";
    EXPECT_EQ(Request("POST", "/api/v2/collections", dup).status, 400);
    // 未知 analyzer
    const std::string bad_analyzer =
        R"({"name":"ba","default_analyzer":"nope","fields":[{"name":"t","type":"text"}]})";
    EXPECT_EQ(Request("POST", "/api/v2/collections", bad_analyzer).status, 400);
}

TEST_F(HttpApiTest, DocumentIdWithSlash) {
    ASSERT_EQ(Request("POST", "/api/v2/collections", collection_body("kb")).status, 200);
    // 文档 id 允许包含 '/'（DESIGN.md §4.2 的 id 形式）
    EXPECT_EQ(Request("PUT", "/api/v2/kb/documents/docs/a.md", doc1).status, 200);
    auto fetched = Request("GET", "/api/v2/kb/documents/docs/a.md");
    EXPECT_EQ(fetched.status, 200);
    EXPECT_NE(fetched.body.find("docs/a.md"), std::string::npos) << fetched.body;
    EXPECT_EQ(Request("DELETE", "/api/v2/kb/documents/docs/a.md").status, 200);
    EXPECT_EQ(Request("GET", "/api/v2/kb/documents/docs/a.md").status, 404);
}

TEST_F(HttpApiTest, SearchDegradedWeightsAndFilter) {
    ASSERT_EQ(Request("POST", "/api/v2/collections", collection_body("kb")).status, 200);
    ASSERT_EQ(Request("PUT", "/api/v2/kb/documents/doc1", doc1).status, 200);
    ASSERT_EQ(Request("PUT", "/api/v2/kb/documents/doc2", doc2).status, 200);

    // 未配置 embedding client：text 查询走 BM25，响应标记 degraded=["vector"]
    auto degraded = Request("POST", "/api/v2/kb/search", R"({"text":"调优","top_k":5})");
    EXPECT_EQ(degraded.status, 200);
    EXPECT_NE(degraded.body.find("\"degraded\":[\"vector\"]"), std::string::npos) << degraded.body;
    EXPECT_NE(degraded.body.find("\"took_ms\":"), std::string::npos);

    // weights 关闭 BM25 路：无向量可用时无结果但不报错
    auto bm25_off = Request("POST",
                            "/api/v2/kb/search",
                            R"({"text":"调优","top_k":5,"weights":{"bm25":0.0,"vector":1.0}})");
    EXPECT_EQ(bm25_off.status, 200);
    EXPECT_EQ(bm25_off.body.find("doc1"), std::string::npos) << bm25_off.body;
    // 非法权重
    EXPECT_EQ(
        Request("POST", "/api/v2/kb/search", R"({"text":"x","weights":{"bm25":-1.0,"vector":1.0}})")
            .status,
        400);

    // filter：tags != wiki 时没有候选
    auto filtered = Request("POST",
                            "/api/v2/kb/search",
                            R"({"text":"调优","top_k":5,"filter":{"and":[)"
                            R"({"field":"tags","op":"=","values":[{"s":"other"}]}]}})");
    EXPECT_EQ(filtered.status, 200);
    EXPECT_EQ(filtered.body.find("doc1"), std::string::npos) << filtered.body;
    // filter 命中：tags = wiki 保留
    auto kept = Request("POST",
                        "/api/v2/kb/search",
                        R"({"text":"调优","top_k":5,"filter":{"and":[)"
                        R"({"field":"tags","op":"=","values":[{"s":"wiki"}]}]}})");
    EXPECT_EQ(kept.status, 200);
    EXPECT_NE(kept.body.find("doc1"), std::string::npos) << kept.body;
    // 非法 filter：未知字段 400
    EXPECT_EQ(Request("POST",
                      "/api/v2/kb/search",
                      R"({"text":"x","filter":{"and":[{"field":"nope","op":"=",)"
                      R"("values":[{"s":"a"}]}]}})")
                  .status,
              400);

    // rerank=true 但未配置 rerank client：降级标记，结果仍返回
    auto reranked =
        Request("POST", "/api/v2/kb/search", R"({"text":"调优","top_k":5,"rerank":true})");
    EXPECT_EQ(reranked.status, 200);
    EXPECT_NE(reranked.body.find("\"rerank\""), std::string::npos) << reranked.body;
}

TEST_F(HttpApiTest, AdminStatsEndpoint) {
    ASSERT_EQ(Request("POST", "/api/v2/collections", collection_body("kb")).status, 200);
    ASSERT_EQ(Request("PUT", "/api/v2/kb/documents/doc1", doc1).status, 200);
    auto stats = Request("GET", "/api/v2/admin/stats");
    EXPECT_EQ(stats.status, 200);
    EXPECT_NE(stats.body.find("\"total_collections\":1"), std::string::npos) << stats.body;
    EXPECT_NE(stats.body.find("\"total_active_documents\":1"), std::string::npos) << stats.body;
}

// ---------------------------------------------------------------------------
// Console session login / whoami / logout / change password
// (auth_on=true so Authenticate actually checks Bearer tokens)
// ---------------------------------------------------------------------------

TEST_F(AuthApiTest, ConsoleLoginAndSession) {
    // login with bootstrap credentials
    auto login = Request("POST", "/api/v2/auth/login", R"({"user":"admin","password":"changeme"})");
    ASSERT_EQ(login.status, 200);
    EXPECT_NE(login.body.find("\"ok\":true"), std::string::npos) << login.body;
    EXPECT_NE(login.body.find("\"token\":\"mss_"), std::string::npos) << login.body;

    const std::string session_token = extract(login.body, "token");
    ASSERT_FALSE(session_token.empty());

    // whoami with session token
    auto whoami = Request("GET", "/api/v2/auth/whoami", "", session_token);
    EXPECT_EQ(whoami.status, 200);
    EXPECT_NE(whoami.body.find("\"auth_type\":\"session\""), std::string::npos) << whoami.body;
    EXPECT_NE(whoami.body.find("\"role\":\"admin\""), std::string::npos) << whoami.body;

    // session token works for API calls
    auto collections = Request("GET", "/api/v2/collections", "", session_token);
    EXPECT_EQ(collections.status, 200);

    // logout
    auto logout = Request("POST", "/api/v2/auth/logout", "", session_token);
    EXPECT_EQ(logout.status, 200);
}

TEST_F(AuthApiTest, ConsoleLoginBadPassword) {
    auto bad = Request("POST", "/api/v2/auth/login", R"({"user":"admin","password":"wrong"})");
    EXPECT_EQ(bad.status, 401);
}

TEST_F(AuthApiTest, ConsoleChangePassword) {
    auto login = Request("POST", "/api/v2/auth/login", R"({"user":"admin","password":"changeme"})");
    ASSERT_EQ(login.status, 200);

    // change password
    auto change =
        Request("POST",
                "/api/v2/auth/password",
                R"({"user":"admin","old_password":"changeme","new_password":"newpass123"})");
    EXPECT_EQ(change.status, 200);
    EXPECT_NE(change.body.find("\"ok\":true"), std::string::npos) << change.body;

    // old password fails
    auto old_login =
        Request("POST", "/api/v2/auth/login", R"({"user":"admin","password":"changeme"})");
    EXPECT_EQ(old_login.status, 401);

    // new password works
    auto new_login =
        Request("POST", "/api/v2/auth/login", R"({"user":"admin","password":"newpass123"})");
    EXPECT_EQ(new_login.status, 200);

    // change back for other tests
    Request("POST",
            "/api/v2/auth/password",
            R"({"user":"admin","old_password":"newpass123","new_password":"changeme"})");
}

// ---------------------------------------------------------------------------
// Create tenant + List documents
// ---------------------------------------------------------------------------

TEST_F(HttpApiTest, CreateTenantAndListDocuments) {
    // create tenant
    auto create_t = Request("POST", "/api/v2/admin/tenants", R"({"name":"test-tenant"})");
    EXPECT_EQ(create_t.status, 200);
    EXPECT_NE(create_t.body.find("\"ok\":true"), std::string::npos) << create_t.body;

    // duplicate create -> 409
    auto dup = Request("POST", "/api/v2/admin/tenants", R"({"name":"test-tenant"})");
    EXPECT_EQ(dup.status, 409);

    // create collection in test-tenant
    auto create_c =
        Request("POST", "/api/v2/collections?tenant=test-tenant", collection_body("kb"));
    EXPECT_EQ(create_c.status, 200);

    // upsert a document
    EXPECT_EQ(Request("PUT", "/api/v2/kb/documents/doc1?tenant=test-tenant", doc1).status, 200);

    // list documents
    auto list = Request("GET", "/api/v2/kb/documents?tenant=test-tenant");
    EXPECT_EQ(list.status, 200);
    EXPECT_NE(list.body.find("\"total\":1"), std::string::npos) << list.body;
    EXPECT_NE(list.body.find("doc1"), std::string::npos) << list.body;

    // list with pagination
    auto page = Request("GET", "/api/v2/kb/documents?tenant=test-tenant&offset=0&limit=10");
    EXPECT_EQ(page.status, 200);
    EXPECT_NE(page.body.find("\"total\":1"), std::string::npos) << page.body;
}

// ---------------------------------------------------------------------------
// Markdown import
// ---------------------------------------------------------------------------

TEST_F(HttpApiTest, MarkdownImport) {
    ASSERT_EQ(Request("POST", "/api/v2/collections", collection_body("kb")).status, 200);

    // Markdown with headings; the collection schema has "title" (text) field.
    // We use title as the text_field for chunk content.
    // Note: newlines and quotes in JSON string must be escaped.
    std::string md = "# Heading 1\\n\\nThis is the first paragraph.\\n\\n## Subsection\\n\\nSecond "
                     "paragraph here.\\n\\n";
    auto import = Request("POST",
                          "/api/v2/kb/documents:import",
                          R"({"name":"test","content":")" + md + R"(","text_field":"title"})");
    // The collection "kb" has vec with mode=client, so no server embedding needed.
    EXPECT_EQ(import.status, 200);
    EXPECT_NE(import.body.find("\"ok\":true"), std::string::npos) << import.body;
    EXPECT_NE(import.body.find("\"chunks\":"), std::string::npos) << import.body;

    // verify chunks are in document list
    auto list = Request("GET", "/api/v2/kb/documents");
    EXPECT_NE(list.body.find("test#chunk_"), std::string::npos) << list.body;

    // re-import (idempotent): should replace, not duplicate
    auto reimport = Request("POST",
                            "/api/v2/kb/documents:import",
                            R"({"name":"test","content":")" + md + R"(","text_field":"title"})");
    EXPECT_EQ(reimport.status, 200);

    // count should not double
    auto list2 = Request("GET", "/api/v2/kb/documents");
    // extract total
    const std::string total_key = "\"total\":";
    size_t pos1 = list.body.find(total_key);
    size_t pos2 = list2.body.find(total_key);
    if (pos1 != std::string::npos && pos2 != std::string::npos) {
        // parse the numbers
        size_t start1 = pos1 + total_key.size();
        size_t end1 = list.body.find_first_of(",}", start1);
        size_t start2 = pos2 + total_key.size();
        size_t end2 = list2.body.find_first_of(",}", start2);
        std::string total1 = list.body.substr(start1, end1 - start1);
        std::string total2 = list2.body.substr(start2, end2 - start2);
        EXPECT_EQ(total1, total2) << "re-import should be idempotent";
    }

    // chunk id 含 '#'，GET/DELETE 必须支持 URL 编码（%23）访问：
    // brpc uri().path() 不解码，服务端需还原后再定位文档。
    auto get_encoded = Request("GET", "/api/v2/kb/documents/test%23chunk_0");
    EXPECT_EQ(get_encoded.status, 200) << get_encoded.body;
    EXPECT_NE(get_encoded.body.find("\"found\":true"), std::string::npos) << get_encoded.body;

    auto del_encoded = Request("DELETE", "/api/v2/kb/documents/test%23chunk_1?confirm=test");
    EXPECT_EQ(del_encoded.status, 200) << del_encoded.body;
    EXPECT_NE(del_encoded.body.find("\"ok\":true"), std::string::npos) << del_encoded.body;
}

// ---------------------------------------------------------------------------
// console 静态文件：/console 重定向 + index.html 兜底
// ---------------------------------------------------------------------------

class StaticFileTest : public HttpApiTestFixture {
protected:
    StaticFileTest() : HttpApiTestFixture(false) {
        // web_dir 指向临时目录下的 web/，SetUp 前准备好 index.html
        web_dir_ = "/tmp/minisearch_http_api_test_XXXXXX";
        std::vector<char> buf(web_dir_.begin(), web_dir_.end());
        buf.push_back('\0');
        if (::mkdtemp(buf.data()) != nullptr) {
            web_dir_ = buf.data();
            const std::string index = web_dir_ + "/index.html";
            std::ofstream out(index);
            out << "<html>console-index</html>";
            out.close();
        }
    }

    ~StaticFileTest() override {
        if (!web_dir_.empty() && web_dir_.find("XXXXXX") == std::string::npos) {
            const std::string cmd = "rm -rf '" + web_dir_ + "'";
            EXPECT_EQ(0, system(cmd.c_str()));
        }
    }
};

TEST_F(StaticFileTest, ConsoleRedirectsAndServesIndex) {
    // 无尾斜杠 /console 必须 302 到 /console/（此前落入认证分支 401）
    auto no_slash = Request("GET", "/console");
    EXPECT_EQ(no_slash.status, 302) << no_slash.body;
    EXPECT_EQ(no_slash.location, "/console/");

    // /console/ 兜底到 index.html（免认证，登录页本身需要加载）
    auto index = Request("GET", "/console/");
    EXPECT_EQ(index.status, 200);
    EXPECT_NE(index.body.find("console-index"), std::string::npos) << index.body;

    // 深层路径 404 正常返回 JSON 错误而非 401
    auto missing = Request("GET", "/console/nope.js");
    EXPECT_EQ(missing.status, 404);
}

} // namespace
