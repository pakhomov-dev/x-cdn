# X-CDN: Ultra-Low Latency Static Cache Server on io_uring

X-CDN is an experimental, high-performance HTTP static file server that leverages **Linux io_uring** and **in-memory caching** to deliver static assets with minimal latency. It is designed to accelerate web applications by serving static content directly from RAM, bypassing traditional disk I/O and reducing CPU overhead.

## Key Features

- **Asynchronous I/O with io_uring** – fully event-driven architecture using the latest Linux kernel interface.
- **Zero-Copy data path** – registered buffers enable `write_fixed` to send preloaded HTTP responses without copying data between kernel and userspace.
- **Multi-Threaded per-Core Design** – each worker thread is pinned to a dedicated CPU core, maximizing cache locality.
- **Keep-Alive Support** – persistent connections for multiple requests.
- **Graceful Shutdown** – handles `SIGINT`/`SIGTERM` to stop accepting new connections and drain existing ones.
- **MIME-Type Detection** – automatic `Content-Type` header based on file extension.
- **Simple Directory-Based Cache** – place all your static files in the `public/` folder; they are loaded into RAM at startup.

## Performance Benchmarks (preliminary)

Tests were performed on a single machine (Ubuntu, 16 cores) using `wrk` with 2 threads and 100 concurrent connections for 10 seconds. File sizes: 1 KB, 100 KB, 1 MB.

| File Size | X-CDN (req/s) | nginx 1.24.0 (req/s) | X-CDN Latency (avg) | nginx Latency (avg) |
|-----------|---------------|----------------------|----------------------|----------------------|
| 1 KB      | 230,112       | 339,981*             | 248 µs               | 178 µs               |
| 100 KB    | 100,540       | 106,134              | 568 µs               | 3.84 ms              |
| 1 MB      | 7,692**       | 14,625               | 5.63 ms              | 2.85 ms              |

*nginx 1KB result is inflated because it benefits from kernel page cache and `sendfile()`; X-CDN uses an in-memory buffer without `sendfile` yet.  
**1 MB test for X-CDN experienced socket timeouts under high load; improvements are in progress.

> **Note:** X-CDN is in active development. The current version demonstrates the viability of io_uring for HTTP servers. Future optimizations (splice, sendfile, SQPOLL) are expected to significantly close the gap and outperform traditional event loops.

## Build Requirements

- Linux kernel 5.6 or later (for io_uring)
- g++ 10+ (C++20 support)
- liburing-dev

Install dependencies on Ubuntu:
```bash
sudo apt update
sudo apt install -y build-essential liburing-dev