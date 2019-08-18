#!/bin/bash

ELF=$(dirname $0)/udpbridge.elf

br0() {
	killall udpbridge.elf
	($ELF TAP:tap0 UDP-LISTEN:1234)&
	sleep .1
	ifconfig tap0 up
	brctl addif br0 tap0
	wait
}

proxy() {
	killall udpbridge.elf
	($ELF UDP-CONNECT:172.30.3.222:1234 UDP-LISTEN:1234)&
	wait
}

tap1() {
	killall udpbridge.elf
	($ELF UDP:hatd-win10:1234 TAP:tap1)&
        sleep .1
        ifconfig tap1 up
	wait
}


# Crude but simple test
test() {
	killall udpbridge.elf

	($ELF TAP:tap0 UDP-LISTEN:1234) &
	sleep .1
	($ELF UDP-LISTEN:4321 UDP:localhost:1234) &
	sleep .1
	($ELF TAP:tap1 UDP:localhost:4321) &
	sleep .1

	ifconfig tap0 up
	brctl addif br0 tap0

	ifconfig tap1 up

	wait
}

$1



