#pragma once
#ifdef XCDN_TLS_ENABLED
#include <openssl/ssl.h>
#include <string>

class XTls {
public:
  XTls() = default;
  ~XTls();

  bool init(const std::string &cert_path, const std::string &key_path);
  SSL *new_ssl(int fd);
  void free_ssl(SSL *ssl);

  bool enabled() const { return ctx_ != nullptr; }

private:
  SSL_CTX *ctx_ = nullptr;
};
#endif
