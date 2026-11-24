#!/system/bin/sh
# Runs at boot (late_start). Watches the IMS process and keeps the patch applied to
# whichever instance is running, since a restarted process loads a fresh,
# unpatched copy of the library.
MODDIR=${0%/*}
. "$MODDIR/common.sh"

[ -f "$STATE" ] || echo on > "$STATE"

(
    done_keys=""
    while true; do
        found=0
        for pid in $(ims_pids); do
            found=1
            mode=patch
            [ "$(cat "$STATE" 2>/dev/null)" = off ] && mode=revert
            key="$pid:$mode"
            case " $done_keys " in *" $key "*) continue ;; esac
            out=$(apply "$pid" "$mode")
            rc=$?
            # rc 1: library not mapped yet, so try again next round.
            # Anything else is final for this pid and mode; log it once.
            if [ $rc -ne 1 ]; then
                log "pid $pid $mode: rc=$rc $out"
                done_keys="$done_keys $key"
            fi
        done
        if [ $found = 1 ]; then $BB sleep 5; else $BB sleep 2; fi
    done
) &
