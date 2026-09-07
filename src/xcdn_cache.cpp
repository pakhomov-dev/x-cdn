#include "xcdn_cache.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

static std::string_view getMimeType(std::string_view path) {
  if (path.ends_with(".css"))
    return "text/css";
  if (path.ends_with(".js"))
    return "application/javascript";
  if (path.ends_with(".png"))
    return "image/png";
  if (path.ends_with(".jpg") || path.ends_with(".jpeg"))
    return "image/jpeg";
  if (path.ends_with(".svg"))
    return "image/svg+xml";
  if (path.ends_with(".ico"))
    return "image/x-icon";
  if (path.ends_with(".json"))
    return "application/json";
  if (path.ends_with(".html") || path.ends_with(".htm"))
    return "text/html";
  return "application/octet-stream";
}

void XCache::preload(const std::string &filePath) {
  int fd = open(filePath.c_str(), O_RDONLY);
  if (fd < 0) {
    std::cerr << "[XCache] Cannot open: " << filePath << std::endl;
    return;
  }

  size_t size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  char *raw_ptr = nullptr;
  if (posix_memalign((void **)&raw_ptr, 4096, size) != 0) {
    std::cerr << "[XCache] Memalign failed for: " << filePath << std::endl;
    close(fd);
    return;
  }

  std::shared_ptr<char[]> file_buffer(raw_ptr, [](char *p) { std::free(p); });

  size_t total_read = 0;
  while (total_read < size) {
    ssize_t bytes_read =
        read(fd, file_buffer.get() + total_read, size - total_read);
    if (bytes_read <= 0)
      break;
    total_read += bytes_read;
  }
  close(fd);

  if (total_read != size) {
    std::cerr << "[XCache] Failed to read full file: " << filePath << std::endl;
    return;
  }

  std::string key = fs::path(filePath).filename().string();
  std::string_view mime = getMimeType(key);

  // Pre-build HTTP response headers once at load time
  char header_buf[1024];
  int header_len = snprintf(
      header_buf, sizeof(header_buf),
      "HTTP/1.1 200 OK\r\nContent-Type: %.*s\r\nContent-Length: %zu\r\n"
      "Connection: keep-alive\r\nCache-Control: public, "
      "max-age=31536000\r\n"
      "Server: XCDN-Killer\r\n\r\n",
      (int)mime.size(), mime.data(), size);

  char *header_raw = nullptr;
  if (posix_memalign((void **)&header_raw, 4096, header_len) != 0) {
    std::cerr << "[XCache] Header memalign failed for: " << filePath
              << std::endl;
    return;
  }
  memcpy(header_raw, header_buf, header_len);
  std::shared_ptr<char[]> header_buffer(header_raw,
                                        [](char *p) { std::free(p); });

  files_[key] = CacheEntry{file_buffer, header_buffer, size,
                           (size_t)header_len, mime, current_idx_++};
  std::cout << "[XCache] Preloaded: " << key
            << " (index: " << current_idx_ - 1 << ", " << size
            << " bytes, header: " << header_len << " bytes)" << std::endl;
}

bool XCache::get(std::string_view path, const char *&data_ptr, size_t &size,
                 const char *&header_ptr, size_t &header_size,
                 int &buf_idx) const {
  auto it = files_.find(std::string(path));
  if (it == files_.end())
    return false;

  data_ptr = it->second.buffer.get();
  size = it->second.size;
  header_ptr = it->second.header_buffer.get();
  header_size = it->second.header_size;
  buf_idx = it->second.buffer_idx;
  return true;
}

void XCache::loadDirectory(const std::string &dirPath) {
  for (const auto &entry : fs::directory_iterator(dirPath)) {
    if (entry.is_regular_file()) {
      preload(entry.path().string());
    }
  }
}
