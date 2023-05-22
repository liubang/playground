//=====================================================================
//
// test_liburing.cpp -
//
// Created by liubang on 2023/05/22 23:44
// Last Modified: 2023/05/22 23:44
//
//=====================================================================
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace playground::cpp::misc::liburing {

constexpr __u32 MAX_CONNECTIONS = 4096;
constexpr __u32 BACKLOG = 512;
constexpr __u32 MAX_MESSAGE_LEN = 2048;
constexpr __u32 BUFFERS_COUNT = MAX_CONNECTIONS;

enum Status {
  ACCEPT,
  READ,
  WRITE,
  PROV_BUF,
};

struct ConnInfo {
  __u32 fd;
  __u16 type;
  __u16 bid;
};

char bufs[BUFFERS_COUNT][MAX_MESSAGE_LEN] = {{0}};

int32_t group_id = 1337;
void add_accept(io_uring* ring,
                __u32 fd,
                sockaddr* client_addr,
                socklen_t* client_len,
                unsigned flags) {
  io_uring_sqe* sqe = io_uring_get_sqe(ring);
  io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);
  io_uring_sqe_set_flags(sqe, flags);

  ConnInfo conn_i = {
      .fd = fd,
      .type = Status::ACCEPT,
  };
  memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

void add_socket_read(io_uring* ring, __u32 fd, unsigned gid, size_t message_size, unsigned flags) {
  io_uring_sqe* sqe = io_uring_get_sqe(ring);
  io_uring_prep_recv(sqe, fd, nullptr, message_size, 0);
  io_uring_sqe_set_flags(sqe, flags);
  sqe->buf_group = gid;

  ConnInfo conn_i = {
      .fd = fd,
      .type = Status::READ,
  };
  memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

void add_socket_write(io_uring* ring, __u32 fd, __u16 bid, size_t message_size, unsigned flags) {
  io_uring_sqe* sqe = io_uring_get_sqe(ring);
  io_uring_prep_send(sqe, fd, &bufs[bid], message_size, 0);
  io_uring_sqe_set_flags(sqe, flags);

  ConnInfo conn_i = {
      .fd = fd,
      .type = Status::WRITE,
      .bid = bid,
  };
  memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

void add_provide_buf(io_uring* ring, __u16 bid, unsigned gid) {
  io_uring_sqe* sqe = io_uring_get_sqe(ring);
  io_uring_prep_provide_buffers(sqe, bufs[bid], MAX_MESSAGE_LEN, 1, gid, bid);

  ConnInfo conn_i = {
      .fd = 0,
      .type = Status::PROV_BUF,
  };
  memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

}  // namespace playground::cpp::misc::liburing

int main(int argc, char* argv[]) {
  if (argc < 2) {
    ::printf("Please give a port number: ./%s [port]\n", argv[0]);
    return 0;
  }

  int portno = strtol(argv[1], nullptr, 10);
  sockaddr_in serv_addr, client_addr;
  socklen_t client_len = sizeof(client_addr);

  __u32 sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  const __u32 val = 1;
  setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(portno);
  serv_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sock_listen_fd, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    ::perror("Error binding socket...\n");
    return 1;
  }

  if (listen(sock_listen_fd, playground::cpp::misc::liburing::BACKLOG) < 0) {
    ::perror("Error listening on socket...\n");
    return 1;
  }
  ::printf("io uring echo server listening for connections on port: %d\n", portno);

  io_uring_params params;
  io_uring ring;
  memset(&params, 0, sizeof(params));

  if (io_uring_queue_init_params(2048, &ring, &params) < 0) {
    ::perror("io uring init failed...\n");
    return 1;
  }

  if ((params.features & IORING_FEAT_FAST_POLL) == 0u) {
    ::printf("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
    return 0;
  }

  io_uring_probe* probe;
  probe = io_uring_get_probe_ring(&ring);
  if ((probe == nullptr) || (io_uring_opcode_supported(probe, IORING_OP_PROVIDE_BUFFERS) == 0)) {
    ::printf("Buffer select not supported, skipping...\n");
    return 0;
  }
  free(probe);

  io_uring_sqe* sqe;
  io_uring_cqe* cqe;

  sqe = io_uring_get_sqe(&ring);
  io_uring_prep_provide_buffers(
      sqe, playground::cpp::misc::liburing::bufs, playground::cpp::misc::liburing::MAX_MESSAGE_LEN,
      playground::cpp::misc::liburing::BUFFERS_COUNT, playground::cpp::misc::liburing::group_id, 0);

  io_uring_submit(&ring);
  io_uring_wait_cqe(&ring, &cqe);
  if (cqe->res < 0) {
    ::printf("cqe->res = %d\n", cqe->res);
    return 1;
  }
  io_uring_cqe_seen(&ring, cqe);

  playground::cpp::misc::liburing::add_accept(&ring, sock_listen_fd, (sockaddr*)&client_addr,
                                              &client_len, 0);

  for (;;) {
    io_uring_submit_and_wait(&ring, 1);
    io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring, head, cqe) {
      ++count;
      playground::cpp::misc::liburing::ConnInfo conn_i;
      memcpy(&conn_i, &cqe->user_data, sizeof(conn_i));

      __u32 type = conn_i.type;
      if (cqe->res == -ENOBUFS) {
        ::fprintf(stdout,
                  "bufs in automatic buffer selection empty, this should not "
                  "happen...\n");
        ::fflush(stdout);
        return 1;
      }
      if (type == playground::cpp::misc::liburing::Status::PROV_BUF) {
        if (cqe->res < 0) {
          ::printf("cqe->res = %d\n", cqe->res);
          return 1;
        }
      } else if (type == playground::cpp::misc::liburing::Status::ACCEPT) {
        __u32 sock_conn_fd = cqe->res;
        if (sock_conn_fd >= 0) {
          playground::cpp::misc::liburing::add_socket_read(
              &ring, sock_conn_fd, playground::cpp::misc::liburing::group_id,
              playground::cpp::misc::liburing::MAX_MESSAGE_LEN, IOSQE_BUFFER_SELECT);
        }
        playground::cpp::misc::liburing::add_accept(&ring, sock_listen_fd, (sockaddr*)&client_addr,
                                                    &client_len, 0);
      } else if (type == playground::cpp::misc::liburing::Status::READ) {
        size_t bytes_read = cqe->res;
        if (cqe->res <= 0) {
          shutdown(conn_i.fd, SHUT_RDWR);
        } else {
          __u32 bid = cqe->flags >> 16;
          playground::cpp::misc::liburing::add_socket_write(&ring, conn_i.fd, bid, bytes_read, 0);
        }
      } else if (type == playground::cpp::misc::liburing::Status::WRITE) {
        playground::cpp::misc::liburing::add_provide_buf(&ring, conn_i.bid,
                                                         playground::cpp::misc::liburing::group_id);
        playground::cpp::misc::liburing::add_socket_read(
            &ring, conn_i.fd, playground::cpp::misc::liburing::group_id,
            playground::cpp::misc::liburing::MAX_MESSAGE_LEN, IOSQE_BUFFER_SELECT);
      }
    }

    io_uring_cq_advance(&ring, count);
  }

  return 0;
}
