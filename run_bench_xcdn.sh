#!/bin/bash
cd "$(dirname "$0")"

pkill -f './xcdn' 2>/dev/null
sleep 1

F1="test1k.bin"
F100="test100k.bin"
F1M="test1m.bin"
T=5
C=100
THR=4

W=${1:-1}
M=${2:-disabled}

echo "=== X-CDN (workers=$W, mode=$M) ==="
./xcdn --port 8080 --dir public --workers $W --uring-mode $M &
XPID=$!
sleep 1

echo "--- 1KB ---"
wrk -t$THR -c$C -d${T}s http://127.0.0.1:8080/$F1 2>&1 | grep -E 'Requests/sec|Latency|Transfer/sec'

echo "--- 100KB ---"
wrk -t$THR -c$C -d${T}s http://127.0.0.1:8080/$F100 2>&1 | grep -E 'Requests/sec|Latency|Transfer/sec'

echo "--- 1MB ---"
wrk -t$THR -c$C -d${T}s http://127.0.0.1:8080/$F1M 2>&1 | grep -E 'Requests/sec|Latency|Transfer/sec'

kill -9 $XPID 2>/dev/null
wait $XPID 2>/dev/null
echo "=== DONE ==="
