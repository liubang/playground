//=====================================================================
//
// echo_server.cpp -
//
// Created by liubang on 2023/08/18 01:09
// Last Modified: 2023/08/18 01:09
//
//=====================================================================
#include <brpc/server.h>
#include <butil/logging.h>
#include <gflags/gflags.h>

#include "echo_service.h"

DEFINE_bool(echo_attachment, true, "Echo attachment as well");
DEFINE_int32(port, 8000, "TCP Port of this server");
DEFINE_string(listen_addr,
              "",
              "Server listen address, may be IPV4/IPV6/UDS."
              " If this is set, the flag port will be ignored");
DEFINE_int32(idle_timeout_s,
             -1,
             "Connection will be closed if there is no "
             "read/write operations during the last `idle_timeout_s'");

int main(int argc, char *argv[]) {
    // Parse gflags. We recommend you to use gflags as well.
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // Generally you only need one Server.
    brpc::Server server;

    // Instance of your service.
    pl::brpc::EchoServiceImpl echo_service_impl;

    // Add the service into server. Notice the second parameter, because the
    // service is put on stack, we don't want server to delete it, otherwise
    // use brpc::SERVER_OWNS_SERVICE.
    if (server.AddService(&echo_service_impl, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Fail to add service";
        return -1;
    }

    butil::EndPoint point;
    if (!FLAGS_listen_addr.empty()) {
        if (butil::str2endpoint(FLAGS_listen_addr.c_str(), &point) < 0) {
            LOG(ERROR) << "Invalid listen address:" << FLAGS_listen_addr;
            return -1;
        }
    } else {
        point = butil::EndPoint(butil::IP_ANY, FLAGS_port);
    }
    // Start the server.
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    if (server.Start(point, &options) != 0) {
        LOG(ERROR) << "Fail to start EchoServer";
        return -1;
    }

    // Wait until Ctrl-C is pressed, then Stop() and Join() the server.
    server.RunUntilAskedToQuit();
    return 0;
}
