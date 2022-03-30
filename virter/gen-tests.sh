#!/usr/bin/env bash

BASES="${*:-alma-8 centos-7 ubuntu-bionic ubuntu-focal}"

cat <<EOF > vms.toml
name = "thin-send-recv"
provision_file = "provision.toml"
provision_timeout = "10m"
EOF

for BASE in $BASES ; do
  cat <<EOF >> vms.toml
[[vms]]
  vcpus = 1
  memory = "1G"
  base_image = "$BASE"
  disks = ["name=scratch,size=500M,bus=virtio"]
EOF

done

cat <<EOF > tests.toml
test_suite_file = "run-tests.toml"
test_timeout = "1m"

[tests]
EOF

for TEST in ../tests/*.sh; do
  SCRIPT_NAME="$(basename "$TEST")"

  cat <<EOF >> tests.toml
[tests."$SCRIPT_NAME"]
  needallplatforms = true
  vms = [1]
EOF
done
