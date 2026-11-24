#!/system/bin/sh
# KernelSU manager "Action" button: toggles the patch on the running IMS process.
MODDIR=${0%/*}
. "$MODDIR/common.sh"

if [ "$(cat "$STATE" 2>/dev/null)" = off ]; then
    echo on > "$STATE"; mode=patch
else
    echo off > "$STATE"; mode=revert
fi
echo "State: $(cat "$STATE")"

pids=$(ims_pids)
if [ -z "$pids" ]; then
    echo "IMS process not running; the service applies the state when it starts."
    exit 0
fi
for pid in $pids; do
    out=$(apply "$pid" "$mode")
    rc=$?
    log "action pid $pid $mode: rc=$rc $out"
    echo "pid $pid: $out"
done
