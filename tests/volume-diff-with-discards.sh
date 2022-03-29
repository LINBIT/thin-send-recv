#!/bin/bash
set -o errexit
set -o pipefail

[ -z "$VG" ] && exit 10

lvcreate --type thin-pool -L 12M --thinpool tpool $VG
lvcreate --type thin -V 100M --thinpool tpool -n tlv_source $VG
lvcreate --type thin -V 100M --thinpool tpool -n tlv_target $VG

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source0

discard_these_64k_offsets=""

for i in $(seq 0 4); do
    offset_64k=$(($RANDOM % 1600))

    dd if=<(echo "hi there") of=/dev/$VG/tlv_source bs=64k count=1 seek=$offset_64k oflag=direct
    discard_these_64k_offsets+="$offset_64k "
done

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source1
./thin_send /dev/$VG/snap_source0 /dev/$VG/snap_source1 | ./thin_recv /dev/$VG/tlv_target

for i in $(seq 0 4); do
    offset=$((RANDOM % 1600))
    date "+%s hi there, i=$i, offset=$offset" | dd of=/dev/$VG/tlv_source bs=64k \
	count=1 seek=$offset conv=fsync,sync
done

for o in $discard_these_64k_offsets; do
	blkdiscard -v -l 64k -o $(( o * 64*1024 )) /dev/$VG/tlv_source
done

sync

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source2
./thin_send /dev/$VG/snap_source1 /dev/$VG/snap_source2 | ./thin_recv /dev/$VG/tlv_target

md5_source=($(md5sum /dev/$VG/tlv_source))
md5_target=($(md5sum /dev/$VG/tlv_target))

[ "$md5_source" = "$md5_target" ] || exit 10

lvremove --force /dev/$VG/snap_source0
lvremove --force /dev/$VG/snap_source1
lvremove --force /dev/$VG/snap_source2
lvremove --force /dev/$VG/tlv_source
lvremove --force /dev/$VG/tlv_target
lvremove --force /dev/$VG/tpool

exit 0
