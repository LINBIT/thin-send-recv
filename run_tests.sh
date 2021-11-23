#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

if [ -z "$VG" ]; then
    echo -e "You need to pass a LVM volume group with at least 12MByte free space\n" \
	"in the environment variable \"VG\"." 1>&2
    exit 10
fi

if [ $UID != 0 ]; then
    echo "The tests need to run as root" 1>&2
    exit 10
fi

result=0
for TEST in tests/*; do
    echo -n running $TEST
    out=$( $TEST < /dev/null 2>&1 )
    ex=$?
    if [[ $ex = 0 ]]; then
	echo -e " ${GREEN}success${NC}"
    else
	result=$ex
	echo -e " ${RED}failure${NC} [$ex]"
	echo "$out"
	echo "===="
	echo "re-running failed test with bash -x"
	bash -x $TEST
    fi
done
exit $result
