# Shared helpers. Sourced by service.sh, action.sh and uninstall.sh; runs under
# KernelSU's busybox ash.
#
# The patch itself is done by bin/imspatch, which finds the incoming-call 100rel
# decision in IdleState::OnAttached() of the running IMS process (from exported
# symbols plus an instruction and GOT check, no fixed offsets) and flips the
# branch so the phone sends a plain 180 and answers in the 200 OK. The change
# lives only in process memory; nothing on disk is modified.
# See docs/ims-call-flow-analysis.md.

BB=/data/adb/ksu/bin/busybox
MODDIR=${MODDIR:-/data/adb/modules/pixel_skt_mt_fix}
LOG=$MODDIR/patch.log
STATE=$MODDIR/state            # "on" or "off"; action.sh flips it
PATCHER=$MODDIR/bin/imspatch

# Processes that host the IMS stack (space-separated).
PROCS="com.android.imsstack"

log() { echo "$($BB date '+%F %T') $*" >> "$LOG"; }

ims_pids() {
    for p in $PROCS; do $BB pidof "$p"; done
}

# apply <pid> <patch|revert>: prints the helper's one-line result, returns its exit code
#   0 ok, 1 library not mapped (yet), 2 site not found, 3 I/O error, 4 unexpected code
apply() {
    "$PATCHER" "$1" "$2"
}
