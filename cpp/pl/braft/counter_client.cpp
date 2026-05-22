// Copyright (c) 2024 liubang. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <braft/raft.h>
#include <braft/route_table.h>
#include <braft/util.h>
#include <brpc/channel.h>
#include <brpc/controller.h>
#include <bthread/bthread.h>
#include <gflags/gflags.h>

#include "cpp/pl/braft/proto/counter.pb.h"

DEFINE_bool(log_each_request, false, "Print log for each request");
DEFINE_bool(use_bthread, false, "Use bthread to send requests");
DEFINE_int32(add_percentage, 100, "Percentage of fetch_add requests (vs get)");
DEFINE_int64(added_by, 1, "Value added to the counter each time");
DEFINE_int32(thread_num, 1, "Number of threads sending requests");
DEFINE_int32(timeout_ms, 1000, "Timeout for each request");
DEFINE_string(conf, "", "Configuration of the raft group");
DEFINE_string(group, "Counter", "Id of the replication group");

bvar::LatencyRecorder g_latency_recorder("counter_client");

static void* sender(void* /*arg*/) {
    while (!::brpc::IsAskedToQuit()) {
        braft::PeerId leader;
        // Select leader from RouteTable.
        if (braft::rtb::select_leader(FLAGS_group, &leader) != 0) {
            butil::Status st = braft::rtb::refresh_leader(FLAGS_group, FLAGS_timeout_ms);
            if (!st.ok()) {
                LOG(WARNING) << "Fail to refresh_leader : " << st;
                bthread_usleep(FLAGS_timeout_ms * 1000L);
            }
            continue;
        }

        ::brpc::Channel channel;
        if (channel.Init(leader.addr, nullptr) != 0) {
            LOG(ERROR) << "Fail to init channel to " << leader;
            bthread_usleep(FLAGS_timeout_ms * 1000L);
            continue;
        }
        ::pl::braft::proto::CounterService_Stub stub(&channel);

        ::brpc::Controller cntl;
        cntl.set_timeout_ms(FLAGS_timeout_ms);
        ::pl::braft::proto::CounterResponse response;

        if (butil::fast_rand_less_than(100) < static_cast<size_t>(FLAGS_add_percentage)) {
            ::pl::braft::proto::FetchAddRequest request;
            request.set_value(FLAGS_added_by);
            stub.fetch_add(&cntl, &request, &response, nullptr);
        } else {
            ::pl::braft::proto::GetRequest request;
            stub.get(&cntl, &request, &response, nullptr);
        }

        if (cntl.Failed()) {
            LOG(WARNING) << "Fail to send request to " << leader << " : " << cntl.ErrorText();
            // Clear leadership since this RPC failed.
            braft::rtb::update_leader(FLAGS_group, braft::PeerId());
            bthread_usleep(FLAGS_timeout_ms * 1000L);
            continue;
        }
        if (!response.success()) {
            LOG(WARNING) << "Fail to send request to " << leader << ", redirecting to "
                         << (response.redirect().empty() ? "nowhere" : response.redirect());
            braft::rtb::update_leader(FLAGS_group, response.redirect());
            continue;
        }
        g_latency_recorder << cntl.latency_us();
        if (FLAGS_log_each_request) {
            LOG(INFO) << "Received response from " << leader << " value=" << response.value()
                      << " latency=" << cntl.latency_us();
            bthread_usleep(1000L * 1000L);
        }
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    butil::AtExitManager exit_manager;

    // Register raft group configuration to RouteTable.
    if (braft::rtb::update_configuration(FLAGS_group, FLAGS_conf) != 0) {
        LOG(ERROR) << "Fail to register configuration " << FLAGS_conf << " of group "
                   << FLAGS_group;
        return -1;
    }

    std::vector<bthread_t> bthread_tids(FLAGS_thread_num);
    std::vector<pthread_t> pthread_tids(FLAGS_thread_num);
    if (!FLAGS_use_bthread) {
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (pthread_create(&pthread_tids[i], nullptr, sender, nullptr) != 0) {
                LOG(ERROR) << "Fail to create pthread";
                return -1;
            }
        }
    } else {
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (bthread_start_background(&bthread_tids[i], nullptr, sender, nullptr) != 0) {
                LOG(ERROR) << "Fail to create bthread";
                return -1;
            }
        }
    }

    while (!::brpc::IsAskedToQuit()) {
        sleep(1);
        LOG_IF(INFO, !FLAGS_log_each_request)
            << "Sending Request to " << FLAGS_group << " (" << FLAGS_conf << ')'
            << " at qps=" << g_latency_recorder.qps(1)
            << " latency=" << g_latency_recorder.latency(1);
    }

    LOG(INFO) << "Counter client is going to quit";
    for (int i = 0; i < FLAGS_thread_num; ++i) {
        if (!FLAGS_use_bthread) {
            pthread_join(pthread_tids[i], nullptr);
        } else {
            bthread_join(bthread_tids[i], nullptr);
        }
    }

    return 0;
}
