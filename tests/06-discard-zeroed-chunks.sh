#!/bin/bash
set -o errexit
set -o pipefail

[ -z "$VG" ] && exit 10

# source pool with smaller chunks than the target pool. thin_recv discards
# a target chunk that is zero outside a range unmapped on the source.
lvcreate --type thin-pool -L 4M --chunksize 64k --thinpool tpool_src $VG
lvcreate --type thin-pool -L 4M --chunksize 256k --thinpool tpool_tgt $VG
lvcreate --type thin -V 8M --thinpool tpool_src -n tlv_source $VG
lvcreate --type thin -V 8M --thinpool tpool_tgt -n tlv_target $VG

dd if=<(yes "DO NOT WRITE TO SNAPSHOTS!") of=/dev/$VG/tlv_source bs=64k count=16 iflag=fullblock oflag=direct

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source0
./thin_send /dev/$VG/tlv_source | ./thin_recv /dev/$VG/tlv_target

# across the boundary of the first two target chunks, both keep data
blkdiscard -v -o $((192*1024)) -l $((128*1024)) /dev/$VG/tlv_source
# the end of the fourth target chunk, it keeps data
blkdiscard -v -o $((832*1024)) -l $((192*1024)) /dev/$VG/tlv_source

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source1
./thin_send /dev/$VG/snap_source0 /dev/$VG/snap_source1 | ./thin_recv /dev/$VG/tlv_target

md5_source=($(md5sum /dev/$VG/tlv_source))
md5_target=($(md5sum /dev/$VG/tlv_target))
[ "$md5_source" = "$md5_target" ] || exit 10

# the rest, now all target chunks contain only zeroes; the second range
# starts and ends within a target chunk
blkdiscard -v -o 0 -l $((192*1024)) /dev/$VG/tlv_source
blkdiscard -v -o $((320*1024)) -l $((512*1024)) /dev/$VG/tlv_source

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source2
./thin_send /dev/$VG/snap_source1 /dev/$VG/snap_source2 | ./thin_recv /dev/$VG/tlv_target

md5_source=($(md5sum /dev/$VG/tlv_source))
md5_target=($(md5sum /dev/$VG/tlv_target))
[ "$md5_source" = "$md5_target" ] || exit 10

data_percent=$(lvs --noheadings -o data_percent $VG/tlv_target | tr -d ' ')
[ "$data_percent" = "0.00" ] || exit 10

lvremove --force /dev/$VG/snap_source0
lvremove --force /dev/$VG/snap_source1
lvremove --force /dev/$VG/snap_source2
lvremove --force /dev/$VG/tlv_source
lvremove --force /dev/$VG/tlv_target
lvremove --force /dev/$VG/tpool_src
lvremove --force /dev/$VG/tpool_tgt

exit 0
