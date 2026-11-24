#!/system/bin/sh
# KernelSU manager "Action" button: toggles the patch on the running IMS process.
MODDIR=${0%/*}
. "$MODDIR/common.sh"

if ! lib_ok; then
    echo "libimsstack.so is not the known build; nothing to do."
    exit 0
fi

if [ "$(cat "$STATE" 2>/dev/null)" = off ]; then
    echo on > "$STATE"; mode=patch
else
    echo off > "$STATE"; mode=revert
fi

pid=$($BB pidof $PROC)
if [ -z "$pid" ]; then
    echo "State: $(cat "$STATE"). IMS process not running; the service applies it when it starts."
    exit 0
fi

apply "$pid" "$mode"
case $? in
    0) echo "State: $(cat "$STATE") (applied to pid $pid)." ;;
    *) echo "State: $(cat "$STATE"), but applying to pid $pid failed; see $LOG." ;;
esac
