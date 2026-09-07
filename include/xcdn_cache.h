#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

class XCache {
public:
  struct CacheEntry {
    std::shared_ptr<char[]> buffer;
    std::shared_ptr<char[]> header_buffer;
    size_t size;
    size_t header_size;
    std::string_view mimeType;
    int buffer_idx;
  };

  void loadDirectory(const std::string &dirPath, int cache_max_age,
                     const std::string &server_name);
  void preload(const std::string &filePath, int cache_max_age,
               const std::string &server_name);
  bool get(std::string_view path, const char *&data_ptr, size_t &size,
           const char *&header_ptr, size_t &header_size, int &buf_idx) const;

  const std::unordered_map<std::string, CacheEntry> &getFiles() const {
    return files_;
  }

private:
  std::unordered_map<std::string, CacheEntry> files_;
  int current_idx_ = 0;
};
