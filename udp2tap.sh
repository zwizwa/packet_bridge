#!/bin/bash
IF=udp2tap
($(dirname $0)/udp2tap.elf $IF) &
sleep 1
ifconfig $IF up
brctl addif br0 $IF
wait



