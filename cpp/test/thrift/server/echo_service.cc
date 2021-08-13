#include "echo_service.h"

namespace echo {

EchoService::EchoService() {}
void EchoService::echo(EchoResponse& response, std::unique_ptr<EchoRequest> request)
{
  response.message = request->message;
}
};  // namespace echo
