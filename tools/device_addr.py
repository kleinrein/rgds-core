#!/usr/bin/env python3
"""Print the handheld's SSH address: 10.55.0.1 on the USB cable (usr/libexec/rgds/usb) if it answers, else rgds.local."""
import socket

USB = '10.55.0.1'
WIFI = 'rgds.local'


def device_addr(timeout=.25):
    try:
        with socket.create_connection((USB, 22), timeout=timeout):
            return USB
    except OSError:
        return WIFI


if __name__ == '__main__':
    print(device_addr())
