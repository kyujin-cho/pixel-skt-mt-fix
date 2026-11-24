# Shared helpers. Sourced by service.sh and action.sh; runs under KernelSU's busybox ash.
#
# What gets patched: libimsstack.so, IdleState::OnAttached():
#   0xa83ac4  bl  MtcExtensionSet::IsAvailableOnBoth("100rel")
#   0xa83ac8  tbz w0, #0, 0xa83bac        <- replaced with: b 0xa83bac
# With the branch taken unconditionally, an incoming INVITE goes straight to
# MtcCallState::OnReadyToAlert() (plain 180, no SDP) instead of sending the SDP
# answer in a reliable 183. The answer then goes into the 200 OK.
# Only this call site changes; other users of IsAvailableOnBoth are untouched.
# The patch lives only in the running process's memory (written through
# /proc/<pid>/mem); nothing on disk changes.

BB=/data/adb/ksu/bin/busybox
MODDIR=${MODDIR:-/data/adb/modules/pixel_skt_mt_fix}
LOG=$MODDIR/patch.log
STATE=$MODDIR/state            # "on" or "off"; action.sh flips it
PROC=com.android.imsstack
LIB=/system_ext/lib64/libimsstack.so

# Known build: sha256 of libimsstack.so (Pixel 11 Pro XL, CD1A.261005.003).
# Any other build is left alone, since the offset would be wrong.
KNOWN_SHA=ec66ef900b4840e6a550ae3775e5d28c8cd83d6283655ec7bfdea21967891a06
SITE=0xa83ac4

# Bytes at SITE: bl (99 9b 06 94) + tbz (20 07 00 36); patched: bl + b (39 00 00 14)
ORIG="99 9b 06 94 20 07 00 36"
PATCHED="99 9b 06 94 39 00 00 14"

log() { echo "$($BB date '+%F %T') $*" >> "$LOG"; }

lib_ok() {
    [ "$($BB sha256sum "$LIB" | $BB cut -d' ' -f1)" = "$KNOWN_SHA" ]
}

site_addr() { # $1 = pid
    base=$($BB grep -m1 "$LIB" /proc/$1/maps | $BB cut -d- -f1)
    [ -n "$base" ] || return 1
    echo $((0x$base + SITE))
}

read_site() { # $1 = pid, $2 = addr -> "99 9b 06 94 ..."
    $BB dd if=/proc/$1/mem bs=1 skip=$2 count=8 2>/dev/null | $BB od -An -tx1 | $BB xargs
}

write_branch() { # $1 = pid, $2 = addr, $3 = "patch" | "revert"
    if [ "$3" = patch ]; then
        printf '\071\000\000\024' | $BB dd of=/proc/$1/mem bs=1 seek=$(($2 + 4)) conv=notrunc 2>/dev/null
    else
        printf '\040\007\000\066' | $BB dd of=/proc/$1/mem bs=1 seek=$(($2 + 4)) conv=notrunc 2>/dev/null
    fi
}

# Bring the running process to the wanted state.
# Returns 0 when it is there, 1 if the lib isn't mapped yet, 2 on unknown bytes, 3 if the write failed.
apply() { # $1 = pid, $2 = "patch" | "revert"
    addr=$(site_addr "$1") || return 1
    if [ "$2" = patch ]; then want=$PATCHED; from=$ORIG; else want=$ORIG; from=$PATCHED; fi
    cur=$(read_site "$1" "$addr")
    [ "$cur" = "$want" ] && return 0
    if [ "$cur" != "$from" ]; then
        log "pid $1: unexpected bytes [$cur], not touching"
        return 2
    fi
    write_branch "$1" "$addr" "$2"
    now=$(read_site "$1" "$addr")
    if [ "$now" = "$want" ]; then
        log "pid $1: $2 ok at $(printf %x "$addr")"
        return 0
    fi
    log "pid $1: $2 FAILED, bytes now [$now]"
    return 3
}
