#!/bin/bash
# compination of 01-volume-diff.sh and 02-volume-diff-with-discards.sh
# but with some added fun

set -x
set -o errexit
set -o pipefail

[ -z "$VG" ] && exit 10

lvcreate --type thin-pool -L 12M --thinpool tpool $VG
lvcreate --type thin -V 100M --thinpool tpool -n tlv_source $VG
lvcreate --type thin -V 100M --thinpool tpool -n tlv_target $VG

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source0

for i in $(seq 0 4); do
    offset=$(($RANDOM % 1600))
    dd if=<(echo "hi there") of=/dev/$VG/tlv_source bs=64k count=1 seek=$offset oflag=direct
done
discard_this=$offset

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source1
./thin_send /dev/$VG/snap_source0 /dev/$VG/snap_source1 | ./thin_recv /dev/$VG/tlv_target

for i in $(seq 0 4); do
    offset=$((RANDOM % 1600))
    date "+%s hi there, i=$i, offset=$offset" | dd of=/dev/$VG/tlv_source bs=64k \
	count=1 seek=$offset conv=fsync,sync
done

# discard the last offset
blkdiscard -v -l 64k -o $(( discard_this * 64 ))k /dev/$VG/tlv_source

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source2

# for the fun of it, write some more to snap_source1, even after we created snap_source2
for i in $(seq 0 4); do
    offset=$((RANDOM % 1600))
    date "+%s hi there, i=$i, offset=$offset" | dd of=/dev/$VG/snap_source1 bs=64k \
	count=1 seek=$offset conv=fsync,sync
done

D=$(mktemp)
./thin_send /dev/$VG/snap_source1 /dev/$VG/snap_source2 > "$D"
<"$D" ./thin_recv /dev/$VG/tlv_target

md5_source=($(md5sum /dev/$VG/tlv_source))
md5_target=($(md5sum /dev/$VG/tlv_target))

[ "$md5_source" = "$md5_target" ] || exit 10

size=$(stat -c %s "$D")

: empty input should not be accepted
! </dev/null ./thin_recv /dev/$VG/tlv_target || exit 10

: truncated input should not be accepted
! <"$D" head -c 27 | ./thin_recv /dev/$VG/tlv_target  || exit 10
! <"$D" head -c 28 | ./thin_recv /dev/$VG/tlv_target  || exit 10
! <"$D" head -c $(( size - 512 )) | ./thin_recv /dev/$VG/tlv_target  || exit 10
! <"$D" head -c $(( size -  52 )) | ./thin_recv /dev/$VG/tlv_target  || exit 10
! <"$D" head -c $(( size -  24 )) | ./thin_recv /dev/$VG/tlv_target  || exit 10
! <"$D" head -c $(( size -   1 )) | ./thin_recv /dev/$VG/tlv_target  || exit 10

: chunk stat mismatch should be detected and rejected
! ( <"$D" head -c $(( size -  24 )); head -c 24 /dev/zero ) | ./thin_recv /dev/$VG/tlv_target  || exit 10

: trailing garbage should not be accepted
! ( cat "$D" ; echo dreck ) | ./thin_recv /dev/$VG/tlv_target  || exit 10
! ( cat "$D" ; head -c 32 /dev/zero ) | ./thin_recv /dev/$VG/tlv_target  || exit 10

rm -f "$D"
# --force removing the thin pool should be enough
# lvremove --force /dev/$VG/snap_source0
# lvremove --force /dev/$VG/snap_source1
# lvremove --force /dev/$VG/snap_source2
# lvremove --force /dev/$VG/tlv_source
# lvremove --force /dev/$VG/tlv_target
lvremove --force /dev/$VG/tpool

exit 0
