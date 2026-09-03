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

  struct io_uring ring;
  if (io_uring_queue_init(QUEUE_DEPTH, &ring, IORING_SETUP_SINGLE_ISSUER) < 0)
    return;

  // СЕКРЕТ ПОБЕДЫ НАД СЕГФОЛТОМ: Выделяем сырой массив в куче!
  // Стек потока девственно чист, а в куче создается ровно 1 ГБ готовой памяти
  auto pool_ptr = std::make_unique<ConnInfo[]>(MAX_CONNECTIONS);
  ConnInfo *pool = pool_ptr.get();

  std::vector<int> free_indexes;
  free_indexes.reserve(MAX_CONNECTIONS);
  for (int i = 0; i < MAX_CONNECTIONS; ++i) {
    pool[i].fd = -1;
    free_indexes.push_back(i);
  }

  XHttpParser parser;
  ConnInfo accept_info{};
  accept_info.fd = server_fd_;
  accept_info.state = ConnInfo::ACCEPT;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (sqe) {
    io_uring_prep_accept(sqe, server_fd_, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe, &accept_info);
    io_uring_submit(&ring);
  }

  while (running_) {
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(&ring, &cqe) < 0)
      continue;

    ConnInfo *info = static_cast<ConnInfo *>(io_uring_cqe_get_data(cqe));
    unsigned int flags = cqe->flags;
    int res = cqe->res;
    io_uring_cqe_seen(&ring, cqe);

    // Обработка ядерной нотификации Zero-Copy
    if (flags & IORING_CQE_F_NOTIF) {
      if (info) {
        info->refs--;
        if (info->refs == 0 && info->state == ConnInfo::WAIT_NOTIF) {
          info->state = ConnInfo::READ;
          sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_read(sqe, info->fd, info->buffer,
                               sizeof(info->buffer), 0);
            io_uring_sqe_set_data(sqe, info);
            io_uring_submit(&ring);
          }
        }
      }
      continue;
    }

    if (res < 0) {
      if (info && info->state != ConnInfo::ACCEPT) {
        if (info->fd >= 0) {
          close(info->fd);
          info->fd = -1;
        }
        int idx = info - pool;
        if (idx >= 0 && idx < MAX_CONNECTIONS)
          free_indexes.push_back(idx);
      } else if (info && info->state == ConnInfo::ACCEPT) {
        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          io_uring_prep_accept(sqe, server_fd_, nullptr, nullptr, 0);
          io_uring_sqe_set_data(sqe, &accept_info);
          io_uring_submit(&ring);
        }
      }
      continue;
    }

    switch (info->state) {
    case ConnInfo::ACCEPT: {
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
        ci->refs = 0;

        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                   sizeof(int));

        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          io_uring_prep_read(sqe, client_fd, ci->buffer, sizeof(ci->buffer), 0);
          io_uring_sqe_set_data(sqe, ci);
        }
      }
      sqe = io_uring_get_sqe(&ring);
      if (sqe) {
        io_uring_prep_accept(sqe, server_fd_, nullptr, nullptr, 0);
        io_uring_sqe_set_data(sqe, &accept_info);
        io_uring_submit(&ring);
      }
      break;
    }

    case ConnInfo::READ: {
      if (res == 0) {
        if (info->fd >= 0) {
          close(info->fd);
          info->fd = -1;
        }
        int idx = info - pool;
        free_indexes.push_back(idx);
        break;
      }

      HttpRequest req;
      if (!parser.parse(info->buffer, res, req)) {
        if (info->fd >= 0) {
          close(info->fd);
          info->fd = -1;
        }
        int idx = info - pool;
        free_indexes.push_back(idx);
        break;
      }

      std::string file_path(req.path);
      if (file_path.empty() || file_path == "/") {
        file_path = "index.html";
      } else if (file_path[0] == '/') {
        file_path = file_path.substr(1);
      }

      const char *data_ptr = nullptr;
      size_t file_size = 0;
      std::string mime;
      int buf_idx = -1;

      if (!cache_.get(file_path, data_ptr, file_size, mime, buf_idx)) {
        static const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: "
                                "0\r\nConnection: close\r\n\r\n";
        size_t len = strlen(nf);
        memcpy(info->buffer, nf, len);
        info->state = ConnInfo::WRITE_RAW;
        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          io_uring_prep_send(sqe, info->fd, info->buffer, len, 0);
          io_uring_sqe_set_data(sqe, info);
          io_uring_submit(&ring);
        }
        break;
      }

      int header_len = snprintf(
          info->header_buf, sizeof(info->header_buf),
          "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
          "Connection: keep-alive\r\nCache-Control: public, "
          "max-age=31536000\r\n"
          "Server: XCDN-Killer\r\n\r\n",
          mime.c_str(), file_size);

      info->file_data = data_ptr;
      info->file_size = file_size;
      info->bytes_sent = 0;
      info->header_len = header_len;
      info->file_buf_idx = buf_idx;

      struct io_uring_sqe *sqe1 = io_uring_get_sqe(&ring);
      struct io_uring_sqe *sqe2 = io_uring_get_sqe(&ring);

      if (!sqe1 || !sqe2) {
        info->state = ConnInfo::READ;
        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          io_uring_prep_read(sqe, info->fd, info->buffer, sizeof(info->buffer),
                             0);
          io_uring_sqe_set_data(sqe, info);
          io_uring_submit(&ring);
        }
        break;
      }

      // Шаг 1: Заголовки (Линкуем операцию)
      io_uring_prep_send(sqe1, info->fd, info->header_buf, header_len, 0);
      sqe1->flags |= IOSQE_IO_LINK;
      io_uring_sqe_set_data(sqe1, info);

      // Шаг 2: Тело файла через стабильный Zero-Copy
      info->state = ConnInfo::WRITE_BODY;
      info->refs++;
      io_uring_prep_send_zc(sqe2, info->fd, info->file_data, file_size, 0, 0);
      io_uring_sqe_set_data(sqe2, info);

      io_uring_submit(&ring);
      break;
    }

    case ConnInfo::WRITE_BODY: {
      int actual_body_res =
          (info->bytes_sent == 0) ? (res - info->header_len) : res;
      if (actual_body_res <= 0) {
        if (info->fd >= 0) {
          close(info->fd);
          info->fd = -1;
        }
        int idx = info - pool;
        free_indexes.push_back(idx);
        break;
      }

      if (info->bytes_sent == 0)
        info->bytes_sent += actual_body_res;
      else
        info->bytes_sent += res;

      if (info->bytes_sent < info->file_size) {
        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          info->refs++;
          io_uring_prep_send_zc(sqe, info->fd,
                                info->file_data + info->bytes_sent,
                                info->file_size - info->bytes_sent, 0, 0);
          io_uring_sqe_set_data(sqe, info);
          io_uring_submit(&ring);
        }
      } else {
        info->state = ConnInfo::WAIT_NOTIF;
        if (info->refs == 0) {
          info->state = ConnInfo::READ;
          sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_read(sqe, info->fd, info->buffer,
                               sizeof(info->buffer), 0);
            io_uring_sqe_set_data(sqe, info);
            io_uring_submit(&ring);
          }
        }
      }
      break;
    }
    case ConnInfo::WRITE_RAW: {
      if (info->fd >= 0) {
        close(info->fd);
        info->fd = -1;
      }
      int idx = info - pool;
      free_indexes.push_back(idx);
      break;
    }
    }
  }
  for (int i = 0; i < MAX_CONNECTIONS; ++i) {
    if (pool[i].fd >= 0)
      close(pool[i].fd);
  }
  io_uring_queue_exit(&ring);
}