#include "test/thrift/if/gen-cpp2/EchoService.h"

#include <folly/SocketAddress.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp2/transport/core/testutil/ServerConfigsMock.h>
#include <thrift/perf/cpp2/util/Util.h>

DEFINE_string(host, "::1", "EchoServer host");
DEFINE_int32(port, 1234, "EchoServer port");
DEFINE_string(transport, "header", "Transport to use: header, rsocket, http2");

int main(int argc, char* argv[])
{
  FLAGS_logtostderr = true;
  folly::init(&argc, &argv);
  folly::EventBase evb;

  // Create a thrift client
  auto addr = folly::SocketAddress(FLAGS_host, FLAGS_port);
  auto ct = std::make_shared<ConnectionThread<echo::EchoServiceAsyncClient>>();
  auto client = ct->newSyncClient(addr, FLAGS_transport);
  // For header transport
  if (FLAGS_transport == "header") {
    client = newHeaderClient<echo::EchoServiceAsyncClient>(&evb, addr);
  }

  // Prepare thrift request
  echo::EchoRequest request;
  request.set_message("Ping this back");
  echo::EchoResponse response;

  // Get an echo'd message
  try {
    client->sync_echo(response, request);
    LOG(INFO) << response.get_message();
  } catch (apache::thrift::transport::TTransportException& ex) {
    LOG(ERROR) << "Request failed " << ex.what();
  }
  return 0;
}
