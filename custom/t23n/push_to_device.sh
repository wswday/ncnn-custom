#!/bin/bash
# push_to_device.sh LOCAL_FILE [REMOTE_NAME] -> /mnt/sdcard/trim_test/
# Host listens; device pulls via its nc client (runs inside the telnet session).
set -uo pipefail
T="$(cd "$(dirname "$0")" && pwd)"   # telnet_run.py lives next to this script
LOCAL=$1
REMOTE=${2:-$(basename "$1")}
[ -s "$LOCAL" ] || { echo "ABORT: local file missing or empty: $LOCAL" >&2; exit 1; }
DIR=/mnt/sdcard/trim_test
PORT=$((13400 + RANDOM % 500))

want=$(md5sum "$LOCAL" | cut -d' ' -f1)

nc -l -p "$PORT" -N < "$LOCAL" > /dev/null 2>&1 &
lpid=$!
sleep 0.5

out=$(python3 "$T/telnet_run.py" -t 300 "mkdir -p $DIR; rm -f $DIR/$REMOTE; nc -w 15 192.168.66.1 $PORT > $DIR/$REMOTE; md5sum $DIR/$REMOTE; ls -l $DIR/$REMOTE")
rc=$?
kill $lpid 2>/dev/null
echo "$out"
if echo "$out" | grep -q "$want"; then
    echo "PUSHED OK: $REMOTE"
    exit 0
else
    echo "PUSH FAILED (want md5 $want, rc $rc)"
    exit 1
fi
