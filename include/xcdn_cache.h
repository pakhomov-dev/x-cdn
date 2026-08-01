#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

class XCache {
public:
    // Загружает все файлы из папки в кэш
    void loadDirectory(const std::string& dirPath);

    // Возвращает указатель на готовый HTTP‑ответ (заголовки+тело)
    const char* get(const std::string& path, size_t& size) const;

    // Доступ к сырым данным буфера (для регистрации в io_uring)
    const char* bufferData() const { return pool_.data(); }
    size_t bufferSize() const { return pool_.size(); }

private:
    // Формирует полный HTTP‑ответ для одного файла и добавляет в пул
    void preload(const std::string& filePath);

    std::vector<char> pool_;                              // единый буфер
    std::unordered_map<std::string, std::pair<size_t, size_t>> files_; // путь -> (offset, size)
};