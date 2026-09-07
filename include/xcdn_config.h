#pragma once
#include <string>

enum class IoUringMode { SQPOLL, POLLING, DISABLED };

struct ServerConfig {
  int port = 8080;
  std::string root = "public";
  int workers = 0;
  int queue_depth = 8192;
  int max_connections = 8192;
  int listen_backlog = 4096;
  int busy_poll_us = 50;
  int small_file_threshold = 4096;

  IoUringMode uring_mode = IoUringMode::SQPOLL;

  bool tls_enabled = false;
  std::string tls_cert;
  std::string tls_key;

  int cache_max_age = 31536000;
  std::string server_name = "XCDN-Killer";

  bool metrics_enabled = false;
  std::string metrics_path = "/metrics";

  std::string config_file;
};

ServerConfig parse_args(int argc, char *argv[]);
ServerConfig load_config(const std::string &path);
void print_usage(const char *prog);
