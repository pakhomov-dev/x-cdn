#include "xcdn_http.h"
#include <cstring>

bool XHttpParser::parse(const char *data, size_t len, HttpRequest &req) const {
  const char *end = static_cast<const char *>(memchr(data, '\r', len));
  if (!end)
    return false;

  std::string_view line(data, end - data);

  size_t pos1 = line.find(' ');
  if (pos1 == std::string_view::npos)
    return false;

  size_t pos2 = line.find(' ', pos1 + 1);
  if (pos2 == std::string_view::npos)
    return false;

  req.method = line.substr(0, pos1);
  req.path = line.substr(pos1 + 1, pos2 - pos1 - 1);

  return true;
}
