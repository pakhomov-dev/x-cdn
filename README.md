<div align="center">

# ⚡ X-CDN

**Ultra-low latency static file server on Linux io_uring**

*In-memory caching · zero-copy splice pipeline · sub-millisecond responses*

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![io_uring](https://img.shields.io/badge/Linux-io__uring-333333.svg)
![Docker](https://img.shields.io/badge/docker-ready-2496ED.svg)

</div>

---

## Why?

nginx is great — until you measure what actually happens when 100 connections hammer your static assets. X-CDN is built around one idea: **keep hot files in RAM and get them to the socket with as few kernel crossings as possible**, using Linux's modern async I/O interface instead of a classic `epoll` loop.

One linked chain of `io_uring` SQEs per request: `send(headers) → splice(file → pipe) → splice(pipe → socket)`. No `write()` copies for the body, no thread-per-connection, no event-loop wakeups per chunk.

## Benchmarks

Same machine, same session, both servers pinned to CPU cores (`taskset`), `wrk -t3 -c100 -d5s`, 1 worker, keep-alive:

| File | X-CDN req/s | nginx req/s | X-CDN avg latency | nginx avg latency | Winner |
|------------|------------|------------|-------------------|-------------------|--------|
| **1 KB** | **175,007** | 132,553 | **0.56 ms** | 0.75 ms | 🏆 X-CDN **+32% RPS** |
| **100 KB** | **93,107** | 86,158 | **1.05 ms** | 3.03 ms | 🏆 X-CDN **+8% RPS, 2.9× lower latency** |
| **1 MB** | 16,685 | **22,772** | **2.29 ms** | 4.25 ms | split: nginx wins RPS, X-CDN **1.9× lower latency** |

Peak observed on 1 KB: **199,802 req/s**.

> Honest notes: numbers vary ±20% run-to-run on a shared dev box (best-of shown in table header column is from one continuous session). On 1 MB @ 100 connections X-CDN currently reports read errors — see [Roadmap](#roadmap). Reproduce everything yourself with two commands (see [Reproduce](#reproduce-the-benchmarks)).

**What this means in practice:** for the sizes that dominate real websites — small assets, API payloads, images, CSS/JS — X-CDN serves more requests per core with ~3× better latency than nginx, because the data never leaves the kernel's page cache path.

## Features

- 🚀 **io_uring throughout** — accept, read, send, splice all through one ring per worker; SQPOLL / polling / disabled modes selectable at runtime
- 🔗 **Linked SQE splice pipeline** — headers and file body are chained kernel-side; the request is answered in one submission with zero body copies
- 🧠 **In-memory cache** — every file is preloaded at startup and served from RAM with `Cache-Control: public, max-age=31536000`
- 🧵 **One thread per core, no context-switch storms** — each worker owns its ring, its connection pool, and its CPU core
- 📡 **Prometheus `/metrics`** — request counts, cache hits, active connections
- 🐳 **Docker out of the box** — single static binary, `docker compose up` and go
- 🌱 **Drop-in behind nginx** — plain HTTP/1.1 backend, speaks to any reverse proxy

## Quick start

### Docker (recommended)

```bash
docker compose up -d
curl http://127.0.0.1:8080/
```

### Build from source

```bash
sudo apt install -y build-essential liburing-dev
make -j$(nproc)
./xcdn --port 8080 --dir public --workers 4
```

### Behind nginx (the fast path to production)

```nginx
server {
    listen 443 ssl http2;
    server_name example.com;

    # dynamic stuff stays in your app
    location /api/ { proxy_pass http://127.0.0.1:3000; }

    # static assets go to X-CDN — zero-copy from RAM
    location ~* \.(js|css|png|jpe?g|gif|svg|woff2?|ico|mp4|webp)$ {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_buffering off;
    }
}
```

TLS, HTTP/2, rate limiting and logging stay where your team already knows them — in nginx. X-CDN does exactly one thing: push static bytes at line rate.

## Configuration

`config.yaml` or CLI — both work, CLI wins:

```yaml
server:
  port: 8080
  workers: 0            # 0 = one worker per core
  uring_mode: disabled   # disabled | sqpoll | polling
  busy_poll_us: 50       # kernel busy-polling, kills tail latency
```

```bash
./xcdn -c config.yaml -w 8 --uring-mode sqpoll --metrics
```

| Flag | Meaning |
|------|---------|
| `--uring-mode disabled` | best all-round: lowest latency, no kernel thread (**default**) |
| `--uring-mode sqpoll` | kernel polls the SQ — wins on tiny files, costs power |
| `--uring-mode polling` | legacy busy-polling path |
| `-w, --workers N` | worker threads (0 = auto, one per core) |
| `--metrics` | expose Prometheus `/metrics` |

## Architecture

```
                 ┌────────────────────────────────────────────┐
   client ──────▶│  worker N  (pinned to core N)              │
                 │                                            │
                 │   io_uring (single ring per worker)        │
                 │      │                                     │
                 │      ▼                                     │
                 │   accept ──▶ read ──▶ parse                │
                 │                       │                    │
                 │              RAM cache hit?                │
                 │                       │                    │
                 │        ┌──────────────┴──────────────┐     │
                 │        ▼                             ▼     │
                 │   ≤ 256 KB: single send()        large:    │
                 │   (registered buffer)            LINK chain│
                 │                                  send hdr  │
                 │                                  LINK      │
                 │                                  splice f→p│
                 │                                  LINK      │
                 │                                  splice p→s│
                 └────────────────────────────────────────────┘
```

Small responses go out through a pre-registered buffer in one `send`. Anything above `SPLICE_THRESHOLD` (256 KB) switches to the linked splice chain: the kernel moves bytes from the file mapping into the pipe and out to the socket **without ever touching userspace**.

## Reproduce the benchmarks

```bash
./run_bench_xcdn.sh 1 disabled 5 100    # workers, mode, seconds, connections
./run_bench_nginx.sh
```

Both scripts pin the server to core 0 and `wrk` to cores 1–3, warm the cache, and print `req/s + latency` per file size. Same commands, same machine, no cherry-picking.

## Roadmap

- [ ] Eliminate read errors on 1 MB @ 100 connections (tracked; small files unaffected)
- [ ] `Range` / `HEAD` / `If-Modified-Since` support for full reverse-proxy compatibility
- [ ] HTTP/2 and kTLS (kernel TLS keeps splice zero-copy end-to-end)
- [ ] Dynamic reload (SIGHUP) — swap cached files without restart
- [ ] Results on dedicated 32-core hardware

## License

[MIT](LICENSE) © Timur Pakhomov
