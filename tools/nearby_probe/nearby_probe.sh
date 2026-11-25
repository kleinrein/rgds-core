#!/bin/sh
# Nearby phase 0 probes (docs/NEARBY_PLAN.md section 11). Copied to /run/rgds/probe with hostapd, hostapd_cli, btmgmt and
# link_test.py. Every Wi-Fi step restores the normal Wi-Fi (usr/libexec/rgds/net wifi) when it ends, on error too.
#   nearby_probe.sh power LABEL SECS      mean battery current (mA) over SECS
#   nearby_probe.sh ble-on | ble-off | ble-info
#   nearby_probe.sh ble-adv SECS          advertise a test service for SECS
#   nearby_probe.sh ble-scan SECS         LE discovery for SECS (devices + rssi)
#   nearby_probe.sh ap-host SECS          soft AP "RGDS-probe" ch 6 + DHCP + link_test server, then restore
#   nearby_probe.sh ap-guest              join RGDS-probe, run link_test client, restore
#   nearby_probe.sh ibss 1|2 SECS         ad-hoc "RGDS-ibss" 2437 MHz, 10.78.0.N; 1 serves, 2 runs the client
# Wi-Fi modes run detached: setsid nohup sh nearby_probe.sh ap-host 240 &
D=/run/rgds/probe
LOG=/data/rgds/logs/nearby-probe.txt
NAME=$(tr -d '\0' < /proc/device-tree/model | sed 's/Anbernic //')
log() { echo "$(date +%T) [$NAME] $*" | tee -a $LOG; }
PATH=$D:$PATH

power() {  # label secs
    s=0; n=0; st=$(cat /sys/class/power_supply/battery/status)
    for i in $(seq 1 $2); do
        c=$(cat /sys/class/power_supply/battery/current_avg); s=$((s + c)); n=$((n + 1)); sleep 1
    done
    log "power $1: mean $((s / n / 1000)) mA over ${2}s ($st)"
}

stop_station() {
    for p in wpa-event wpa udhcpc; do
        [ -f /run/rgds/$p.pid ] && kill $(cat /run/rgds/$p.pid) 2>/dev/null; rm -f /run/rgds/$p.pid
    done
    sleep 1
    ip addr flush dev wlan0 2>/dev/null
}

restore() {
    [ -f $D/hostapd.pid ] && kill $(cat $D/hostapd.pid) 2>/dev/null
    [ -f $D/udhcpd.pid ] && kill $(cat $D/udhcpd.pid) 2>/dev/null
    [ -f $D/wpa.pid ] && kill $(cat $D/wpa.pid) 2>/dev/null
    rm -f $D/*.pid
    sleep 1
    ip addr flush dev wlan0 2>/dev/null
    /usr/libexec/rgds/net wifi
    for i in $(seq 1 30); do ip -4 -o addr show wlan0 | grep -q inet && break; sleep 1; done
    log "restored: $(ip -4 -o addr show wlan0 | awk '{print $4}')"
}

case $1 in
power) power "$2" "$3" ;;
ble-on)
    if [ -d /sys/class/bluetooth/hci0 ]; then log "ble: already on"; else /usr/libexec/rgds/bt on; log "ble: on"; fi ;;
ble-off) /usr/libexec/rgds/bt off; log "ble: off" ;;
ble-info)
    btmgmt info 2>&1 | tee -a $LOG
    btmgmt advinfo 2>&1 | tee -a $LOG
    btmgmt advsize 2>&1 | tee -a $LOG ;;
ble-adv)
    btmgmt power on >/dev/null 2>&1; btmgmt le on >/dev/null 2>&1
    # flags + 16-bit UUID 0xFEED + service data "RGDS" + 8 test bytes
    timeout 5 btmgmt add-adv -d 020106030312ED0F1612ED524744530102030405060708 -t $2 1 < /dev/null 2>&1 | tee -a $LOG
    log "ble: advertising for $2 s"; sleep $2 ;;
ble-scan)
    btmgmt power on >/dev/null 2>&1; btmgmt le on >/dev/null 2>&1
    timeout $2 btmgmt find -l < /dev/null > $D/scan.txt 2>&1
    log "ble: scan found $(grep -c dev_found $D/scan.txt) adverts"
    grep -A2 dev_found $D/scan.txt | grep -B1 -A1 -i "12ed\|ed12\|5247445" | tee -a $LOG
    grep dev_found $D/scan.txt | head -20 >> $LOG ;;
ap-host)
    trap restore EXIT
    log "ap-host: start"
    stop_station
    cat > $D/hostapd.conf <<EOF
interface=wlan0
driver=nl80211
ctrl_interface=$D/hapd.ctrl
ssid=RGDS-probe
hw_mode=g
channel=6
ieee80211n=1
wmm_enabled=1
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wpa_passphrase=rgdsprobe
EOF
    ip link set wlan0 up
    t0=$(date +%s)
    hostapd -B -P $D/hostapd.pid -f $D/hostapd.log $D/hostapd.conf || { log "ap-host: hostapd failed: $(tail -5 $D/hostapd.log)"; exit 1; }
    sleep 2
    log "ap-host: hostapd up in $(( $(date +%s) - t0 )) s: $(tail -2 $D/hostapd.log | tr '\n' ' ')"
    ip addr add 10.77.0.1/24 dev wlan0
    printf 'start 10.77.0.10\nend 10.77.0.20\ninterface wlan0\nlease_file %s/leases\npidfile %s/udhcpd.pid\noption subnet 255.255.255.0\n' $D $D > $D/udhcpd.conf
    touch $D/leases; udhcpd $D/udhcpd.conf
    BCAST_ADDR=10.77.0.255 python3 $D/link_test.py server $2 &
    power ap-idle 20
    end=$(( $(date +%s) + $2 - 20 ))
    while [ $(date +%s) -lt $end ]; do
        sleep 15; log "ap-host: stations: $(hostapd_cli -p $D/hapd.ctrl all_sta 2>/dev/null | grep -c '^[0-9a-f][0-9a-f]:')"
    done
    log "ap-host: hostapd log tail: $(grep -iE 'error|fail|deauth|disassoc' $D/hostapd.log | tail -5 | tr '\n' ' ')"
    ;;
ap-guest)
    trap restore EXIT
    log "ap-guest: start"
    stop_station
    printf 'ctrl_interface=%s/wpa.ctrl\nnetwork={\n ssid="RGDS-probe"\n psk="rgdsprobe"\n scan_freq=2437\n}\n' $D > $D/wpa.conf
    ip link set wlan0 up
    t0=$(date +%s)
    wpa_supplicant -B -i wlan0 -c $D/wpa.conf -P $D/wpa.pid -f $D/wpa.log
    for i in $(seq 1 60); do wpa_cli -p $D/wpa.ctrl status 2>/dev/null | grep -q wpa_state=COMPLETED && break; sleep 1; done
    log "ap-guest: associated after $(( $(date +%s) - t0 )) s: $(wpa_cli -p $D/wpa.ctrl status | grep -E 'wpa_state|freq' | tr '\n' ' ')"
    udhcpc -i wlan0 -n -q -t 10 -s /usr/share/udhcpc/default.script >/dev/null 2>&1 || udhcpc -i wlan0 -n -q -t 10 >/dev/null 2>&1
    log "ap-guest: address after $(( $(date +%s) - t0 )) s: $(ip -4 -o addr show wlan0 | awk '{print $4}')"
    ip -4 -o addr show wlan0 | grep -q 10.77.0 || ip addr add 10.77.0.9/24 dev wlan0
    log "ap-guest: link: $(python3 $D/link_test.py client 10.77.0.1)"
    power ap-guest-idle 20
    ;;
ibss)
    trap restore EXIT
    log "ibss $2: start"
    stop_station
    printf 'ctrl_interface=%s/wpa.ctrl\nap_scan=2\nnetwork={\n ssid="RGDS-ibss"\n mode=1\n frequency=2437\n key_mgmt=NONE\n}\n' $D > $D/wpa.conf
    ip link set wlan0 up
    wpa_supplicant -B -i wlan0 -c $D/wpa.conf -P $D/wpa.pid -f $D/wpa.log || { log "ibss: wpa_supplicant failed"; exit 1; }
    for i in $(seq 1 20); do wpa_cli -p $D/wpa.ctrl status 2>/dev/null | grep -q wpa_state=COMPLETED && break; sleep 1; done
    log "ibss $2: $(wpa_cli -p $D/wpa.ctrl status | grep -E 'wpa_state|mode|bssid' | tr '\n' ' ')"
    ip addr add 10.78.0.$2/24 dev wlan0
    if [ "$2" = 1 ]; then
        BCAST_ADDR=10.78.0.255 python3 $D/link_test.py server $3
    else
        sleep 10
        log "ibss: link: $(python3 $D/link_test.py client 10.78.0.1)"
        power ibss-idle 20
    fi
    log "ibss $2: wpa log: $(grep -iE 'fail|error|disconnect' $D/wpa.log | tail -3 | tr '\n' ' ')"
    ;;
esac
