#include "xcdn_server.h"
#include <cstring>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

static XServer *g_server = nullptr;

extern "C" void handle_signal(int) {
  if (g_server)
    g_server->stop();
}

XServer::XServer(int port, const std::string &cache_dir)
    : port_(port), running_(false), server_fd_(-1) {
  g_server = this;
  struct sigaction sa{};
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);

  server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    std::cerr << "socket() failed\n";
    exit(1);
  }

  int opt = 1;
  setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  int fastopen = 3;
  setsockopt(server_fd_, IPPROTO_TCP, TCP_FASTOPEN, &fastopen,
             sizeof(fastopen));

  struct sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port_);

  if (bind(server_fd_, (struct sockaddr *)&address, sizeof(address)) < 0) {
    exit(1);
  }
  if (listen(server_fd_, 4096) < 0) {
    exit(1);
  }
  cache_.loadDirectory(cache_dir);
}

XServer::~XServer() {
  stop();
  if (server_fd_ >= 0)
    close(server_fd_);
}

void XServer::start() {
  running_ = true;
  unsigned int n = std::thread::hardware_concurrency();
  for (unsigned int i = 0; i < n; ++i)
    workers_.emplace_back(&XServer::workerThread, this, i);
}

void XServer::stop() {
  running_ = false;
  for (auto &t : workers_)
    if (t.joinable())
      t.join();
}

void XServer::workerThread(int core_id) {
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(core_id, &cs);
  pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

  struct io_uring ring{};
  struct io_uring_params params{};
  params.flags = IORING_SETUP_SQPOLL;
  params.sq_thread_idle = 2000;

  if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0) {
    std::cerr << "[XCDN] SQPOLL init failed, falling back\n";
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, IORING_SETUP_SINGLE_ISSUER) <
        0)
      return;
  }

  auto pool_ptr = std::make_unique<ConnInfo[]>(MAX_CONNECTIONS);
  ConnInfo *pool = pool_ptr.get();

  std::vector<int> free_indexes;
  free_indexes.reserve(MAX_CONNECTIONS);
  for (int i = 0; i < MAX_CONNECTIONS; ++i) {
    pool[i].fd = -1;
    free_indexes.push_back(i);
  }

  XHttpParser parser;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (sqe) {
    io_uring_prep_multishot_accept(sqe, server_fd_, nullptr, nullptr, 0);
    io_uring_sqe_set_data64(sqe, (uint64_t)(MAX_CONNECTIONS));
    io_uring_submit(&ring);
  }

  while (running_) {
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(&ring, &cqe) < 0)
      continue;

    uint64_t user_data = io_uring_cqe_get_data64(cqe);
    unsigned int flags = cqe->flags;
    int res = cqe->res;
    io_uring_cqe_seen(&ring, cqe);

    if (flags & IORING_CQE_F_NOTIF)
      continue;

    if (res < 0) {
      if (user_data == (uint64_t)MAX_CONNECTIONS) {
        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          io_uring_prep_multishot_accept(sqe, server_fd_, nullptr, nullptr, 0);
          io_uring_sqe_set_data64(sqe, (uint64_t)(MAX_CONNECTIONS));
          io_uring_submit(&ring);
        }
      } else {
        ConnInfo *info = &pool[user_data];
        if (info->fd >= 0)
          close(info->fd);
        info->fd = -1;
        free_indexes.push_back((int)user_data);
      }
      continue;
    }

    // New connection
    if (user_data == (uint64_t)MAX_CONNECTIONS) {
      int client_fd = res;
      if (free_indexes.empty()) {
        close(client_fd);
      } else {
        int idx = free_indexes.back();
        free_indexes.pop_back();
        ConnInfo *ci = &pool[idx];
        ci->fd = client_fd;
        ci->state = ConnInfo::READ;
        ci->bytes_sent = 0;
        ci->file_data = nullptr;
        ci->header_data = nullptr;
        ci->refs = 0;

        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        int busy_poll = 50;
        setsockopt(client_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll,
                   sizeof(busy_poll));

        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          io_uring_prep_read(sqe, client_fd, ci->buffer, sizeof(ci->buffer), 0);
          io_uring_sqe_set_data64(sqe, (uint64_t)idx);
          io_uring_submit(&ring);
        }
      }
      continue;
    }

    ConnInfo *info = &pool[user_data];
    int idx = (int)user_data;

    switch (info->state) {

    case ConnInfo::READ: {
      if (res == 0) {
        if (info->fd >= 0)
          close(info->fd);
        info->fd = -1;
        free_indexes.push_back(idx);
        break;
      }

      HttpRequest req;
      if (!parser.parse(info->buffer, res, req)) {
        if (info->fd >= 0)
          close(info->fd);
        info->fd = -1;
        free_indexes.push_back(idx);
        break;
      }

      std::string_view path_sv(req.path);
      if (path_sv.empty() || path_sv == "/") {
        path_sv = "index.html";
      } else if (path_sv[0] == '/') {
        path_sv.remove_prefix(1);
      }

      const char *data_ptr = nullptr;
      size_t file_size = 0;
      const char *header_ptr = nullptr;
      size_t header_size = 0;
      int buf_idx = -1;

      if (!cache_.get(path_sv, data_ptr, file_size, header_ptr, header_size,
                      buf_idx)) {
        static const char *nf =
            "HTTP/1.1 404 Not Found\r\nContent-Length: "
            "0\r\nConnection: close\r\n\r\n";
        size_t len = strlen(nf);
        memcpy(info->buffer, nf, len);
        info->state = ConnInfo::WRITE_RAW;
        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          io_uring_prep_send(sqe, info->fd, info->buffer, len, 0);
          io_uring_sqe_set_data64(sqe, (uint64_t)idx);
          io_uring_submit(&ring);
        }
        break;
      }

      info->file_data = data_ptr;
      info->file_size = file_size;
      info->header_data = header_ptr;
      info->header_len = header_size;
      info->bytes_sent = 0;
      info->file_buf_idx = buf_idx;

      struct iovec iov[2];
      iov[0].iov_base = (void *)header_ptr;
      iov[0].iov_len = header_size;
      iov[1].iov_base = (void *)data_ptr;
      iov[1].iov_len = file_size;

      struct msghdr msg{};
      msg.msg_iov = iov;
      msg.msg_iovlen = 2;

      info->state = ConnInfo::WRITE_BODY;
      info->bytes_sent = header_size + file_size;
      sqe = io_uring_get_sqe(&ring);
      if (sqe) {
        io_uring_prep_sendmsg(sqe, info->fd, &msg, 0);
        io_uring_sqe_set_data64(sqe, (uint64_t)idx);
        io_uring_submit(&ring);
      }
      break;
    }

    case ConnInfo::WRITE_BODY: {
      size_t total = info->header_len + info->file_size;
      if (res < (int)total) {
        if (info->fd >= 0)
          close(info->fd);
        info->fd = -1;
        free_indexes.push_back(idx);
        break;
      }

      info->state = ConnInfo::READ;
      sqe = io_uring_get_sqe(&ring);
      if (sqe) {
        io_uring_prep_read(sqe, info->fd, info->buffer, sizeof(info->buffer),
                           0);
        io_uring_sqe_set_data64(sqe, (uint64_t)idx);
        io_uring_submit(&ring);
      }
      break;
    }

    case ConnInfo::WRITE_RAW: {
      if (info->fd >= 0)
        close(info->fd);
      info->fd = -1;
      free_indexes.push_back(idx);
      break;
    }

    default:
      break;
    }
  }

  for (int i = 0; i < MAX_CONNECTIONS; ++i) {
    if (pool[i].fd >= 0)
      close(pool[i].fd);
  }
  io_uring_queue_exit(&ring);
}
