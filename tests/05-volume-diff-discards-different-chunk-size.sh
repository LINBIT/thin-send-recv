#!/bin/bash
set -o errexit
set -o pipefail

[ -z "$VG" ] && exit 10

# source pool with smaller chunks than the target pool, discards on the
# source cover only parts of the target's chunks
lvcreate --type thin-pool -L 4M --chunksize 64k --thinpool tpool_src $VG
lvcreate --type thin-pool -L 4M --chunksize 256k --thinpool tpool_tgt $VG
lvcreate --type thin -V 8M --thinpool tpool_src -n tlv_source $VG
lvcreate --type thin -V 8M --thinpool tpool_tgt -n tlv_target $VG

dd if=<(yes "DO NOT WRITE TO SNAPSHOTS!") of=/dev/$VG/tlv_source bs=64k count=16 iflag=fullblock oflag=direct

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source0
./thin_send /dev/$VG/tlv_source | ./thin_recv /dev/$VG/tlv_target

# within one target chunk
blkdiscard -v -o 0 -l 64k /dev/$VG/tlv_source
# partial head chunk, one whole chunk, partial tail chunk
blkdiscard -v -o $((192*1024)) -l $((384*1024)) /dev/$VG/tlv_source

lvcreate --snapshot /dev/$VG/tlv_source -n snap_source1
./thin_send /dev/$VG/snap_source0 /dev/$VG/snap_source1 | ./thin_recv /dev/$VG/tlv_target

md5_source=($(md5sum /dev/$VG/tlv_source))
md5_target=($(md5sum /dev/$VG/tlv_target))

[ "$md5_source" = "$md5_target" ] || exit 10

lvremove --force /dev/$VG/snap_source0
lvremove --force /dev/$VG/snap_source1
lvremove --force /dev/$VG/tlv_source
lvremove --force /dev/$VG/tlv_target
lvremove --force /dev/$VG/tpool_src
lvremove --force /dev/$VG/tpool_tgt

exit 0
