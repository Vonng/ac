#!/bin/sh

# usage:  $ramdisk.sh [capacity=1] [label=ramdisk]
# mount a ramdisk device with given capacity

# default capacity=1(gb)
capacity=1
if [ "$1" != "" ]; then
	capacity=$1
fi

# default label="ramdisk"
label="ramdisk"
if [ "$2" != "" ]; then
	label="$2"
fi

# echo "capacity=$capacity\nlabel=$label"

# 1(capacity)=2097152(ram)
ram=$(($capacity*2097152))

if ! test -e "/Volumes/$label" ; then
    diskutil erasevolume HFS+ "$label" `hdiutil attach -nomount ram://$ram`
fi
