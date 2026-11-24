#!/system/bin/sh
# On removal, put the running IMS process back to its original code.
MODDIR=${0%/*}
. "$MODDIR/common.sh"
for pid in $(ims_pids); do
    apply "$pid" revert >/dev/null
done
