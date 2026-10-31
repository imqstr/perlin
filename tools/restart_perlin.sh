#!/usr/bin/env bash
# Rebuild Perlin, restart the client against the local BDS, and prove the
# running process is the binary that was just built.
#
# The client is stopped by exact process name: a "pgrep -f build/Perlin" pattern
# also matches the command line of whatever shell is running this script.
# SIGTERM first, because a -9 kill loses unflushed options.json.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo=$PWD
build=${PERLIN_BUILD_DIR:-$repo/build}
log=$repo/output/bds-client-validation.log

if [ "${1:-}" != "--no-build" ]; then
    echo "== building"
    cmake --build "$build" -j"$(nproc)" 2>&1 | grep -E "error:|FAILED|ninja: build stopped" && {
        echo "!! build failed, leaving the running client alone"
        exit 1
    }
fi
[ -x "$build/Perlin" ] || { echo "!! no binary at $build/Perlin"; exit 1; }

if pgrep -x Perlin >/dev/null; then
    echo "== stopping client"
    pkill -TERM -x Perlin
    for _ in $(seq 10); do
        sleep 1
        pgrep -x Perlin >/dev/null || break
    done
    pgrep -x Perlin >/dev/null && { echo "   did not exit, forcing"; pkill -9 -x Perlin; sleep 2; }
fi
pgrep -x Perlin >/dev/null && { echo "!! client still running"; exit 1; }

pgrep -x bedrock_server >/dev/null || echo "note: bedrock_server is not running, --join will not connect"

mkdir -p "$repo/output"
[ -f "$log" ] && mv "$log" "$repo/output/bds-client-validation-prev.log"

echo "== starting client"
cd "$repo" || exit 1
nohup env PERLIN_RENDERER=vulkan mullvad-exclude ./build/Perlin \
    --join 127.0.0.1:19132 --login --control-port 18765 > "$log" 2>&1 &

for _ in $(seq 20); do
    sleep 1
    pgrep -x Perlin >/dev/null && break
done
pid=$(pgrep -x Perlin | head -1)
[ -n "$pid" ] || { echo "!! client did not start; see $log"; tail -20 "$log"; exit 1; }

# Freshness check: the process has to have started after the binary was written.
built=$(stat -c %Y "$build/Perlin")
started=$(date -d "$(ps -o lstart= -p "$pid")" +%s)
echo "== pid $pid; binary $(date -d @"$built" '+%H:%M:%S'), process $(date -d @"$started" '+%H:%M:%S')"
if [ "$started" -lt "$built" ]; then
    echo "!! the running process predates the binary: it is NOT the new build"
    exit 1
fi
echo "== running the current build"
