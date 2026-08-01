#pragma once
#include <liburing.h>
#include <vector>
#include <thread>
#include <string>
#include "xcdn_cache.h"
#include "xcdn_http.h" 

class XServer {
public:
    XServer(int port, const std::string& cache_dir);
    ~XServer();

    void start();  
    void stop();   

private:
    void workerThread(int core_id);

    struct ConnInfo {
        int fd;
        enum { ACCEPT, READ, WRITE } state;
        char buffer[4096];
        size_t bytes_done = 0;
        size_t total_len = 0;
    };

    static constexpr int QUEUE_DEPTH = 512;
    static constexpr int MAX_CONNECTIONS = 32768;

    int server_fd_;
    int port_;
    XCache cache_;
    std::vector<std::thread> workers_;
    bool running_ = false;
};