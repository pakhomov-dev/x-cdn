#include "xcdn_config.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <string>

static std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\r\n\"");
  size_t end = s.find_last_not_of(" \t\r\n\"");
  if (start == std::string::npos)
    return "";
  return s.substr(start, end - start + 1);
}

static int parse_int(const std::string &s, int fallback) {
  try {
    return std::stoi(s);
  } catch (...) {
    return fallback;
  }
}

ServerConfig load_config(const std::string &path) {
  ServerConfig cfg;
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "[config] Cannot open: " << path << "\n";
    return cfg;
  }

  std::string line;
  std::string section;
  while (std::getline(file, line)) {
    size_t comment = line.find('#');
    if (comment != std::string::npos)
      line = line.substr(0, comment);

    size_t colon = line.find(':');
    if (colon == std::string::npos) {
      size_t indent = line.find_first_not_of(" \t");
      if (indent == std::string::npos)
        continue;
      section = trim(line);
      continue;
    }

    std::string key = trim(line.substr(0, colon));
    std::string val = trim(line.substr(colon + 1));

    if (section == "server") {
      if (key == "port")
        cfg.port = parse_int(val, cfg.port);
      else if (key == "root")
        cfg.root = val;
      else if (key == "workers")
        cfg.workers = parse_int(val, cfg.workers);
      else if (key == "queue_depth")
        cfg.queue_depth = parse_int(val, cfg.queue_depth);
      else if (key == "max_connections")
        cfg.max_connections = parse_int(val, cfg.max_connections);
      else if (key == "listen_backlog")
        cfg.listen_backlog = parse_int(val, cfg.listen_backlog);
      else if (key == "busy_poll_us")
        cfg.busy_poll_us = parse_int(val, cfg.busy_poll_us);
      else if (key == "server_name")
        cfg.server_name = val;
      else if (key == "uring_mode") {
        if (val == "sqpoll")
          cfg.uring_mode = IoUringMode::SQPOLL;
        else if (val == "polling")
          cfg.uring_mode = IoUringMode::POLLING;
        else if (val == "disabled")
          cfg.uring_mode = IoUringMode::DISABLED;
      }
    } else if (section == "tls") {
      if (key == "enabled")
        cfg.tls_enabled = (val == "true" || val == "1");
      else if (key == "cert")
        cfg.tls_cert = val;
      else if (key == "key")
        cfg.tls_key = val;
    } else if (section == "cache") {
      if (key == "max_age")
        cfg.cache_max_age = parse_int(val, cfg.cache_max_age);
    } else if (section == "metrics") {
      if (key == "enabled")
        cfg.metrics_enabled = (val == "true" || val == "1");
      else if (key == "path")
        cfg.metrics_path = val;
    }
  }

  std::cerr << "[config] Loaded from " << path << "\n";
  return cfg;
}

void print_usage(const char *prog) {
  std::cout
      << "X-CDN — High-performance static file server\n\n"
      << "Usage: " << prog << " [options]\n\n"
      << "Options:\n"
      << "  -c, --config <file>       Config file (YAML)\n"
      << "  -p, --port <port>         Listen port (default: 8080)\n"
      << "  -d, --dir <path>          Static files directory (default: "
         "public)\n"
      << "  -w, --workers <n>         Worker threads (default: auto)\n"
      << "  --queue-depth <n>         io_uring queue depth (default: 8192)\n"
      << "  --max-connections <n>     Max connections per worker (default: "
         "8192)\n"
      << "  --uring-mode <mode>       sqpoll | polling | disabled\n"
      << "  --tls-cert <file>         TLS certificate path\n"
      << "  --tls-key <file>          TLS private key path\n"
      << "  --metrics                 Enable Prometheus /metrics endpoint\n"
      << "  -h, --help                Show this help\n";
}

ServerConfig parse_args(int argc, char *argv[]) {
  static struct option long_options[] = {
      {"config", required_argument, nullptr, 'c'},
      {"port", required_argument, nullptr, 'p'},
      {"dir", required_argument, nullptr, 'd'},
      {"workers", required_argument, nullptr, 'w'},
      {"queue-depth", required_argument, nullptr, 'q'},
      {"max-connections", required_argument, nullptr, 'm'},
      {"uring-mode", required_argument, nullptr, 'u'},
      {"tls-cert", required_argument, nullptr, 't'},
      {"tls-key", required_argument, nullptr, 'k'},
      {"metrics", no_argument, nullptr, 'e'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};

  // Pass 1: find --config path
  std::string config_path;
  for (int i = 1; i < argc; i++) {
    if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) &&
        i + 1 < argc) {
      config_path = argv[i + 1];
      break;
    }
  }

  // Load config file first (base)
  ServerConfig cfg;
  if (!config_path.empty()) {
    cfg = load_config(config_path);
  }

  // Pass 2: CLI flags override config file values
  optind = 1;
  int opt;
  while ((opt = getopt_long(argc, argv, "c:p:d:w:q:m:u:t:k:eh", long_options,
                            nullptr)) != -1) {
    switch (opt) {
    case 'c':
      break;
    case 'p':
      cfg.port = std::stoi(optarg);
      break;
    case 'd':
      cfg.root = optarg;
      break;
    case 'w':
      cfg.workers = std::stoi(optarg);
      break;
    case 'q':
      cfg.queue_depth = std::stoi(optarg);
      break;
    case 'm':
      cfg.max_connections = std::stoi(optarg);
      break;
    case 'u':
      if (strcmp(optarg, "sqpoll") == 0)
        cfg.uring_mode = IoUringMode::SQPOLL;
      else if (strcmp(optarg, "polling") == 0)
        cfg.uring_mode = IoUringMode::POLLING;
      else if (strcmp(optarg, "disabled") == 0)
        cfg.uring_mode = IoUringMode::DISABLED;
      break;
    case 't':
      cfg.tls_cert = optarg;
      cfg.tls_enabled = true;
      break;
    case 'k':
      cfg.tls_key = optarg;
      cfg.tls_enabled = true;
      break;
    case 'e':
      cfg.metrics_enabled = true;
      break;
    case 'h':
      print_usage(argv[0]);
      exit(0);
    default:
      print_usage(argv[0]);
      exit(1);
    }
  }

  return cfg;
}
