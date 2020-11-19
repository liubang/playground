#include <folly/init/Init.h>
#include <proxygen/httpserver/HTTPServerOptions.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/transport/core/ThriftProcessor.h>
#include <thrift/lib/cpp2/transport/http2/common/HTTP2RoutingHandler.h>
#include <gflags/gflags.h>

#include "echo_service.h"


using apache::thrift::HTTP2RoutingHandler;
using apache::thrift::ThriftServer;
using apache::thrift::ThriftServerAsyncProcessorFactory;
using proxygen::HTTPServerOptions;

std::unique_ptr<HTTP2RoutingHandler> createHTTP2RoutingHandler(
    std::shared_ptr<ThriftServer> server) {
  auto h2_option = std::make_unique<HTTPServerOptions>();
  h2_option->threads = static_cast<size_t>(server->getNumIOWorkerThreads());
  h2_option->idleTimeout = server->getIdleTimeout();
  h2_option->shutdownOn = {SIGINT, SIGTERM};
  return std::make_unique<HTTP2RoutingHandler>(
      std::move(h2_option), server->getThriftProcessor(), *server);
}

template <class ServiceHandler>
std::shared_ptr<ThriftServer> newServer(int32_t port) {
  auto handler = std::make_shared<ServiceHandler>();
  auto proc_factory =
      std::make_shared<ThriftServerAsyncProcessorFactory<ServiceHandler>>(
          handler);
  auto server = std::make_shared<ThriftServer>();
  server->setPort(port);
  server->setProcessorFactory(proc_factory);
  server->addRoutingHandler(createHTTP2RoutingHandler(server));
  return server;
}

int main(int argc, char* argv[]) {
  folly::init(&argc, &argv);
  auto echo_server = newServer<echo::EchoService>(1234);
  echo_server->serve();
  return 0;
}
