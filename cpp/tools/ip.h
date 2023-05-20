#pragma once

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <string>

namespace playground::cpp::tools {

inline std::optional<std::string> getLocalIp() {
  constexpr char REMOTE_ADDRESS[] = "10.255.255.255";

  struct sockaddr_in remote_server;
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    return std::nullopt;
  }

  memset(&remote_server, 0, sizeof(remote_server));
  remote_server.sin_family = AF_INET;
  remote_server.sin_addr.s_addr = inet_addr(REMOTE_ADDRESS);
  remote_server.sin_port = htons(22);
  int err =
      ::connect(sock, reinterpret_cast<const struct sockaddr*>(&remote_server),
                sizeof(remote_server));
  if (err < 0) {
    return std::nullopt;
  }

  struct sockaddr_in local_addr;
  socklen_t local_addr_len = sizeof(local_addr);
  err = getsockname(sock, reinterpret_cast<struct sockaddr*>(&local_addr),
                    &local_addr_len);
  char buffer[INET_ADDRSTRLEN];
  std::string local_ip;
  const char* p =
      inet_ntop(AF_INET, &local_addr.sin_addr, buffer, INET_ADDRSTRLEN);
  if (p != nullptr) {
    local_ip = std::string(buffer);
  }
  ::close(sock);
  return local_ip;
}

}  // namespace playground::cpp::tools
