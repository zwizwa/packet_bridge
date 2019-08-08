#!/bin/bash
IF=udp2tap
PORT=1234

($(dirname $0)/udpbridge.elf udp2tap $IF $PORT) &
sleep 1
ifconfig $IF up
brctl addif br0 $IF
wait



