#include "xcdn_cache.h"
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

static std::string getMimeType(const std::string &path) {
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

  // Выравниваем буфер по границе страницы 4096 байт для сверхбыстрого ядерного
  // DMA
  char *raw_ptr = nullptr;
  if (posix_memalign((void **)&raw_ptr, 4096, size) != 0) {
    std::cerr << "[XCache] Memalign failed for: " << filePath << std::endl;
    close(fd);
    return;
  }

  // Обертываем сырой указатель в умный shared_ptr с правильным удалением через
  // free()
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

  // Сохраняем выровненную структуру в мапу кэша
  files_[key] =
      CacheEntry{file_buffer, size, getMimeType(filePath), current_idx_++};
  std::cout << "[XCache] Preloaded & Aligned: " << key
            << " (index: " << current_idx_ - 1 << ", " << size << " bytes)"
            << std::endl;
}

// Метод get теперь работает строго по сигнатуре из xcdn_cache.h
bool XCache::get(const std::string &path, const char *&data_ptr, size_t &size,
                 std::string &mimeType, int &buf_idx) const {
  auto it = files_.find(path);
  if (it == files_.end())
    return false;

  // Для получения сырого указателя из shared_ptr<char[]> используем .get()
  data_ptr = it->second.buffer.get();
  size = it->second.size;
  mimeType = it->second.mimeType;
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
