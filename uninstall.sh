#!/system/bin/sh
# On removal, put the running IMS process back to its original code.
MODDIR=${0%/*}
. "$MODDIR/common.sh"
pid=$($BB pidof $PROC)
[ -n "$pid" ] && lib_ok && apply "$pid" revert
