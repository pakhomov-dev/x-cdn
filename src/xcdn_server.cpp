#include "xcdn_server.h"
#include "xcdn_http.h"
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>

// Глобальный указатель для обработки сигнала
static XServer* g_server = nullptr;

extern "C" void handle_signal(int) {
    if (g_server) g_server->stop();
}

XServer::XServer(int port, const std::string& cache_dir)
    : port_(port), running_(false)
{
    g_server = this;

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) { std::cerr << "socket() failed\n"; exit(1); }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) { std::cerr << "bind() failed\n"; exit(1); }
    if (listen(server_fd_, 4096) < 0) { std::cerr << "listen() failed\n"; exit(1); }

    // Загружаем все файлы из указанной папки в кэш
    cache_.loadDirectory(cache_dir);
}

XServer::~XServer() { stop(); if (server_fd_ >= 0) close(server_fd_); }

void XServer::start() {
    running_ = true;
    unsigned int n = std::thread::hardware_concurrency();
    std::cout << "[XServer] Starting on port " << port_ << " with " << n << " threads\n";
    for (unsigned int i = 0; i < n; ++i)
        workers_.emplace_back(&XServer::workerThread, this, i);
}

void XServer::stop() {
    running_ = false;
    for (auto& t : workers_) if (t.joinable()) t.join();
}

void XServer::workerThread(int core_id) {
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(core_id, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

    struct io_uring ring;
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, IORING_SETUP_SINGLE_ISSUER) < 0) return;

    if (cache_.bufferSize() > 0) {
        struct iovec iov;
        iov.iov_base = const_cast<char*>(cache_.bufferData());
        iov.iov_len  = cache_.bufferSize();
        if (io_uring_register_buffers(&ring, &iov, 1) < 0) {
            std::cerr << "[Worker " << core_id << "] Failed to register buffers, using copy\n";
        } else {
            std::cout << "[Worker " << core_id << "] Zero-Copy enabled (" << cache_.bufferSize() << " bytes)\n";
        }
    }

    std::vector<ConnInfo> pool(MAX_CONNECTIONS);
    ConnInfo si{};
    si.fd = server_fd_;
    si.state = ConnInfo::ACCEPT;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, server_fd_, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe, &si);
    io_uring_submit(&ring);

    XHttpParser parser;

    while (running_) {
        struct io_uring_cqe* cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0) continue;

        ConnInfo* info = static_cast<ConnInfo*>(io_uring_cqe_get_data(cqe));

        if (cqe->res < 0) {
            io_uring_cqe_seen(&ring, cqe);
            if (info && info->state == ConnInfo::ACCEPT) {
                sqe = io_uring_get_sqe(&ring);
                io_uring_prep_accept(sqe, server_fd_, nullptr, nullptr, 0);
                io_uring_sqe_set_data(sqe, &si);
                io_uring_submit(&ring);
            } else if (info) close(info->fd);
            continue;
        }

        switch (info->state) {
        case ConnInfo::ACCEPT: {
            int fd = cqe->res;
            if (fd >= MAX_CONNECTIONS) { close(fd); break; }
            auto* ci = &pool[fd];
            ci->fd = fd;
            ci->state = ConnInfo::READ;

            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_read(sqe, fd, ci->buffer, sizeof(ci->buffer), 0);
            io_uring_sqe_set_data(sqe, ci);

            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_accept(sqe, server_fd_, nullptr, nullptr, 0);
            io_uring_sqe_set_data(sqe, &si);
            io_uring_submit(&ring);
            break;
        }
        case ConnInfo::READ: {
            if (cqe->res <= 0) { close(info->fd); break; }

            HttpRequest req;
            if (!parser.parse(info->buffer, cqe->res, req)) { close(info->fd); break; }

            // Извлекаем путь к файлу
            std::string file_path = req.path;
            if (file_path.empty() || file_path == "/") file_path = "index.html";
            else if (file_path[0] == '/') file_path = file_path.substr(1);

            // Ищем готовый ответ в кэше
            size_t resp_len = 0;
            const char* resp = cache_.get(file_path, resp_len);
            if (!resp) {
                // Если файла нет – пробуем 404.html
                resp = cache_.get("404.html", resp_len);
                if (!resp) {
                    static const char fallback[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
                    resp = fallback;
                    resp_len = strlen(fallback);
                }
            }

            // Отправка с Zero‑Copy, если буфер зарегистрирован
            if (resp >= cache_.bufferData() && resp < cache_.bufferData() + cache_.bufferSize()) {
                sqe = io_uring_get_sqe(&ring);
                io_uring_prep_write_fixed(sqe, info->fd, resp, resp_len, 0, 0);
                io_uring_sqe_set_data(sqe, info);
            } else {
                if (resp_len > sizeof(info->buffer)) { close(info->fd); break; }
                memcpy(info->buffer, resp, resp_len);
                info->total_len = resp_len;
                sqe = io_uring_get_sqe(&ring);
                io_uring_prep_write(sqe, info->fd, info->buffer, info->total_len, 0);
                io_uring_sqe_set_data(sqe, info);
            }
            info->state = ConnInfo::WRITE;
            io_uring_submit(&ring);
            break;
        }
        case ConnInfo::WRITE: {
            info->state = ConnInfo::READ;
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_read(sqe, info->fd, info->buffer, sizeof(info->buffer), 0);
            io_uring_sqe_set_data(sqe, info);
            io_uring_submit(&ring);
            break;
        }
        }
        io_uring_cqe_seen(&ring, cqe);
    }
    io_uring_queue_exit(&ring);
}