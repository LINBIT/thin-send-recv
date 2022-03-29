#!/bin/bash
set -o errexit
set -o pipefail

[ -z "$VG" ] && exit 10

lvcreate --type thin-pool -L 12M --thinpool tpool $VG
lvcreate --type thin -V 100M --thinpool tpool -n tlv_source $VG
lvcreate --type thin -V 100M --thinpool tpool -n tlv_target $VG

for i in $(seq 0 9); do
    offset=$((RANDOM % 1600))
    date "+%s hi there, i=$i, offset=$offset" | dd of=/dev/$VG/tlv_source bs=64k \
	count=1 seek=$offset conv=fsync,sync
done

./thin_send /dev/$VG/tlv_source | ./thin_recv /dev/$VG/tlv_target
md5_source=($(md5sum /dev/$VG/tlv_source))
md5_target=($(md5sum /dev/$VG/tlv_target))

[ "$md5_source" = "$md5_target" ] || exit 10

lvremove --force /dev/$VG/tlv_source
lvremove --force /dev/$VG/tlv_target
lvremove --force /dev/$VG/tpool

exit 0
