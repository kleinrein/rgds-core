#!/bin/sh
D=/run/rgds/probe; P="sh $D/nearby_probe.sh"; cd $D
trap '/usr/libexec/rgds/bt off >/dev/null 2>&1; /usr/libexec/rgds/net wifi; $P power end-restored 3' EXIT
[ -d /sys/class/bluetooth/hci0 ] || { /usr/libexec/rgds/bt on >/dev/null 2>&1; sleep 8; }
$P power bt-on-idle-2 30
timeout 5 ./btmgmt add-adv -d 020106030312ED0F1612ED524744530102030405060708 -t 60 1 < /dev/null > /dev/null 2>&1; sleep 5
$P power bt-advertising 30
timeout 5 ./btmgmt rm-adv 1 < /dev/null > /dev/null 2>&1
timeout 45 ./btmgmt find -l < /dev/null > /dev/null 2>&1 &
sleep 5
$P power bt-scanning 30
wait
/usr/libexec/rgds/bt off >/dev/null 2>&1; sleep 8
$P power bt-off-again 30
/usr/libexec/rgds/net wifi-sleep; sleep 8
$P power radios-off 30
