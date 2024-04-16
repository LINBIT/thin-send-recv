#!/bin/bash
set -o errexit
set -o pipefail

[ -z "$VG" ] && exit 10

function run_and_kill_with()
{
    echo Testing $1
    gdb --quiet --args ./thin_send --send /dev/$VG/tlv_source <<EOF > /dev/null
break reserve_metadata_snap
run
finish
delete 1
signal $1
EOF
}

lvcreate --type thin-pool -L 12M --thinpool tpool $VG
lvcreate --type thin -V 100M --thinpool tpool -n tlv_source $VG

run_and_kill_with SIGKILL
# verify that after SIGKILL the meta-data is reseved
dmsetup message /dev/mapper/$VG-tpool-tpool 0 release_metadata_snap || exit 10

for signal in SIGABRT SIGALRM SIGBUS SIGFPE SIGHUP SIGPIPE \
		      SIGPWR SIGQUIT SIGSEGV SIGTERM SIGUSR1 \
		      SIGUSR2 SIGXCPU SIGXFSZ; do
    run_and_kill_with $signal

    # In case it was not release the following reserve will fail:
    dmsetup message /dev/mapper/$VG-tpool-tpool 0 reserve_metadata_snap || exit 10
    dmsetup message /dev/mapper/$VG-tpool-tpool 0 release_metadata_snap
done

lvremove --force /dev/$VG/tlv_source
lvremove --force /dev/$VG/tpool

exit 0
