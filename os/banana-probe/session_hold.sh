#!/bin/sh
# session_hold.sh on|off: borrow the panels from RG DS Core for a hand run, in RAM only (a reboot undoes it). Refuses
# while another session holds them; saves the launcher's environment for real_trial.sh.
case $1 in
on)
    for h in /run/rgds-hold /run/rgds/*-hold; do [ -e "$h" ] && { echo "$h: another session holds the panels" >&2; exit 1; }; done
    pidof ctremu rgds-experimental-vulkan rgds-experimental rgds-play > /dev/null && { echo "a game is running" >&2; exit 1; }
    pidof rgds-launcher > /dev/null || { echo "no launcher: someone else has the panels" >&2; exit 1; }
    tr "\0" "\n" < /proc/$(pidof rgds-launcher)/environ | grep -E "^[A-Z_0-9a-z]+=" | grep -v " " > /run/rgds/dsab/launcher.env
    cp /usr/libexec/rgds/session /run/rgds/session.dsab
    printf '#!/bin/sh\nwhile [ -e /run/rgds/dsab-hold ]; do sleep 1; done\nexec /bin/sh /run/rgds/session.dsab\n' > /run/rgds/session.dsabwait
    chmod 755 /run/rgds/session.dsab /run/rgds/session.dsabwait
    mount --bind /run/rgds/session.dsabwait /usr/libexec/rgds/session
    touch /run/rgds/dsab-hold
    killall rgds-launcher 2> /dev/null
    for i in 1 2 3 4 5 6 7 8 9 10; do pidof rgds-launcher > /dev/null || break; sleep 0.5; done
    pidof rgds-launcher > /dev/null && { echo "launcher still running" >&2; exit 1; }
    echo held ;;
off)
    rm -f /run/rgds/dsab-hold
    for i in 1 2 3 4 5 6 7 8 9 10 11 12; do pidof rgds-launcher > /dev/null && break; sleep 1; done
    umount /usr/libexec/rgds/session 2> /dev/null
    pidof rgds-launcher > /dev/null && echo released || echo "launcher not back yet" ;;
esac
