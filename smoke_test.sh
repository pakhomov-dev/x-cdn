#!/bin/bash
set -e
cd "$(dirname "$0")"
pkill -9 -f './xcdn' 2>/dev/null || true
sleep 0.5

./xcdn --port 8090 --dir public --workers 1 --uring-mode disabled &
XPID=$!
sleep 1
trap "kill -9 $XPID 2>/dev/null" EXIT

for f in test1k.bin test100k.bin test1m.bin; do
  curl -s -o /tmp/dl.bin "http://127.0.0.1:8090/$f"
  a=$(md5sum < /tmp/dl.bin | cut -d' ' -f1)
  b=$(md5sum < public/$f | cut -d' ' -f1)
  if [ "$a" = "$b" ]; then
    echo "OK   $f"
  else
    echo "FAIL $f  got=$a want=$b"
  fi
done

echo "DONE"