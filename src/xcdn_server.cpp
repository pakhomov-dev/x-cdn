#include "xcdn_server.h"
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

static constexpr size_t SPLICE_THRESHOLD = 256 * 1024;

static void release_conn(ConnInfo *info) {
  if (info->fd >= 0) {
    close(info->fd);
    info->fd = -1;
  }
  if (info->pipe_rd >= 0) {
    close(info->pipe_rd);
    info->pipe_rd = -1;
  }
  if (info->pipe_wr >= 0) {
    close(info->pipe_wr);
    info->pipe_wr = -1;
  }
}

static bool submit_splice_step(struct io_uring *ring, ConnInfo *info,
                                int idx) {
  size_t sock_rem = info->file_size - info->body_sent;
  if (sock_rem == 0)
    return true;

  size_t file_rem = info->file_size - info->file_off;

  if (file_rem > 0) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
      return false;
    io_uring_prep_splice(sqe, info->file_fd, (int64_t)info->file_off,
                         info->pipe_wr, -1, (unsigned)file_rem, 0);
    io_uring_sqe_set_data64(sqe, (uint64_t)idx);
    sqe->flags |= IOSQE_IO_LINK;

    sqe = io_uring_get_sqe(ring);
    if (!sqe)
      return false;
    io_uring_prep_splice(sqe, info->pipe_rd, -1, info->fd, -1,
                         (unsigned)sock_rem, 0);
    io_uring_sqe_set_data64(sqe, (uint64_t)idx);
    info->splice_phase = 1;
  } else {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
      return false;
    io_uring_prep_splice(sqe, info->pipe_rd, -1, info->fd, -1,
                         (unsigned)sock_rem, 0);
    io_uring_sqe_set_data64(sqe, (uint64_t)idx);
    info->splice_phase = 2;
  }
  io_uring_submit(ring);
  return false;
}

static bool prep_read_next(struct io_uring *ring, ConnInfo *info, int idx) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe)
    return false;
  io_uring_prep_read(sqe, info->fd, info->buffer, sizeof(info->buffer), 0);
  io_uring_sqe_set_data64(sqe, (uint64_t)idx);
  io_uring_submit(ring);
  return true;
}

#ifdef XCDN_TLS_ENABLED
#include <openssl/err.h>
#endif

static XServer *g_server = nullptr;

extern "C" void handle_signal(int) {
  if (g_server)
    g_server->stop();
}

XServer::XServer(const ServerConfig &cfg)
    : cfg_(cfg), running_(false), server_fd_(-1) {
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
  address.sin_port = htons(cfg_.port);

  if (bind(server_fd_, (struct sockaddr *)&address, sizeof(address)) < 0) {
    std::cerr << "bind() failed on port " << cfg_.port << "\n";
    exit(1);
  }
  if (listen(server_fd_, cfg_.listen_backlog) < 0) {
    std::cerr << "listen() failed on port " << cfg_.port << "\n";
    exit(1);
  }
  cache_.loadDirectory(cfg_.root, cfg_.cache_max_age, cfg_.server_name);

#ifdef XCDN_TLS_ENABLED
  if (cfg_.tls_enabled) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    if (!tls_.init(cfg_.tls_cert, cfg_.tls_key)) {
      std::cerr << "[XCDN] TLS init failed\n";
      exit(1);
    }
  }
#endif
}

XServer::~XServer() {
  stop();
  if (server_fd_ >= 0)
    close(server_fd_);
}

void XServer::start() {
  running_ = true;
  unsigned int n = (cfg_.workers > 0)
                       ? static_cast<unsigned int>(cfg_.workers)
                       : std::thread::hardware_concurrency();
  std::cerr << "[XCDN] Starting " << n << " workers on port " << cfg_.port
            << "\n";
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
  memset(&params, 0, sizeof(params));

  bool use_sqpoll = (cfg_.uring_mode == IoUringMode::SQPOLL);
  if (use_sqpoll) {
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;
  }

  if (io_uring_queue_init_params(cfg_.queue_depth, &ring, &params) < 0) {
    if (io_uring_queue_init(cfg_.queue_depth, &ring, 0) < 0)
      return;
    use_sqpoll = false;
  }

  auto pool_ptr = std::make_unique<ConnInfo[]>(cfg_.max_connections);
  ConnInfo *pool = pool_ptr.get();

  std::vector<int> free_indexes;
  free_indexes.reserve(cfg_.max_connections);
  for (int i = 0; i < cfg_.max_connections; ++i) {
    pool[i].fd = -1;
    free_indexes.push_back(i);
  }

  XHttpParser parser;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (sqe) {
    io_uring_prep_multishot_accept(sqe, server_fd_, nullptr, nullptr, 0);
    io_uring_sqe_set_data64(sqe, (uint64_t)(cfg_.max_connections));
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
      if (user_data == (uint64_t)cfg_.max_connections) {
        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          io_uring_prep_multishot_accept(sqe, server_fd_, nullptr, nullptr, 0);
          io_uring_sqe_set_data64(sqe, (uint64_t)(cfg_.max_connections));
          io_uring_submit(&ring);
        }
      } else {
        ConnInfo *info = &pool[user_data];
        release_conn(info);
        free_indexes.push_back((int)user_data);
      }
      continue;
    }

    if (user_data == (uint64_t)cfg_.max_connections) {
      int client_fd = res;
      if (free_indexes.empty()) {
        close(client_fd);
      } else if (
#ifdef XCDN_TLS_ENABLED
          cfg_.tls_enabled && tls_.enabled()
#else
          false
#endif
      ) {
#ifdef XCDN_TLS_ENABLED
        std::thread(&XServer::handleTlsConnection, this, client_fd).detach();
#endif
      } else {
        metrics_.active_connections++;
        int idx = free_indexes.back();
        free_indexes.pop_back();
        ConnInfo *ci = &pool[idx];
        ci->fd = client_fd;
        ci->state = ConnInfo::READ;
        ci->bytes_sent = 0;
        ci->send_ptr = nullptr;
        ci->send_total = 0;
        ci->is_large_file = false;
        ci->file_data = nullptr;
        ci->header_data = nullptr;
        ci->refs = 0;
        ci->file_fd = -1;
        ci->pipe_rd = -1;
        ci->pipe_wr = -1;
        ci->body_sent = 0;
        ci->file_off = 0;
        ci->splice_phase = 0;

        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        int sndbuf = 1 << 20;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        if (cfg_.busy_poll_us > 0) {
          int busy_poll = cfg_.busy_poll_us;
          setsockopt(client_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll,
                     sizeof(busy_poll));
        }

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
          metrics_.active_connections--;
          release_conn(info);
          free_indexes.push_back(idx);
          break;
        }

        HttpRequest req;
        if (!parser.parse(info->buffer, res, req)) {
          release_conn(info);
          free_indexes.push_back(idx);
          break;
        }

        metrics_.total_requests++;

        std::string_view path_sv(req.path);

        if (cfg_.metrics_enabled && path_sv == cfg_.metrics_path) {
          std::string body = metrics_.format_prometheus();
          char hdr[256];
          int hlen = snprintf(hdr, sizeof(hdr),
                              "HTTP/1.1 200 OK\r\nContent-Type: text/plain; "
                              "version=0.0.4\r\nContent-Length: %zu\r\n"
                              "Connection: keep-alive\r\n\r\n",
                              body.size());
          memcpy(info->buffer, hdr, hlen);
          memcpy(info->buffer + hlen, body.data(), body.size());
          info->state = ConnInfo::WRITE_RAW;
          info->bytes_sent = 0;
          info->send_ptr = info->buffer;
          info->send_total = hlen + body.size();
          info->is_large_file = false;
          sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_send(sqe, info->fd, info->buffer,
                               hlen + body.size(), 0);
            io_uring_sqe_set_data64(sqe, (uint64_t)idx);
            io_uring_submit(&ring);
          }
          break;
        }
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
        int file_fd = -1;

        if (!cache_.get(path_sv, data_ptr, file_size, header_ptr, header_size,
                        buf_idx, file_fd)) {
          metrics_.total_404s++;
          static const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: "
                                  "0\r\nConnection: close\r\n\r\n";
          size_t len = strlen(nf);
          memcpy(info->buffer, nf, len);
          info->state = ConnInfo::WRITE_RAW;
          info->bytes_sent = 0;
          info->send_ptr = info->buffer;
          info->send_total = len;
          info->is_large_file = false;
          sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_send(sqe, info->fd, info->buffer, len, 0);
            io_uring_sqe_set_data64(sqe, (uint64_t)idx);
            io_uring_submit(&ring);
          }
          break;
        }

        metrics_.cache_hits++;
        info->file_data = data_ptr;
        info->file_size = file_size;
        info->header_data = header_ptr;
        info->header_len = header_size;
        info->bytes_sent = 0;
        info->file_buf_idx = buf_idx;
        info->file_fd = file_fd;
        info->body_sent = 0;
        info->file_off = 0;

        size_t total = header_size + file_size;
        info->bytes_sent = 0;
        info->send_total = total;

        if (file_size > SPLICE_THRESHOLD && file_fd >= 0) {
          if (info->pipe_rd < 0) {
            int pfd[2];
            if (pipe(pfd) == 0) {
              int psz = 1 << 20;
              fcntl(pfd[1], F_SETPIPE_SZ, psz);
              info->pipe_rd = pfd[0];
              info->pipe_wr = pfd[1];
            }
          }
          if (info->pipe_rd >= 0) {
            int big = 2 << 20;
            setsockopt(info->fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));

            info->state = ConnInfo::SPLICE_HEADER;
            info->is_large_file = true;
            info->send_ptr = info->header_data;
            info->send_total = header_size;
            info->bytes_sent = 0;
            info->body_sent = 0;
            info->file_off = 0;
            info->splice_phase = 0;

            sqe = io_uring_get_sqe(&ring);
            if (sqe) {
              io_uring_prep_send(sqe, info->fd, info->header_data, header_size,
                                 0);
              io_uring_sqe_set_data64(sqe, (uint64_t)idx);
              sqe->flags |= IOSQE_IO_LINK;

              sqe = io_uring_get_sqe(&ring);
              if (sqe) {
                io_uring_prep_splice(sqe, file_fd, 0, info->pipe_wr, -1,
                                     (unsigned)file_size, 0);
                io_uring_sqe_set_data64(sqe, (uint64_t)idx);
                sqe->flags |= IOSQE_IO_LINK;

                sqe = io_uring_get_sqe(&ring);
                if (sqe) {
                  io_uring_prep_splice(sqe, info->pipe_rd, -1, info->fd, -1,
                                       (unsigned)file_size, 0);
                  io_uring_sqe_set_data64(sqe, (uint64_t)idx);
                  io_uring_submit(&ring);
                }
              }
            }
            break;
          }
        }

        info->state = ConnInfo::WRITE_BODY;
        if (total <= sizeof(info->buffer)) {
          info->is_large_file = false;
          info->send_ptr = info->buffer;
          memcpy(info->buffer, header_ptr, header_size);
          memcpy(info->buffer + header_size, data_ptr, file_size);
          sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_send(sqe, info->fd, info->buffer, total, 0);
            io_uring_sqe_set_data64(sqe, (uint64_t)idx);
            io_uring_submit(&ring);
          }
        } else {
          info->is_large_file = true;
          info->send_ptr = header_ptr;
          sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_send(sqe, info->fd, info->send_ptr, total, 0);
            io_uring_sqe_set_data64(sqe, (uint64_t)idx);
            io_uring_submit(&ring);
          }
        }
        break;
      }

      case ConnInfo::WRITE_BODY: {
        if (res <= 0) {
          metrics_.active_connections--;
          release_conn(info);
          free_indexes.push_back(idx);
          break;
        }
        info->bytes_sent += res;

        if (info->bytes_sent < info->send_total) {
          sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_send(sqe, info->fd,
                               info->send_ptr + info->bytes_sent,
                               info->send_total - info->bytes_sent, 0);
            io_uring_sqe_set_data64(sqe, (uint64_t)idx);
            io_uring_submit(&ring);
          }
          break;
        }

        metrics_.bytes_sent += info->send_total;
        info->state = ConnInfo::READ;
        prep_read_next(&ring, info, idx);
        break;
      }

      case ConnInfo::SPLICE_HEADER: {
        if (res <= 0) {
          metrics_.active_connections--;
          release_conn(info);
          free_indexes.push_back(idx);
          break;
        }
        if (info->splice_phase == 0) {
          info->bytes_sent += res;
          if (info->bytes_sent < info->send_total) {
            sqe = io_uring_get_sqe(&ring);
            if (sqe) {
              io_uring_prep_send(sqe, info->fd,
                                 info->header_data + info->bytes_sent,
                                 info->send_total - info->bytes_sent, 0);
              io_uring_sqe_set_data64(sqe, (uint64_t)idx);
              io_uring_submit(&ring);
            }
            break;
          }
          metrics_.bytes_sent += info->header_len;
          info->state = ConnInfo::SPLICE_BODY;
          info->splice_phase = 1;
          break;
        }
        if (info->splice_phase == 1) {
          info->file_off += res;
          info->splice_phase = 2;
          break;
        }
        info->body_sent += res;
        if (info->body_sent >= info->file_size) {
          int norm = 1 << 20;
          setsockopt(info->fd, SOL_SOCKET, SO_SNDBUF, &norm, sizeof(norm));
          metrics_.bytes_sent += info->file_size;
          info->state = ConnInfo::READ;
          prep_read_next(&ring, info, idx);
          break;
        }
        if (submit_splice_step(&ring, info, idx)) {
          int norm = 1 << 20;
          setsockopt(info->fd, SOL_SOCKET, SO_SNDBUF, &norm, sizeof(norm));
          metrics_.bytes_sent += info->file_size;
          info->state = ConnInfo::READ;
          prep_read_next(&ring, info, idx);
        }
        break;
      }

      case ConnInfo::SPLICE_BODY: {
        if (res <= 0) {
          metrics_.active_connections--;
          release_conn(info);
          free_indexes.push_back(idx);
          break;
        }
        if (info->splice_phase == 1) {
          info->file_off += res;
          info->splice_phase = 2;
          break;
        }
        info->body_sent += res;
        if (info->body_sent >= info->file_size) {
          int norm = 1 << 20;
          setsockopt(info->fd, SOL_SOCKET, SO_SNDBUF, &norm, sizeof(norm));
          metrics_.bytes_sent += info->file_size;
          info->state = ConnInfo::READ;
          prep_read_next(&ring, info, idx);
          break;
        }
        if (submit_splice_step(&ring, info, idx)) {
          int norm = 1 << 20;
          setsockopt(info->fd, SOL_SOCKET, SO_SNDBUF, &norm, sizeof(norm));
          metrics_.bytes_sent += info->file_size;
          info->state = ConnInfo::READ;
          prep_read_next(&ring, info, idx);
        }
        break;
      }

      case ConnInfo::WRITE_RAW: {
        if (res > 0)
          info->bytes_sent += res;
        if (res > 0 && info->bytes_sent < info->send_total) {
          sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_send(sqe, info->fd,
                               info->send_ptr + info->bytes_sent,
                               info->send_total - info->bytes_sent, 0);
            io_uring_sqe_set_data64(sqe, (uint64_t)idx);
            io_uring_submit(&ring);
          }
          break;
        }
        metrics_.active_connections--;
        release_conn(info);
        free_indexes.push_back(idx);
        break;
      }

      default:
        break;
      }
  }

  for (int i = 0; i < cfg_.max_connections; ++i)
    release_conn(&pool[i]);
  io_uring_queue_exit(&ring);
}

#ifdef XCDN_TLS_ENABLED
int XServer::sslRead(ConnInfo *info, char *buf, int len) {
  int n = SSL_read(info->ssl, buf, len);
  if (n <= 0) {
    int err = SSL_get_error(info->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
      return 0;
    return -1;
  }
  return n;
}

int XServer::sslWrite(ConnInfo *info, const char *buf, int len) {
  int total = 0;
  while (total < len) {
    int n = SSL_write(info->ssl, buf + total, len - total);
    if (n <= 0) {
      int err = SSL_get_error(info->ssl, n);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        continue;
      return -1;
    }
    total += n;
  }
  return total;
}

void XServer::handleTlsConnection(int client_fd) {
  metrics_.active_connections++;

  int flag = 1;
  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

  SSL *ssl = tls_.new_ssl(client_fd);
  if (!ssl) {
    close(client_fd);
    metrics_.active_connections--;
    return;
  }

  if (SSL_accept(ssl) <= 0) {
    SSL_shutdown(ssl);
    tls_.free_ssl(ssl);
    close(client_fd);
    metrics_.active_connections--;
    return;
  }

  metrics_.total_requests++;

  char request_buf[8192];
  int n = SSL_read(ssl, request_buf, sizeof(request_buf) - 1);
  if (n <= 0) {
    SSL_shutdown(ssl);
    tls_.free_ssl(ssl);
    close(client_fd);
    metrics_.active_connections--;
    return;
  }
  request_buf[n] = '\0';

  HttpRequest req;
  XHttpParser parser;
  if (!parser.parse(request_buf, n, req)) {
    SSL_shutdown(ssl);
    tls_.free_ssl(ssl);
    close(client_fd);
    metrics_.active_connections--;
    return;
  }

  std::string_view path_sv(req.path);

  if (cfg_.metrics_enabled && path_sv == cfg_.metrics_path) {
    std::string body = metrics_.format_prometheus();
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain; "
                        "version=0.0.4\r\nContent-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        body.size());
    SSL_write(ssl, hdr, hlen);
    SSL_write(ssl, body.data(), body.size());
    SSL_shutdown(ssl);
    tls_.free_ssl(ssl);
    close(client_fd);
    metrics_.active_connections--;
    return;
  }

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
  int file_fd = -1;

  if (!cache_.get(path_sv, data_ptr, file_size, header_ptr, header_size,
                  buf_idx, file_fd)) {
    metrics_.total_404s++;
    static const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: "
                            "0\r\nConnection: close\r\n\r\n";
    SSL_write(ssl, nf, strlen(nf));
    SSL_shutdown(ssl);
    tls_.free_ssl(ssl);
    close(client_fd);
    metrics_.active_connections--;
    return;
  }

  metrics_.cache_hits++;
  SSL_write(ssl, header_ptr, header_size);
  SSL_write(ssl, data_ptr, file_size);
  metrics_.bytes_sent += header_size + file_size;

  SSL_shutdown(ssl);
  tls_.free_ssl(ssl);
  close(client_fd);
  metrics_.active_connections--;
}
#endif
