#!/bin/bash
cd "$(dirname "$0")"
W=${1:-1}
M=${2:-disabled}
T=${3:-10}
C=${4:-100}

pkill -9 -f './xcdn' 2>/dev/null; sleep 1

echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
echo "=== X-CDN workers=$W mode=$M conns=$C dur=${T}s ==="

# Сервер на ядро 0, wrk на 1-3
taskset -c 0 ./xcdn --port 8080 --dir public --workers $W --uring-mode $M &
XPID=$!
sleep 1
trap "kill -9 $XPID 2>/dev/null" EXIT

for f in test1k.bin test100k.bin test1m.bin; do
  # Прогрев
  wrk -t2 -c$C -d3s http://127.0.0.1:8080/$f > /dev/null 2>&1
  sleep 1
  echo "--- $f ---"
  taskset -c 1-3 wrk -t3 -c$C -d${T}s --latency http://127.0.0.1:8080/$f
  sleep 2
done
echo "DONE"