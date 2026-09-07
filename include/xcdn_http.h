#pragma once
#include <string_view>

struct HttpRequest {
  std::string_view method;
  std::string_view path;
};

class XHttpParser {
public:
  bool parse(const char *data, size_t len, HttpRequest &req) const;
};
