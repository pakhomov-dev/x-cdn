#pragma once
#include "xcdn_cache.h"
#include "xcdn_config.h"
#include "xcdn_http.h"
#include "xcdn_metrics.h"
#ifdef XCDN_TLS_ENABLED
#include "xcdn_tls.h"
#endif
#include <liburing.h>
#include <string>
#include <thread>
#include <vector>

struct ConnInfo {
  enum State { ACCEPT, READ, WRITE_BODY, WRITE_RAW };
  int fd;
  int state;

  char buffer[8192];

  const char *file_data;
  size_t file_size;
  size_t bytes_sent;
  size_t header_len;
  const char *header_data;
  int file_buf_idx;
  int refs;

#ifdef XCDN_TLS_ENABLED
  SSL *ssl = nullptr;
#endif
};

class XServer {
public:
  explicit XServer(const ServerConfig &cfg);
  ~XServer();
  void start();
  void stop();

private:
  void workerThread(int core_id);
#ifdef XCDN_TLS_ENABLED
  int sslRead(ConnInfo *info, char *buf, int len);
  int sslWrite(ConnInfo *info, const char *buf, int len);
  void handleTlsConnection(int client_fd);
#endif

  ServerConfig cfg_;
  int server_fd_;
  bool running_;
  std::vector<std::thread> workers_;
  XCache cache_;
  Metrics metrics_;
#ifdef XCDN_TLS_ENABLED
  XTls tls_;
#endif
};
