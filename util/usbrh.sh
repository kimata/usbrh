#!/bin/sh

DRIVER_PATH=/sys/bus/usb/drivers

/sbin/modprobe -v -s usbrh

echo -n "$1" > $DRIVER_PATH/hiddev/unbind 2> /dev/null
echo -n "$1" > $DRIVER_PATH/usbhid/unbind 2> /dev/null
echo -n "$1" > $DRIVER_PATH/usbrh/bind    2> /dev/null

exit 0
