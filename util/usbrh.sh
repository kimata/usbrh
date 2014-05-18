#!/bin/sh

DRIVER_PATH=/sys/bus/usb/drivers

/sbin/modprobe -v -s usbrh

dev_id=`echo -n $1 | awk -F '/' '{print $NF}'`

echo -n "$dev_id:1.0" > $DRIVER_PATH/usbhid/unbind      2> /dev/null
echo -n "$dev_id:1.0" > $DRIVER_PATH/usbrh/bind         2> /dev/null

exit 0
