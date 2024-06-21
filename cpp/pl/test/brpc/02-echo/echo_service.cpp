//=====================================================================
//
// echo_service.cpp -
//
// Created by liubang on 2023/08/18 01:01
// Last Modified: 2023/08/18 01:01
//
//=====================================================================
#include "echo_service.h"

#include <brpc/controller.h>

void pl::brpc::EchoServiceImpl::Echo(::google::protobuf::RpcController* cntl_base,
                                     const ::pl::brpc::EchoRequest* request,
                                     ::pl::brpc::EchoResponse* response,
                                     ::google::protobuf::Closure* done) {
    ::brpc::ClosureGuard done_guard(done);
    [[__maybe_unused__]] auto* cntl = static_cast<::brpc::Controller*>(cntl_base);
    response->set_message(request->message());
}
