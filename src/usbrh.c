/*
 * USRBRH driver
 *
 * Copyright (C) 2008 Tetsuya Kimata <kimata@acapulco.dyndns.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 *
 * $Id: usbrh.c 39 2009-09-04 16:11:42Z kimata $
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/usb.h>
#include <linux/device.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("USBRH driver");
MODULE_VERSION("0.0.8");
MODULE_AUTHOR("Tetsuya Kimata, kimata@acapulco.dyndns.org");

#define USBRH_NAME              "usbrh"

#define USBRH_VENDOR_ID         0x1774
#define USBRH_PRODUCT_ID        0x1001

#ifdef CONFIG_USB_DYNAMIC_MINORS
#define USBRH_MINOR_BASE        0
#else
#define USBRH_MINOR_BASE        123
#endif

#define USBRH_SENSOR_ENDPOINT   0x81

#define USBRH_BUFFER_SIZE       7
#define USBRH_SENSOR_RETRY      2

#define USBRH_FIXED_INT_UNIT_SQ 400
#define USBRH_FIXED_INT_UNIT    (USBRH_FIXED_INT_UNIT_SQ*USBRH_FIXED_INT_UNIT_SQ)
#define USBRH_FIXED_VAL(a)      ((int)((a) * USBRH_FIXED_INT_UNIT))
#define USBRH_FIXED_MUL(a, b)   (((a)/USBRH_FIXED_INT_UNIT_SQ) * \
                                 ((b)/USBRH_FIXED_INT_UNIT_SQ))

/*
 * SHT1x / SHT7x
 * http://www.sensirion.com/images/getFile?id=25
 */
#define USBRH_C1                USBRH_FIXED_VAL(-4)          /* 12bit */
#define USBRH_C2                USBRH_FIXED_VAL(0.0405)      /* 12bit */
#define USBRH_C3                USBRH_FIXED_VAL(-0.0000028)  /* 12bit */
#define USBRH_T1                USBRH_FIXED_VAL(0.01)        /* 12bit */
#define USBRH_T2                USBRH_FIXED_VAL(0.00008)     /* 12bit */
#define USBRH_D1                USBRH_FIXED_VAL(-40.0)       /* 5V. 14bit */
#define USBRH_D2                USBRH_FIXED_VAL(0.01)        /* 5V. 14bit */

#define USBRH_HUMI_LINEAR_CALC(so_rh)                                   \
    (USBRH_C1 + USBRH_FIXED_MUL(USBRH_C2, so_rh) +                      \
     USBRH_FIXED_MUL(USBRH_FIXED_MUL(USBRH_C3, so_rh), so_rh))
#define USBRH_HUMI_CALC(so_rh, so_t)                                    \
    ((USBRH_FIXED_MUL(USBRH_TEMP_CALC(so_t) - USBRH_FIXED_VAL(25),      \
                      USBRH_T1 + USBRH_FIXED_MUL(USBRH_T2, so_rh))) +   \
     USBRH_HUMI_LINEAR_CALC(so_rh))
#define USBRH_TEMP_CALC(so_t)   (USBRH_D1 + USBRH_FIXED_MUL(USBRH_D2, so_t))
#define USBRH_HUMI(so_rh, so_t) USBRH_HUMI_CALC(USBRH_FIXED_VAL(so_rh), \
                                                USBRH_FIXED_VAL(so_t))
#define USBRH_TEMP(so_t)        USBRH_TEMP_CALC(USBRH_FIXED_VAL(so_t))

#ifdef DEBUG
#define DEBUG_INFO(...)         dev_info(__VA_ARGS__)
#else
#define DEBUG_INFO(...)
#endif

static struct usb_device_id usbrh_table [] = {
    {USB_DEVICE(USBRH_VENDOR_ID, USBRH_PRODUCT_ID)},
    {},
};

MODULE_DEVICE_TABLE(usb, usbrh_table);

struct usbrh {
    struct usb_device *udev;
    struct usb_interface *interface;
    struct proc_dir_entry *proc_dir;

    unsigned int index;
    struct mutex io_mutex;
    struct kref kref;

    unsigned char led;
    unsigned char heater;
};

struct usbrh_fh {
    struct usbrh *dev;
    int done;
};

struct usbrh_sensor_value {
    unsigned char humiMSB;
    unsigned char humiLSB;
    unsigned char tempMSB;
    unsigned char tempLSB;
    unsigned char reserved[3];
};

struct usbrh_proc_entry {
    char *name;
    read_proc_t *read_proc;
    write_proc_t *write_proc;
};

static struct usb_driver usbrh_driver;
static struct proc_dir_entry *usbrh_proc_base;

static int usbrh_control_msg(struct usbrh *dev, int value, char *buffer)
{
    return usb_control_msg
        (dev->udev,
         usb_sndctrlpipe(dev->udev, 0),
         USB_REQ_SET_CONFIGURATION, // bRequest (0x09)
         USB_DIR_OUT|USB_TYPE_CLASS|USB_RECIP_INTERFACE, // bmRequestType (0x21)
         value, // wValue
         0, // wIndex
         buffer, USBRH_BUFFER_SIZE, msecs_to_jiffies(100));
}

static int usbrh_read_sensor_onece(struct usbrh *dev,
                                   struct usbrh_sensor_value *value)
{
    int read_size;
    int result;
    char buffer[USBRH_BUFFER_SIZE];

    memset(buffer, 0, sizeof(buffer));

    result = usbrh_control_msg(dev, 0x0200, buffer);

    DEBUG_INFO(&dev->udev->dev, "usb_control_msg: %d", result);
    if (result < 0) {
        return -1;
    }

    result = usb_bulk_msg(dev->udev,
                          usb_rcvbulkpipe(dev->udev, USBRH_SENSOR_ENDPOINT),
                          value, sizeof(*value),
                          &read_size, msecs_to_jiffies(1000));

    DEBUG_INFO(&dev->udev->dev, "usb_bulk_msg: %d", result);

    return ((result == 0) && (read_size == sizeof(*value))) ? 0 : -1;
}

static int usbrh_read_sensor(struct usbrh *dev,
                             struct usbrh_sensor_value *value)
{
    int result;
    int i;

    for (i = 0; i < USBRH_SENSOR_RETRY; i++) {
        result = usbrh_read_sensor_onece(dev, value);
        if (result == 0) {
            break;
        }
    }

    return result;
}

static int usbrh_control_led(struct usbrh *dev, unsigned char led_index,
                             unsigned char is_on)
{
    int result;
    char buffer[USBRH_BUFFER_SIZE];

    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 3 + led_index;
    buffer[1] = is_on;

    result = usbrh_control_msg(dev, 0x0300, buffer);

    DEBUG_INFO(&dev->udev->dev, "usb_control_msg: %d", result);
    if (result < 0) {
        return -1;
    }

    return 0;
}

static int usbrh_control_heater(struct usbrh *dev, unsigned char is_on)
{
    int result;
    char buffer[USBRH_BUFFER_SIZE];

    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 1;
    buffer[1] = is_on << 2;

    result = usbrh_control_msg(dev, 0x0300, buffer);

    DEBUG_INFO(&dev->udev->dev, "usb_control_msg: %d", result);
    if (result < 0) {
        return -1;
    }

    return 0;
}


static size_t usbrh_value_so_rh(struct usbrh_sensor_value *value)
{
    return ((int)(value->humiMSB) << 8 | value->humiLSB);
}

static int usbrh_value_so_t(struct usbrh_sensor_value *value)
{
    return ((int)(value->tempMSB) << 8 | value->tempLSB);
}

static int usbrh_calc_humi(struct usbrh_sensor_value *value)
{
    return USBRH_HUMI(usbrh_value_so_rh(value), usbrh_value_so_t(value));
}

static int usbrh_calc_temp(struct usbrh_sensor_value *value)
{
    return USBRH_TEMP(usbrh_value_so_t(value));
}

static int usbrh_sprint_value(char *buffer, int value)
{
    int len;

    len = 0;
    if (value < 0) {
        len += sprintf(buffer+len, "-");
        value = -value;
    }
    len += sprintf(buffer+len, "%d.",
                   value/USBRH_FIXED_INT_UNIT);
    len += sprintf(buffer+len, "%02d",
                   ((int)(value*100/USBRH_FIXED_INT_UNIT) % 100));

    return len;
}

static int usbrh_proc_stat_read(char *buffer, char **start, off_t offset,
                                int count, int *peof, void *dat)
{
    struct usbrh *dev;
    struct usbrh_sensor_value value;
    int len;

    dev = (struct usbrh *)dat;

    *peof = 1;

    if (usbrh_read_sensor(dev, &value)) {
        return sprintf(buffer, "Failed to get temperature/humierature\n");
    }

    len = sprintf(buffer, "t:");
    len += usbrh_sprint_value(buffer+len, usbrh_calc_temp(&value));
    len += sprintf(buffer+len, " h:");
    len += usbrh_sprint_value(buffer+len, usbrh_calc_humi(&value));
    len += sprintf(buffer+len, "\n");

    return len;
}

static int usbrh_proc_temp_read(char *buffer, char **start, off_t offset,
                                int count, int *peof, void *dat)
{
    struct usbrh *dev;
    struct usbrh_sensor_value value;
    int len;

    dev = (struct usbrh *)dat;

    *peof = 1;

    if (usbrh_read_sensor(dev, &value)) {
        return sprintf(buffer, "Failed to get temperature\n");
    }

    len = usbrh_sprint_value(buffer, usbrh_calc_temp(&value));
    len += sprintf(buffer+len, "\n");

    return len;
}

static int usbrh_proc_humi_read(char *buffer, char **start, off_t offset,
                                int count, int *peof, void *dat)
{
    struct usbrh *dev;
    struct usbrh_sensor_value value;
    int len;

    dev = (struct usbrh *)dat;

    *peof = 1;

    if (usbrh_read_sensor(dev, &value)) {
        return sprintf(buffer, "Failed to get humidity\n");
    }

    len = usbrh_sprint_value(buffer, usbrh_calc_humi(&value));
    len += sprintf(buffer+len, "\n");

    return len;
}

static int usbrh_proc_led_read(char *buffer, char **start, off_t offset,
                               int count, int *peof, void *dat)
{
    struct usbrh *dev;

    dev = (struct usbrh *)dat;
    *peof = 1;

    return sprintf(buffer, "%d\n", dev->led);
}

static int usbrh_proc_led_write(struct file *file, __user const char *buffer,
                                unsigned long count, void *dat)
{
    struct usbrh *dev;

    dev = (struct usbrh *)dat;
    dev->led = (buffer[0] - '0') & 0x3;

    usbrh_control_led(dev, 0, (dev->led >> 0) & 0x1);
    usbrh_control_led(dev, 1, (dev->led >> 1) & 0x1);

    return count;
}

static int usbrh_proc_heater_read(char *buffer, char **start, off_t offset,
                                  int count, int *peof, void *dat)
{
    struct usbrh *dev;

    dev = (struct usbrh *)dat;
    *peof = 1;

    return sprintf(buffer, "%d\n", dev->heater);
}

static int usbrh_proc_heater_write(struct file *file, __user const char *buffer,
                                   unsigned long count, void *dat)
{
    struct usbrh *dev;

    dev = (struct usbrh *)dat;
    dev->heater = (buffer[0] - '0') & 0x1;

    usbrh_control_heater(dev, dev->heater);

    return count;
}


static const char* USBRH_DIGIT_STR[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};
static const struct usbrh_proc_entry USBRH_ENTRY_LIST[] = {
    { "status",         usbrh_proc_stat_read, NULL },
    { "temperature",    usbrh_proc_temp_read, NULL },
    { "humidity",       usbrh_proc_humi_read, NULL },
    { "led",            usbrh_proc_led_read, usbrh_proc_led_write },
    { "heater",         usbrh_proc_heater_read, usbrh_proc_heater_write },
    {},
};

static void usbrh_create_proc(struct usbrh *dev)
{
    struct proc_dir_entry *proc_dir;
    struct proc_dir_entry *proc_file;
    unsigned int index;
    int i;

    index = dev->index;

    if (index >= ARRAY_SIZE(USBRH_DIGIT_STR)) {
        pr_err("too many USBRH: %d", index);
    }

    proc_dir = proc_mkdir(USBRH_DIGIT_STR[index], usbrh_proc_base);
    if (proc_dir == NULL) {
        pr_err("Faile to create /proc/" USBRH_NAME "/%d", index);
        return;
    }

    for (i = 0; USBRH_ENTRY_LIST[i].name != NULL; i++) {
        if (USBRH_ENTRY_LIST[i].write_proc == NULL) {
            proc_file = create_proc_read_entry(USBRH_ENTRY_LIST[i].name,
                                               S_IFREG|S_IRUGO,
                                               proc_dir,
                                               USBRH_ENTRY_LIST[i].read_proc,
                                               dev);
        } else {
            proc_file = create_proc_entry(USBRH_ENTRY_LIST[i].name,
                                          S_IFREG|S_IRUGO|S_IWUSR, proc_dir);

            if (proc_file != NULL) {
                proc_file->read_proc = USBRH_ENTRY_LIST[i].read_proc;
                proc_file->write_proc = USBRH_ENTRY_LIST[i].write_proc;
                proc_file->data = dev;
            }
        }

        if (proc_file == NULL) {
            pr_err("Faile to create /proc/" USBRH_NAME "/%d/%s",
                index, USBRH_ENTRY_LIST[i].name);
        }
    }

    dev->proc_dir = proc_dir;
}

static void usbrh_remove_proc(struct usbrh *dev)
{
    int i;

    for (i = 0; USBRH_ENTRY_LIST[i].name != NULL; i++) {
        remove_proc_entry(USBRH_ENTRY_LIST[i].name, dev->proc_dir);
    }
    remove_proc_entry(USBRH_DIGIT_STR[dev->index], usbrh_proc_base);
}

static void usbrh_delete(struct kref *kref)
{
    struct usbrh *dev;

    dev = container_of(kref, struct usbrh, kref);

    usbrh_remove_proc(dev);
    usb_put_dev(dev->udev);
    kfree(dev);
}

static int usbrh_release(struct inode *inode, struct file *file)
{
    struct usbrh_fh *dev_fh;

    dev_fh = (struct usbrh_fh *)file->private_data;
    if (dev_fh == NULL) {
        return -ENODEV;
    }

    kref_put(&dev_fh->dev->kref, usbrh_delete);
    kfree(dev_fh);

    return 0;
}

static int usbrh_open(struct inode *inode, struct file *file)
{
    struct usbrh *dev;
    struct usbrh_fh *dev_fh;
    struct usb_interface *interface;
    int subminor;

    subminor = iminor(inode);

    interface = usb_find_interface(&usbrh_driver, subminor);
    if (interface == NULL) {
        pr_err("Can't find device for minor %d", subminor);
        return -ENODEV;
    }

    dev = usb_get_intfdata(interface);
    if (dev == NULL) {
        return -ENODEV;
    }

    dev_fh = kmalloc(sizeof(struct usbrh_fh), GFP_KERNEL);
    if (dev_fh == NULL) {
        pr_err("Out of memory");
        return -ENOMEM;
    }

    dev_fh->dev = dev;
    dev_fh->done = 0;

    kref_get(&dev->kref);
    file->private_data = dev_fh;

    return 0;
}

static ssize_t usbrh_read(struct file *file, char __user *buffer, size_t count,
                          loff_t *ppos)
{
    struct usbrh_fh *dev_fh;
    struct usbrh *dev;
    struct usbrh_sensor_value value;
    int result;

    dev_fh = (struct usbrh_fh *)file->private_data;
    dev = dev_fh->dev;

    mutex_lock(&dev->io_mutex);
    if (dev->interface == NULL) {
        result = -ENODEV;
        goto exit;
    }

    if (dev_fh->done) {
        result = 0;
        goto exit;
    }

    if (usbrh_read_sensor(dev, &value)) {
        result = -EIO;
        goto exit;
    }
    dev_fh->done = 1;

    if (count > sizeof(value)) {
        count = sizeof(value);
    }

    if (copy_to_user(buffer, &value, count)) {
        result = -EFAULT;
        goto exit;
    }
    result = count;

 exit:
    mutex_unlock(&dev->io_mutex);

    return result;
}

static struct file_operations usbrh_fops = {
    .owner =    THIS_MODULE,
    .read =     usbrh_read,
    .write =    NULL,
    .open =     usbrh_open,
    .release =  usbrh_release,
};

static struct usb_class_driver usbrh_class = {
    .name       = "usb/" USBRH_NAME "%d",
    .fops       = &usbrh_fops,
    .minor_base = USBRH_MINOR_BASE,
};

static int usbrh_probe(struct usb_interface *interface,
                       const struct usb_device_id *id)
{
    struct usbrh *dev = NULL;
    int result = -ENOMEM;

    dev = kmalloc(sizeof(struct usbrh), GFP_KERNEL);
    if (dev == NULL) {
        pr_err("Out of memory");
        goto error;
    }

    memset(dev, 0, sizeof(*dev));
    mutex_init(&dev->io_mutex);
    kref_init(&dev->kref);

    usb_set_intfdata(interface, dev);

    result = usb_register_dev(interface, &usbrh_class);
    if (result) {
        pr_err("Unable to get a minor for this device.");
        usb_set_intfdata(interface, NULL);
        goto error;
    }

    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    dev->interface = interface;
    dev->index = interface->minor - USBRH_MINOR_BASE;

    usbrh_create_proc(dev);

    dev_info(&dev->udev->dev, "USBRH device now attached to /dev/usbrh%d", dev->index);
    return 0;

 error:
    if (dev != NULL) {
        kref_put(&dev->kref, usbrh_delete);
    }
    return result;
}

static void usbrh_disconnect(struct usb_interface *interface)
{
    struct usbrh *dev;

    dev = usb_get_intfdata(interface);

    dev_info(&dev->udev->dev, "USBRH disconnected");

    usb_set_intfdata(interface, NULL);
    usb_deregister_dev(interface, &usbrh_class);

    mutex_lock(&dev->io_mutex);
    dev->interface = NULL;
    mutex_unlock(&dev->io_mutex);

    kref_put(&dev->kref, usbrh_delete);
}

static struct usb_driver usbrh_driver = {
    .name       = USBRH_NAME,
    .id_table   = usbrh_table,
    .probe      = usbrh_probe,
    .disconnect = usbrh_disconnect,
};

static int __init usbrh_init(void)
{
    int result;

    usbrh_proc_base = proc_mkdir(USBRH_NAME, NULL);
    if (usbrh_proc_base == NULL) {
        pr_err("Failed to create_proc_entry");
        return -EIO;
    }

    result = usb_register(&usbrh_driver);
    if (result) {
        remove_proc_entry(USBRH_NAME, NULL);
        pr_err("Failed to usb_register: %d", result);
        return -EIO;
    }

    return 0;
}

static void __exit usbrh_exit(void)
{
    if (usbrh_proc_base != NULL) {
        usbrh_proc_base->subdir = NULL;
        remove_proc_entry(USBRH_NAME, NULL);
    }
    usb_deregister(&usbrh_driver);
}

module_init(usbrh_init);
module_exit(usbrh_exit);