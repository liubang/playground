#pragma once

#ifdef __linux__
#include "cpp/pl/test/brpc/02-echo/echo.pb.h"

namespace pl::brpc {
class EchoServiceImpl : public EchoService {
public:
    void Echo(::google::protobuf::RpcController* cntl_base,
              const ::pl::brpc::EchoRequest* request,
              ::pl::brpc::EchoResponse* response,
              ::google::protobuf::Closure* done) override;
};
} // namespace pl::brpc
#endif // __linux__
