#include "xcdn_tls.h"
#include <iostream>

XTls::~XTls() {
  if (ctx_)
    SSL_CTX_free(ctx_);
}

bool XTls::init(const std::string &cert_path, const std::string &key_path) {
  const SSL_METHOD *method = TLS_server_method();
  ctx_ = SSL_CTX_new(method);
  if (!ctx_) {
    std::cerr << "[TLS] SSL_CTX_new failed\n";
    return false;
  }

  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

  if (SSL_CTX_use_certificate_file(ctx_, cert_path.c_str(),
                                   SSL_FILETYPE_PEM) <= 0) {
    std::cerr << "[TLS] Cannot load cert: " << cert_path << "\n";
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
    return false;
  }

  if (SSL_CTX_use_PrivateKey_file(ctx_, key_path.c_str(),
                                  SSL_FILETYPE_PEM) <= 0) {
    std::cerr << "[TLS] Cannot load key: " << key_path << "\n";
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
    return false;
  }

  if (!SSL_CTX_check_private_key(ctx_)) {
    std::cerr << "[TLS] Private key does not match certificate\n";
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
    return false;
  }

  SSL_CTX_set_mode(ctx_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  std::cerr << "[TLS] Initialized: " << cert_path << " + " << key_path << "\n";
  return true;
}

SSL *XTls::new_ssl(int fd) {
  SSL *ssl = SSL_new(ctx_);
  if (!ssl)
    return nullptr;
  SSL_set_fd(ssl, fd);
  return ssl;
}

void XTls::free_ssl(SSL *ssl) {
  if (ssl)
    SSL_free(ssl);
}
