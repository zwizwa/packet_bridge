#!/bin/bash
IF=udp2tap
PORT=1234

($(dirname $0)/udp2tap.elf $IF $PORT) &
sleep 1
ifconfig $IF up
brctl addif br0 $IF
wait



