#!/usr/bin/env bash
# PROTOTYPE: launch the rebuilt host for a browser session. Prints pid + port, keeps running until killed.
set -euo pipefail
APP=/tmp/sm-host-proto/StreamMateStudioHost.app
TOKEN="${TOKEN:-protok}"
STATE=/tmp/sm-proto-state.json
LOG=/tmp/sm-proto-host.log
pkill -f "$APP/Contents/MacOS/studio-host" 2>/dev/null || true
sleep 0.5
rm -f "$STATE"
"$APP/Contents/MacOS/studio-host" --token "$TOKEN" --host 127.0.0.1 --port 0 --state-file "$STATE" --allow-live-egress > "$LOG" 2>&1 &
PID=$!
for i in $(seq 1 60); do
  if [[ -f "$STATE" ]] && grep -q '"ready"' "$STATE"; then break; fi
  sleep 0.25
done
PORT=$(python3 -c 'import json;print(json.load(open("/tmp/sm-proto-state.json"))["port"])')
echo "pid=$PID port=$PORT token=$TOKEN log=$LOG"
echo "$PID" > /tmp/sm-proto-host.pid
