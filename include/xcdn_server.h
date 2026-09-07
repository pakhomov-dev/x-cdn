#pragma once
#include "xcdn_cache.h"
#include "xcdn_http.h"
#include <liburing.h>
#include <string>
#include <thread>
#include <vector>

constexpr int QUEUE_DEPTH = 8192;
constexpr int MAX_CONNECTIONS = 8192;
constexpr size_t SMALL_FILE_THRESHOLD = 4096;

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
};

class XServer {
public:
  XServer(int port, const std::string &cache_dir);
  ~XServer();
  void start();
  void stop();

private:
  void workerThread(int core_id);

  int port_;
  int server_fd_;
  bool running_;
  std::vector<std::thread> workers_;
  XCache cache_;
};
