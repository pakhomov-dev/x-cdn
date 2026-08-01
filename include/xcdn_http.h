#pragma once
#include <string>
#include <string_view>

struct HttpRequest {
    std::string method;
    std::string path;
};

class XHttpParser {
public:
    bool parse(const char* data, size_t len, HttpRequest& req) const;
};