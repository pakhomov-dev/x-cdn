#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

struct Metrics {
  std::atomic<uint64_t> total_requests{0};
  std::atomic<uint64_t> total_404s{0};
  std::atomic<uint64_t> bytes_sent{0};
  std::atomic<uint64_t> active_connections{0};
  std::atomic<uint64_t> cache_hits{0};
  std::atomic<uint64_t> cache_misses{0};

  std::chrono::steady_clock::time_point start_time;

  Metrics() : start_time(std::chrono::steady_clock::now()) {}

  std::string format_prometheus() const {
    auto now = std::chrono::steady_clock::now();
    double uptime =
        std::chrono::duration<double>(now - start_time).count();

    return "# HELP xcdn_total_requests Total HTTP requests served.\n"
           "# TYPE xcdn_total_requests counter\n" +
           std::string("xcdn_total_requests ") +
           std::to_string(total_requests.load()) + "\n" +
           "# HELP xcdn_responses_404 Total 404 responses.\n"
           "# TYPE xcdn_responses_404 counter\n" +
           "xcdn_responses_404 " + std::to_string(total_404s.load()) + "\n" +
           "# HELP xcdn_bytes_sent_total Total bytes sent.\n"
           "# TYPE xcdn_bytes_sent_total counter\n" +
           "xcdn_bytes_sent_total " + std::to_string(bytes_sent.load()) + "\n" +
           "# HELP xcdn_active_connections Current active connections.\n"
           "# TYPE xcdn_active_connections gauge\n" +
           "xcdn_active_connections " +
           std::to_string(active_connections.load()) + "\n" +
           "# HELP xcdn_cache_hits_total Cache hits.\n"
           "# TYPE xcdn_cache_hits_total counter\n" +
           "xcdn_cache_hits_total " + std::to_string(cache_hits.load()) + "\n" +
           "# HELP xcdn_cache_misses_total Cache misses.\n"
           "# TYPE xcdn_cache_misses_total counter\n" +
           "xcdn_cache_misses_total " +
           std::to_string(cache_misses.load()) + "\n" +
           "# HELP xcdn_uptime_seconds Server uptime in seconds.\n"
           "# TYPE xcdn_uptime_seconds gauge\n" +
           "xcdn_uptime_seconds " + std::to_string(uptime) + "\n";
  }
};
