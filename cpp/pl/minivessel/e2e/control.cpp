#include <brpc/channel.h>
#include <brpc/controller.h>
#include <chrono>
#include <cstdlib>
#include <gflags/gflags.h>
#include <iostream>
#include <string>
#include <thread>

#include "cpp/pl/minivessel/e2e/minivessel_e2e.pb.h"

DEFINE_string(address, "127.0.0.1:9300", "counter replica address");
DEFINE_string(command,
              "status",
              "wait|health|status|promote|demote|add|checkpoint|poll|pause-polling|resume-polling|"
              "stop|sharedlog-status");
DEFINE_int64(delta, 0, "counter delta for add");
DEFINE_string(request_id, "", "request ID for add");
DEFINE_uint64(timeout_ms, 30000, "wait timeout");

namespace {
namespace proto = pl::minivessel::e2e::protocol;

const char* CodeName(int code) {
    switch (code) {
        case 10:
            return "ABORTED";
        default:
            return "ERROR";
    }
}

void Print(const proto::CounterStatus& status) {
    std::cout << "replica=" << status.replica_id() << " role=" << status.role()
              << " value=" << status.value() << " applied_lrsn=" << status.applied_lrsn()
              << " writer_epoch=" << status.writer_epoch()
              << " resources=" << (status.primary_resources_active() ? "active" : "inactive")
              << " transitions=" << status.lifecycle_transitions()
              << " last_error=" << status.last_error() << '\n';
}

bool Invoke(proto::CounterService_Stub* stub, const std::string& command) {
    brpc::Controller controller;
    proto::Empty empty;
    proto::CounterStatus status;
    proto::CommitResponse commit;
    if (command == "status" || command == "health" || command == "wait")
        stub->Status(&controller, &empty, &status, nullptr);
    else if (command == "promote")
        stub->Promote(&controller, &empty, &status, nullptr);
    else if (command == "demote")
        stub->Demote(&controller, &empty, &status, nullptr);
    else if (command == "poll")
        stub->Poll(&controller, &empty, &status, nullptr);
    else if (command == "pause-polling")
        stub->PausePolling(&controller, &empty, &status, nullptr);
    else if (command == "resume-polling")
        stub->ResumePolling(&controller, &empty, &status, nullptr);
    else if (command == "stop")
        stub->Stop(&controller, &empty, &status, nullptr);
    else if (command == "checkpoint")
        stub->Checkpoint(&controller, &empty, &commit, nullptr);
    else if (command == "add") {
        proto::AddRequest request;
        request.set_delta(FLAGS_delta);
        request.set_request_id(FLAGS_request_id.empty() ? "cli-add" : FLAGS_request_id);
        stub->Add(&controller, &request, &commit, nullptr);
    } else {
        std::cerr << "unknown command: " << command << '\n';
        return false;
    }
    if (controller.Failed()) {
        std::cerr << controller.ErrorText() << '\n';
        return false;
    }
    if (command == "add" || command == "checkpoint") {
        if (commit.status().code() != 0) {
            std::cerr << CodeName(commit.status().code()) << ": " << commit.status().message()
                      << '\n';
            return false;
        }
        std::cout << "lrsn=" << commit.lrsn() << '\n';
    } else {
        if (status.status().code() != 0) {
            std::cerr << CodeName(status.status().code()) << ": " << status.status().message()
                      << '\n';
            return false;
        }
        Print(status);
        if (command == "health" &&
            (status.role() == "failed" || status.role() == "stopped" ||
             !status.last_error().empty())) {
            std::cerr << "replica is not healthy: role=" << status.role()
                      << " last_error=" << status.last_error() << '\n';
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.timeout_ms = 2000;
    options.max_retry = 0;
    if (channel.Init(FLAGS_address.c_str(), &options) != 0)
        return 2;
    if (FLAGS_command == "sharedlog-status") {
        proto::SharedLogService_Stub sharedlog(&channel);
        proto::Group request;
        request.set_group_id("counter");
        request.set_incarnation(1);
        proto::StatusResponse response;
        brpc::Controller controller;
        sharedlog.Status(&controller, &request, &response, nullptr);
        if (controller.Failed() || response.status().code() != 0) {
            std::cerr << (controller.Failed() ? controller.ErrorText()
                                              : response.status().message())
                      << '\n';
            return 1;
        }
        std::cout << "durable_lrsn=" << response.durable_lrsn()
                  << " object_count=" << response.object_count()
                  << " writer_epoch=" << response.writer_epoch()
                  << " writer_owner=" << response.writer_owner() << '\n';
        return 0;
    }
    proto::CounterService_Stub stub(&channel);
    if (FLAGS_command != "wait")
        return Invoke(&stub, FLAGS_command) ? 0 : 1;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(FLAGS_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (Invoke(&stub, "wait"))
            return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::cerr << "timed out waiting for " << FLAGS_address << '\n';
    return 1;
}
