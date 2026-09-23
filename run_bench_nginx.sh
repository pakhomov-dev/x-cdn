#!/bin/bash
cd "$(dirname "$0")"

pkill -x nginx 2>/dev/null
sleep 1

F1="test1k.bin"
F100="test100k.bin"
F1M="test1m.bin"
T=5
C=100
THR=4

cat > /tmp/nginx_bench.conf << 'NGX'
worker_processes 1;
pid /tmp/nginx_bench.pid;
events { worker_connections 4096; use epoll; multi_accept on; }
http {
    access_log off;
    error_log /tmp/nginx_error.log crit;
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 65;
    keepalive_requests 100000;
    open_file_cache max=1000 inactive=20s;
    open_file_cache_valid 30s;
    open_file_cache_min_uses 2;
    open_file_cache_errors on;
    server {
        listen 8081 reuseport;
        server_name localhost;
        root PUBLICDIR;
        location / { try_files $uri $uri/ =404; }
    }
}
NGX
sed -i "s|PUBLICDIR|$(pwd)/public|" /tmp/nginx_bench.conf

nginx -c /tmp/nginx_bench.conf
sleep 1

echo "=== nginx (1 worker, sendfile, epoll) ==="
echo "--- 1KB ---"
wrk -t$THR -c$C -d${T}s http://127.0.0.1:8081/$F1 2>&1 | grep -E 'Requests/sec|Latency|Transfer/sec'

echo "--- 100KB ---"
wrk -t$THR -c$C -d${T}s http://127.0.0.1:8081/$F100 2>&1 | grep -E 'Requests/sec|Latency|Transfer/sec'

echo "--- 1MB ---"
wrk -t$THR -c$C -d${T}s http://127.0.0.1:8081/$F1M 2>&1 | grep -E 'Requests/sec|Latency|Transfer/sec'

nginx -c /tmp/nginx_bench.conf -s stop 2>/dev/null
echo "=== DONE ==="
