#!/bin/sh
# Borrow the panels from Banana's menu for a probe run over SSH, in RAM only (a reboot undoes it).
#   hold.sh on    banana-ui exits and its respawn waits while /run/rgds-hold exists
#   hold.sh off   the flag goes and Banana's menu comes back
case $1 in
on)
    if ! grep -q rgds-hold /usr/bin/banana-ui-session; then
        mkdir -p /run/rgds
        cp /usr/bin/banana-ui-session /run/rgds/banana-ui-session.orig
        printf '#!/bin/sh\nwhile [ -e /run/rgds-hold ]; do sleep 1; done\nexec /bin/sh /run/rgds/banana-ui-session.orig\n' \
            > /run/rgds/banana-ui-session
        chmod 755 /run/rgds/banana-ui-session
        mount --bind /run/rgds/banana-ui-session /usr/bin/banana-ui-session
    fi
    touch /run/rgds-hold
    killall banana-ui 2>/dev/null
    for i in 1 2 3 4 5 6 7 8 9 10; do pidof banana-ui > /dev/null || break; sleep 0.5; done
    pidof banana-ui > /dev/null && { echo "banana-ui still running" >&2; exit 1; }
    echo held
    ;;
off)
    rm -f /run/rgds-hold
    echo released
    ;;
*) echo "usage: $0 on|off" >&2; exit 2 ;;
esac
