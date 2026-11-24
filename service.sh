#!/system/bin/sh
# Runs at boot (late_start). Watches com.android.imsstack and keeps the patch
# applied to whichever process instance is running, since a restart of the IMS
# process loads a fresh, unpatched copy of the library.
MODDIR=${0%/*}
. "$MODDIR/common.sh"

[ -f "$STATE" ] || echo on > "$STATE"

if ! lib_ok; then
    log "libimsstack.so hash differs from the known build; module idle (offset unknown)"
    exit 0
fi

(
    last=""
    while true; do
        pid=$($BB pidof $PROC)
        if [ -n "$pid" ]; then
            mode=patch
            [ "$(cat "$STATE" 2>/dev/null)" = off ] && mode=revert
            key="$pid:$mode"
            if [ "$key" != "$last" ]; then
                apply "$pid" "$mode"
                rc=$?
                # 1 = library not mapped yet: try again next round
                [ $rc -ne 1 ] && last=$key
            fi
            $BB sleep 5
        else
            $BB sleep 2
        fi
    done
) &
