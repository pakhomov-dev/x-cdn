#include "xcdn_http.h"
#include <cstring>

bool XHttpParser::parse(const char *data, size_t len, HttpRequest &req) const {
  // Сверхбыстрый поиск конца первой строки через memchr (на уровне ассемблера)
  const char *end = static_cast<const char *>(memchr(data, '\r', len));
  if (!end)
    return false;

  // Создаем string_view на всю первую строку
  std::string_view line(data, end - data);

  size_t pos1 = line.find(' ');
  if (pos1 == std::string_view::npos)
    return false;

  size_t pos2 = line.find(' ', pos1 + 1);
  if (pos2 == std::string_view::npos)
    return false;

  // ИСПРАВЛЕНО: Теперь substr возвращает string_view! Ноль аллокаций памяти!
  req.method = line.substr(0, pos1);
  req.path = line.substr(pos1 + 1, pos2 - pos1 - 1);

  return true;
}
