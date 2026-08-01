#include "xcdn_cache.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

// Простейшее определение MIME-типа по расширению
static std::string getMimeType(const std::string& path) {
    if (path.ends_with(".css"))  return "text/css";
    if (path.ends_with(".js"))   return "application/javascript";
    if (path.ends_with(".png"))  return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
    if (path.ends_with(".svg"))  return "image/svg+xml";
    if (path.ends_with(".ico"))  return "image/x-icon";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".html") || path.ends_with(".htm")) return "text/html";
    return "application/octet-stream";
}

void XCache::preload(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[XCache] Cannot open: " << filePath << std::endl;
        return;
    }
    size_t bodySize = file.tellg();
    file.seekg(0);
    std::string body(bodySize, '\0');
    file.read(&body[0], bodySize);

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: " << getMimeType(filePath) << "\r\n"
             << "Content-Length: " << bodySize << "\r\n"
             << "Connection: keep-alive\r\n"
             << "\r\n"
             << body;

    std::string respStr = response.str();
    size_t offset = pool_.size();
    pool_.insert(pool_.end(), respStr.begin(), respStr.end());

    // Ключ – только имя файла (например, "index.html")
    std::string key = fs::path(filePath).filename().string();
    files_[key] = {offset, respStr.size()};
    std::cout << "[XCache] Preloaded " << key << " (" << respStr.size() << " bytes)" << std::endl;
}

void XCache::loadDirectory(const std::string& dirPath) {
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.is_regular_file()) {
            preload(entry.path().string());
        }
    }
}

const char* XCache::get(const std::string& path, size_t& size) const {
    auto it = files_.find(path);
    if (it == files_.end()) {
        size = 0;
        return nullptr;
    }
    size = it->second.second;
    return pool_.data() + it->second.first;
}