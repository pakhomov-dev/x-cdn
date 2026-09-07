#!/bin/bash
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

XCDN_PORT=8080
NGINX_PORT=8081
ROUNDS=${1:-3}
WRK_THREADS=2
WRK_CONNECTIONS=100
WRK_DURATION=10
WORKDIR=$(cd "$(dirname "$0")" && pwd)
PUBLIC_DIR="$WORKDIR/public"
RESULTS_FILE="$WORKDIR/results.txt"

print_banner() {
    echo -e "${CYAN}"
    echo "================================================="
    echo "       X-CDN vs nginx Benchmark Suite"
    echo "       Rounds: $ROUNDS  |  wrk: ${WRK_THREADS}t/${WRK_CONNECTIONS}c/${WRK_DURATION}s"
    echo "================================================="
    echo -e "${NC}"
}

install_deps() {
    echo -e "${YELLOW}[1/5] Installing dependencies...${NC}"
    apt-get update -qq
    apt-get install -y -qq g++ make liburing-dev libssl-dev nginx wrk build-essential curl
    echo -e "${GREEN}  Done${NC}"
}

build_xcdn() {
    echo -e "${YELLOW}[2/5] Building X-CDN...${NC}"
    make -C "$WORKDIR" clean 2>/dev/null || true
    make -C "$WORKDIR" -j$(nproc)
    echo -e "${GREEN}  Done${NC}"
}

generate_files() {
    echo -e "${YELLOW}[3/5] Generating test files...${NC}"
    mkdir -p "$PUBLIC_DIR"
    [ -f "$PUBLIC_DIR/index.html" ] || echo '<!DOCTYPE html><html><body><h1>It works!</h1></body></html>' > "$PUBLIC_DIR/index.html"
    [ -f "$PUBLIC_DIR/404.html" ] || echo '<h1>404 Not Found</h1>' > "$PUBLIC_DIR/404.html"
    dd if=/dev/urandom of="$PUBLIC_DIR/test1k.bin" bs=1024 count=1 2>/dev/null
    dd if=/dev/urandom of="$PUBLIC_DIR/test100k.bin" bs=1024 count=100 2>/dev/null
    dd if=/dev/urandom of="$PUBLIC_DIR/test1m.bin" bs=1048576 count=1 2>/dev/null
    echo -e "${GREEN}  Done${NC}"
}

setup_nginx() {
    echo -e "${YELLOW}[4/5] Configuring nginx...${NC}"
    nginx -s stop 2>/dev/null || true
    sleep 1
    cat > /tmp/nginx_bench.conf << NGXCONF
worker_processes auto;
pid /tmp/nginx_bench.pid;
worker_rlimit_nofile 65535;
events {
    worker_connections 4096;
    use epoll;
    multi_accept on;
}
http {
    include /etc/nginx/mime.types;
    default_type application/octet-stream;
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
        listen ${NGINX_PORT} default_server reuseport;
        server_name localhost;
        root ${PUBLIC_DIR};
        location / {
            try_files \$uri \$uri/ =404;
        }
    }
}
NGXCONF
    echo -e "${GREEN}  Done${NC}"
}

tune_system() {
    echo -e "${YELLOW}[5/5] Tuning system...${NC}"
    ulimit -n 65535 2>/dev/null || true
    ulimit -l unlimited 2>/dev/null || true
    sysctl -w net.core.somaxconn=65535 2>/dev/null || true
    sysctl -w net.core.netdev_max_backlog=65535 2>/dev/null || true
    sysctl -w net.ipv4.tcp_max_syn_backlog=65535 2>/dev/null || true
    sysctl -w net.ipv4.ip_local_port_range="1024 65535" 2>/dev/null || true
    sysctl -w net.ipv4.tcp_tw_reuse=1 2>/dev/null || true
    sysctl -w fs.file-max=2097152 2>/dev/null || true
    echo -e "${GREEN}  Done${NC}"
}

kill_xcdn() {
    pkill -f './xcdn' 2>/dev/null || true
    sleep 0.5
}

kill_nginx() {
    nginx -s stop 2>/dev/null || true
    sleep 0.5
}

run_wrk() {
    wrk -t${WRK_THREADS} -c${WRK_CONNECTIONS} -d${WRK_DURATION}s "$1" 2>&1
}

parse_rps() {
    echo "$1" | grep 'Requests/sec:' | awk '{print $2}'
}

parse_latency() {
    echo "$1" | grep 'Latency' | head -1 | awk '{print $2}'
}

calc_avg() {
    echo "$1" | tr ' ' '\n' | grep -v '^$' | awk '{s+=$1; n++} END {if(n>0) printf "%.0f", s/n; else print "0"}'
}

print_result() {
    local file="$1" label="$2"
    local avg_xcdn avg_nginx diff_pct diff_str color
    avg_xcdn=$(calc_avg "${RPS_XCDN[$file]}")
    avg_nginx=$(calc_avg "${RPS_NGINX[$file]}")
    if [ "$avg_nginx" -gt 0 ] 2>/dev/null; then
        diff_pct=$(( (avg_xcdn - avg_nginx) * 100 / avg_nginx ))
    else
        diff_pct=0
    fi
    if [ "$diff_pct" -ge 0 ]; then
        diff_str="+${diff_pct}%"
        color="${GREEN}"
    else
        diff_str="${diff_pct}%"
        color="${RED}"
    fi
    printf "${CYAN}|${NC} %-8s | %12s   | %12s   | ${color}%-8s${NC}   ${CYAN}|${NC}\n" \
        "$label" "${avg_xcdn} rps" "${avg_nginx} rps" "$diff_str"
}

print_lat() {
    local file="$1" label="$2"
    local avg_lat_xcdn avg_lat_nginx
    avg_lat_xcdn=$(calc_avg "${LAT_XCDN[$file]}")
    avg_lat_nginx=$(calc_avg "${LAT_NGINX[$file]}")
    printf "${CYAN}|${NC} %-8s | %12s   | %12s   |          ${CYAN}|${NC}\n" \
        "$label" "${avg_lat_xcdn}" "${avg_lat_nginx}"
}

print_banner

if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Please run as root: sudo ./benchmark.sh${NC}"
    exit 1
fi

cd "$WORKDIR"

install_deps
build_xcdn
generate_files
setup_nginx
tune_system

declare -A RPS_XCDN RPS_NGINX LAT_XCDN LAT_NGINX
FILES=("test1k.bin" "test100k.bin" "test1m.bin")
FILE_LABELS=("1 KB" "100 KB" "1 MB")

echo -e "${YELLOW}Warmup run...${NC}"
./xcdn --config config.yaml --metrics &
sleep 1
wrk -t2 -c50 -d3s "http://127.0.0.1:${XCDN_PORT}/test1k.bin" > /dev/null 2>&1
kill_xcdn
nginx -c /tmp/nginx_bench.conf 2>/dev/null
sleep 1
wrk -t2 -c50 -d3s "http://127.0.0.1:${NGINX_PORT}/test1k.bin" > /dev/null 2>&1
kill_nginx
echo -e "${GREEN}  Warmup done${NC}"
echo ""

for ((round=1; round<=ROUNDS; round++)); do
    echo -e "${BOLD}--- Round $round / $ROUNDS ---${NC}"

    ./xcdn --config config.yaml --metrics &
    sleep 1

    for i in "${!FILES[@]}"; do
        file="${FILES[$i]}"
        label="${FILE_LABELS[$i]}"
        echo -ne "  X-CDN  ${label}... "
        output=$(run_wrk "http://127.0.0.1:${XCDN_PORT}/${file}")
        rps=$(parse_rps "$output")
        lat=$(parse_latency "$output")
        echo -e "${GREEN}${rps} req/s${NC}  (lat: ${lat})"
        RPS_XCDN["$file"]+=" $rps"
        LAT_XCDN["$file"]+=" $lat"
    done

    kill_xcdn
    sleep 1

    nginx -c /tmp/nginx_bench.conf 2>/dev/null
    sleep 1

    for i in "${!FILES[@]}"; do
        file="${FILES[$i]}"
        label="${FILE_LABELS[$i]}"
        echo -ne "  nginx  ${label}... "
        output=$(run_wrk "http://127.0.0.1:${NGINX_PORT}/${file}")
        rps=$(parse_rps "$output")
        lat=$(parse_latency "$output")
        echo -e "${GREEN}${rps} req/s${NC}  (lat: ${lat})"
        RPS_NGINX["$file"]+=" $rps"
        LAT_NGINX["$file"]+=" $lat"
    done

    kill_nginx
    sleep 2
    echo ""
done

echo ""
echo -e "${CYAN}=================================================${NC}"
echo -e "${CYAN}   FINAL RESULTS (avg $ROUNDS rounds)${NC}"
echo -e "${CYAN}=================================================${NC}"
printf "${CYAN}|${NC} %-8s | %15s | %15s | %10s ${CYAN}|${NC}\n" "Size" "X-CDN rps" "nginx rps" "Diff"
echo -e "${CYAN}+----------+-----------------+-----------------+----------+${NC}"
for i in "${!FILES[@]}"; do
    print_result "${FILES[$i]}" "${FILE_LABELS[$i]}"
done

echo -e "${CYAN}+----------+-----------------+-----------------+----------+${NC}"
printf "${CYAN}|${NC} %-8s | %15s | %15s |          ${CYAN}|${NC}\n" "Size" "X-CDN lat" "nginx lat"
echo -e "${CYAN}+----------+-----------------+-----------------+----------+${NC}"
for i in "${!FILES[@]}"; do
    print_lat "${FILES[$i]}" "${FILE_LABELS[$i]}"
done
echo -e "${CYAN}=================================================${NC}"

{
    echo "X-CDN vs nginx Benchmark Results"
    echo "================================="
    echo "Date: $(date)"
    echo "Server: $(uname -n)"
    echo "CPU: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2)"
    echo "Cores: $(nproc)"
    echo "Kernel: $(uname -r)"
    echo "Rounds: $ROUNDS"
    echo "wrk: ${WRK_THREADS} threads, ${WRK_CONNECTIONS} connections, ${WRK_DURATION}s"
    echo ""
    echo "=== Throughput (requests/sec) ==="
    printf "%-8s %15s %15s %10s\n" "Size" "X-CDN" "nginx" "Diff"
    echo "--------------------------------------"
    for i in "${!FILES[@]}"; do
        file="${FILES[$i]}"
        label="${FILE_LABELS[$i]}"
        avg_xcdn=$(calc_avg "${RPS_XCDN[$file]}")
        avg_nginx=$(calc_avg "${RPS_NGINX[$file]}")
        if [ "$avg_nginx" -gt 0 ] 2>/dev/null; then
            diff_pct=$(( (avg_xcdn - avg_nginx) * 100 / avg_nginx ))
        else
            diff_pct=0
        fi
        printf "%-8s %15s %15s %+d%%\n" "$label" "$avg_xcdn" "$avg_nginx" "$diff_pct"
    done
    echo ""
    echo "=== Latency ==="
    printf "%-8s %15s %15s\n" "Size" "X-CDN" "nginx"
    echo "--------------------------------------"
    for i in "${!FILES[@]}"; do
        file="${FILES[$i]}"
        label="${FILE_LABELS[$i]}"
        avg_lat_xcdn=$(calc_avg "${LAT_XCDN[$file]}")
        avg_lat_nginx=$(calc_avg "${LAT_NGINX[$file]}")
        printf "%-8s %15s %15s\n" "$label" "${avg_lat_xcdn}" "${avg_lat_nginx}"
    done
} > "$RESULTS_FILE"

echo -e "${GREEN}Results saved to $RESULTS_FILE${NC}"
echo -e "${BOLD}Done!${NC}"
