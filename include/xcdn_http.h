#pragma once
#include <string_view>

struct HttpRequest {
  std::string_view method;
  std::string_view path;
};

class XHttpParser {
public:
  // Метод parse теперь принимает HttpRequest по ссылке и нарезает string_view
  bool parse(const char *data, size_t len, HttpRequest &req) const;
};
