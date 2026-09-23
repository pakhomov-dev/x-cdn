#!/bin/bash
cd "$(dirname "$0")"
pkill -f './xcdn' 2>/dev/null
sleep 0.5
./xcdn --uring-mode polling -p 8090 -d public --workers 1 &
XPID=$!
sleep 0.5
for f in test1k.bin test100k.bin test1m.bin; do
  curl -s -o /dev/null -w "$f %{http_code} %{size_download}\n" "http://127.0.0.1:8090/$f"
done
echo -n "keepalive: "
curl -s -o /dev/null -w "%{http_code} " "http://127.0.0.1:8090/test1m.bin"
curl -s -o /dev/null -w "%{http_code}\n" "http://127.0.0.1:8090/test100k.bin"
kill $XPID 2>/dev/null
wait $XPID 2>/dev/null
