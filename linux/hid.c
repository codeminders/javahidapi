/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.
 
 Alan Ott
 Signal 11 Software
 
 8/22/2009
 Linux Version - 6/2/2009
 
 Copyright 2009, All Rights Reserved.
 
 At the discretion of the user of this library,
 this software may be licensed under the terms of the
 GNU Public License v3, a BSD-Style license, or the
 original HIDAPI license as outlined in the LICENSE.txt,
 LICENSE-gpl3.txt, LICENSE-bsd.txt, and LICENSE-orig.txt
 files located at the root of the source distribution.
 These files may also be found in the public source
 code repository located at:
 http://github.com/signal11/hidapi .
 ********************************************************/

/* C */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <errno.h>
    
/* Unix */
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <poll.h>

/* Linux */
#include <linux/hidraw.h>
#include <linux/version.h>
#include <libudev.h>
#include <pthread.h>

#include "hidapi.h"

#define  HID_DEVICE_SUPPORT_CONNECT 1
#define  TRUE                       1
#define  FALSE                      0
#define  KERN_SUCCESS               0
#define  OK                         0
#define  FAILURE                   -1    
#define  MAX_PATH                   256
#define  SLEEP_TIME                 250 * 1000
//#define  DEBUG

#if HID_DEVICE_SUPPORT_CONNECT

typedef struct _udev_state udev_state;

#define DRV_STATE_UDEV_MONITOR(ds) ((ds)->udev_monitor)
#define DEV_ACTION_ADD "add"
#define DEV_ACTION_REMOVE "remove"


typedef  struct _udev_notify udev_notify;

struct _udev_notify {
    char path[MAX_PATH];
    struct hid_device_info *info;
    struct udev_device *dev;
    udev_notify *next;
};

/* Register callbacks implementation*/

struct _udev_state {
    udev_notify *udev_notify;
    struct udev_monitor *udev_monitor;
    int shutdown_thread;
    int start_add;
    pthread_mutex_t notify_lock;
    pthread_mutex_t lock;
    pthread_t thread;
};

typedef struct hid_device_callback_connect_t hid_device_callback_connect;
typedef void *hid_device_context;

struct hid_device_callback_connect_t
{
    hid_device_callback callback;
    hid_device_context context; 
    hid_device_callback_connect *next;
};
/* Static list of all the callbacks registred.*/

static hid_device_callback_connect *connect_callback_list = NULL;
static struct hid_device_info *connect_device_info = NULL;
static udev_state *dev_state  = NULL;

/* Static list of all the devices open. This way when a device gets
 disconnected, its hid_device structure can be marked as disconnected
 from hid_device_removal_callback(). */
static hid_device *device_list = NULL;
static pthread_mutex_t device_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t connect_callback_mutex = PTHREAD_MUTEX_INITIALIZER;


/* Declare of inner  functions */
int hid_init_connect();
void hid_deinit_connect(void);

#endif //HID_DEVICE_SUPPORT_CONNECT

/* Definitions from linux/hidraw.h. Since these are new, some distros
 may not have header files which contain them. */
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x06, len)
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x07, len)
#endif

struct hid_device_ {
    int device_handle;
    int blocking;
    int uses_numbered_reports;
    int ref_count; /* Reference count of the opened devices */
    hid_device *next; 
};


static __u32 kernel_version = 0;

hid_device *new_hid_device()
{
    hid_device *dev = calloc(1, sizeof(hid_device));
    dev->device_handle = -1;
    dev->blocking = 1;
    dev->uses_numbered_reports = 0;
    dev->ref_count = 1;
    dev->next = NULL;
    
    /* Add the new record to the device_list. */
    
    pthread_mutex_lock(&device_list_mutex);
    
    if (!device_list)
        device_list = dev;
    else {
        hid_device *d = device_list;
        while (d) {
            if (!d->next) {
                d->next = dev;
                break;
            }
            d = d->next;
        }
    }
    
    pthread_mutex_unlock(&device_list_mutex);
    
    return dev;
}

static void free_hid_device(hid_device *dev)
{
    if (!dev)
        return;
    
    /* Remove it from the device list. */
    
    pthread_mutex_lock(&device_list_mutex);
    
    hid_device *d = device_list;
    if (d == dev) {
        device_list = d->next;
    }
    else {
        while (d) {
            if (d->next == dev) {
                d->next = d->next->next;
                break;
            }
            d = d->next;
        }
    }
    
    pthread_mutex_unlock(&device_list_mutex);
    
    /* Free the structure itself. */
    
    close(dev->device_handle);
    free(dev);
}

//#pragma mark get opened device

static hid_device* get_hid_device_path(const char *path)
{
#define BUF_LEN  256
    hid_device *dev = NULL;    
    char buf[BUF_LEN];
    int res = 0;
    
    pthread_mutex_lock(&device_list_mutex);
    
    if (!device_list){
        pthread_mutex_unlock(&device_list_mutex);
        return NULL;
    }
    else {
        hid_device *d = device_list;
        while (d) {
            if(d->device_handle > 0){    
                /* Get Raw Name */
                res = ioctl(d->device_handle, HIDIOCGRAWNAME(256), buf);
                   if (res < 0){
                    //printf("HIDIOCGRAWNAME");
                  }
                else {
                    //printf("Raw Name: %s\n", buf);
                }
                if (!strcmp(buf, path)) {
                    d->ref_count++;
                    dev = d; 
                    break;
                }
            }
            d = d->next;
        }
    }
    pthread_mutex_unlock(&device_list_mutex);
    
    return dev;
}

#if 0
static void register_error(hid_device *device, const char *op)
{
}
#endif

/* Get an attribute value from a udev_device and return it as a whar_t
 string. The returned string must be freed with free() when done.*/
static wchar_t *copy_udev_string(struct udev_device *dev, const char *udev_name)
{
    const char *str;
    wchar_t *ret = NULL;
    str = udev_device_get_sysattr_value(dev, udev_name);
    if (str) {
        /* Convert the string from UTF-8 to wchar_t */
        size_t wlen = mbstowcs(NULL, str, 0);
        ret = calloc(wlen+1, sizeof(wchar_t));
        mbstowcs(ret, str, wlen+1);
        ret[wlen] = 0x0000;
    }
    
    return ret;
}

/* uses_numbered_reports() returns 1 if report_descriptor describes a device
 which contains numbered reports. */ 
static int uses_numbered_reports(__u8 *report_descriptor, __u32 size) {
    int i = 0;
    int size_code;
    int data_len, key_size;
    
    while (i < size) {
        int key = report_descriptor[i];
        
        /* Check for the Report ID key */
        if (key == 0x85/*Report ID*/) {
            /* This device has a Report ID, which means it uses
             numbered reports. */
            return 1;
        }
        
        //printf("key: %02hhx\n", key);
        
        if ((key & 0xf0) == 0xf0) {
            /* This is a Long Item. The next byte contains the
             length of the data section (value) for this key.
             See the HID specification, version 1.11, section
             6.2.2.3, titled "Long Items." */
            if (i+1 < size)
                data_len = report_descriptor[i+1];
            else
                data_len = 0; /* malformed report */
            key_size = 3;
        }
        else {
            /* This is a Short Item. The bottom two bits of the
             key contain the size code for the data section
             (value) for this key.  Refer to the HID
             specification, version 1.11, section 6.2.2.2,
             titled "Short Items." */
            size_code = key & 0x3;
            switch (size_code) {
                case 0:
                case 1:
                case 2:
                    data_len = size_code;
                    break;
                case 3:
                    data_len = 4;
                    break;
                default:
                    /* Can't ever happen since size_code is & 0x3 */
                    data_len = 0;
                    break;
            };
            key_size = 1;
        }
        
        /* Skip over this key and it's associated data */
        i += data_len + key_size;
    }
    
    /* Didn't find a Report ID key. Device doesn't use numbered reports. */
    return 0;
}

static int get_device_string(hid_device *dev, const char *key, wchar_t *string, size_t maxlen)
{
    struct udev *udev;
    struct udev_device *udev_dev, *parent;
    struct stat s;
    int ret = -1;
    
    /* Create the udev object */
    udev = udev_new();
    if (!udev) {
        //printf("Can't create udev\n");
        return -1;
    }
    
    /* Get the dev_t (major/minor numbers) from the file handle. */
    fstat(dev->device_handle, &s);
    /* Open a udev device from the dev_t. 'c' means character device. */
    udev_dev = udev_device_new_from_devnum(udev, 'c', s.st_rdev);
    if (udev_dev) {
        const char *str;
        /* Find the parent USB Device */
        parent = udev_device_get_parent_with_subsystem_devtype(
                                                               udev_dev,
                                                               "usb",
                                                               "usb_device");
        if (parent) {
            str = udev_device_get_sysattr_value(parent, key);
            if (str) {
                /* Convert the string from UTF-8 to wchar_t */
                //ret = mbstowcs(string, str, maxlen);
                ret = mbstowcs(string, str, maxlen) < 0?-1:0;
                goto end;
            }
        }
    }
    
end:
    udev_device_unref(udev_dev);
    // parent doesn't need to be (and can't be) unref'd.
    // I'm not sure why, but it'll throw double-free() errors.
    udev_unref(udev);
    
    return ret;
}

int HID_API_EXPORT hid_init(void)
{
   const char *locale;
   /* Set the locale if it's not set. */
   locale = setlocale(LC_CTYPE, NULL);
   if (!locale)
       setlocale(LC_CTYPE, "");
    hid_init_connect();
    return 0;
}

int HID_API_EXPORT hid_exit(void)
{
    /* Nothing to do for this in the Linux/hidraw implementation. */
    hid_remove_all_notification_callbacks();    
    hid_deinit_connect();
    return 0;
}

struct hid_device_info  HID_API_EXPORT *hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
    struct udev *udev;
    struct udev_enumerate *enumerate;
    struct udev_list_entry *devices, *dev_list_entry;
    
    struct hid_device_info *root = NULL; // return object
    struct hid_device_info *cur_dev = NULL;
    
    hid_init();
    
    /* Create the udev object */
    udev = udev_new();
    if (!udev) {
        //printf("Can't create udev\n");
        return NULL;
    }
    
    /* Create a list of the devices in the 'hidraw' subsystem. */
    enumerate = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerate, "hidraw");
    udev_enumerate_scan_devices(enumerate);
    devices = udev_enumerate_get_list_entry(enumerate);
    /* For each item, see if it matches the vid/pid, and if so
     create a udev_device record for it */
    udev_list_entry_foreach(dev_list_entry, devices) {
        const char *sysfs_path;
        const char *dev_path;
        const char *str;
        struct udev_device *hid_dev; // The device's HID udev node.
        struct udev_device *dev; // The actual hardware device.
        struct udev_device *intf_dev; // The device's interface (in the USB sense).
        unsigned short dev_vid;
        unsigned short dev_pid;
        
        /* Get the filename of the /sys entry for the device
         and create a udev_device object (dev) representing it */
        sysfs_path = udev_list_entry_get_name(dev_list_entry);
        hid_dev = udev_device_new_from_syspath(udev, sysfs_path);
        dev_path = udev_device_get_devnode(hid_dev);
        
        /* The device pointed to by hid_dev contains information about
         the hidraw device. In order to get information about the
         USB device, get the parent device with the
         subsystem/devtype pair of "usb"/"usb_device". This will
         be several levels up the tree, but the function will find
         it.*/
        dev = udev_device_get_parent_with_subsystem_devtype(
                                                            hid_dev,
                                                            "usb",
                                                            "usb_device");
        if (!dev) {
            /* Unable to find parent usb device. */
            goto next;
        }
        
        /* Get the VID/PID of the device */
        str = udev_device_get_sysattr_value(dev,"idVendor");
        dev_vid = (str)? strtol(str, NULL, 16): 0x0;
        str = udev_device_get_sysattr_value(dev, "idProduct");
        dev_pid = (str)? strtol(str, NULL, 16): 0x0;
        
        /* Check the VID/PID against the arguments */
        if ((vendor_id == 0x0 && product_id == 0x0) ||
            (vendor_id == dev_vid && product_id == dev_pid)) {
            struct hid_device_info *tmp;
            size_t len;
            
            /* VID/PID match. Create the record. */
            tmp = malloc(sizeof(struct hid_device_info));
            if (cur_dev) {
                cur_dev->next = tmp;
            }
            else {
                root = tmp;
            }
            cur_dev = tmp;
            
            /* Fill out the record */
            cur_dev->next = NULL;
            str = dev_path;
            if (str) {
                len = strlen(str);
                cur_dev->path = calloc(len+1, sizeof(char));
                strncpy(cur_dev->path, str, len+1);
                cur_dev->path[len] = '\0';
            }
            else
                cur_dev->path = NULL;
            
            /* Serial Number */
            cur_dev->serial_number
            = copy_udev_string(dev, "serial");
            
            /* Manufacturer and Product strings */
            cur_dev->manufacturer_string
            = copy_udev_string(dev, "manufacturer");
            cur_dev->product_string
            = copy_udev_string(dev, "product");
            
            /* VID/PID */
            cur_dev->vendor_id = dev_vid;
            cur_dev->product_id = dev_pid;
            
            /* Release Number */
            str = udev_device_get_sysattr_value(dev, "bcdDevice");
            cur_dev->release_number = (str)? strtol(str, NULL, 16): 0x0;
            
            /* Interface Number */
            cur_dev->interface_number = -1;
            /* Get a handle to the interface's udev node. */
            intf_dev = udev_device_get_parent_with_subsystem_devtype(
                                                                     hid_dev,
                                                                     "usb",
                                                                     "usb_interface");
            if (intf_dev) {
                str = udev_device_get_sysattr_value(intf_dev, "bInterfaceNumber");
                cur_dev->interface_number = (str)? strtol(str, NULL, 16): -1;
            }
        }
        else
            goto next;
    next:
        udev_device_unref(hid_dev);
        /* dev and intf_dev don't need to be (and can't be)
         unref()d.  It will cause a double-free() error.  I'm not
         sure why.  */
    }
    /* Free the enumerator and udev objects. */
    udev_enumerate_unref(enumerate);
    udev_unref(udev);
    
    return root;
}

void  HID_API_EXPORT hid_free_enumeration(struct hid_device_info *devs)
{
    struct hid_device_info *d = devs;
    while (d) {
        struct hid_device_info *next = d->next;
        free(d->path);
        free(d->serial_number);
        free(d->manufacturer_string);
        free(d->product_string);
        free(d);
        d = next;
    }
}

hid_device * hid_open(unsigned short vendor_id, unsigned short product_id, wchar_t *serial_number)
{
    struct hid_device_info *devs, *cur_dev;
    const char *path_to_open = NULL;
    hid_device *handle = NULL;
    
    devs = hid_enumerate(vendor_id, product_id);
    cur_dev = devs;
    while (cur_dev) {
        if (cur_dev->vendor_id == vendor_id &&
            cur_dev->product_id == product_id) {
            if (serial_number) {
                if (wcscmp(serial_number, cur_dev->serial_number) == 0) {
                    path_to_open = cur_dev->path;
                    break;
                }
            }
            else {
                path_to_open = cur_dev->path;
                break;
            }
        }
        cur_dev = cur_dev->next;
    }
    
    if (path_to_open) {
        /* Open the device */
        handle = hid_open_path(path_to_open);
    }
    
    hid_free_enumeration(devs);
    
    return handle;
}

hid_device * HID_API_EXPORT hid_open_path(const char *path)
{
    hid_device *dev = NULL;
    
    dev = get_hid_device_path(path);
    if(dev)
        return dev;

    hid_init();
    
    dev = new_hid_device();
    
    if (kernel_version == 0) {
        struct utsname name;
        int major, minor, release;
        int ret;
        uname(&name);
        ret = sscanf(name.release, "%d.%d.%d", &major, &minor, &release);
        if (ret == 3) {
            kernel_version = major << 16 | minor << 8 | release;
            //printf("Kernel Version: %d\n", kernel_version);
        }
        else {
            printf("Couldn't sscanf() version string %s\n", name.release);
        }
    }
    
    // OPEN HERE //
    dev->device_handle = open(path, O_RDWR);
    
    // If we have a good handle, return it.
    if (dev->device_handle > 0) {
        
        /* Get the report descriptor */
        int res, desc_size = 0;
        struct hidraw_report_descriptor rpt_desc;
        
        memset(&rpt_desc, 0x0, sizeof(rpt_desc));
        
        /* Get Report Descriptor Size */
        res = ioctl(dev->device_handle, HIDIOCGRDESCSIZE, &desc_size);
        if (res < 0)
            perror("HIDIOCGRDESCSIZE");
        
        
        /* Get Report Descriptor */
        rpt_desc.size = desc_size;
        res = ioctl(dev->device_handle, HIDIOCGRDESC, &rpt_desc);
        if (res < 0) {
            perror("HIDIOCGRDESC");
        } else {
            /* Determine if this device uses numbered reports. */
            dev->uses_numbered_reports =
            uses_numbered_reports(rpt_desc.value,
                                  rpt_desc.size);
        }
        
        return dev;
    }
    else {
        // Unable to open any devices.
        free_hid_device(dev);
        return NULL;
    }
}

int HID_API_EXPORT hid_write_timeout(hid_device *dev, const unsigned char *data, size_t length, int milliseconds)
{
    int bytes_written;
    
    if (milliseconds != 0) {
        /* milliseconds is -1 or > 0. In both cases, we want to
         call poll() and wait for data to arrive. -1 means
         INFINITE. */
        int ret;
        struct pollfd fds;
        
        fds.fd = dev->device_handle;
        fds.events = POLLOUT;
        fds.revents = 0;
        ret = poll(&fds, 1, milliseconds);
        if (ret == -1 || ret == 0)
        /* Error or timeout */
            return ret;
    }
    
    bytes_written = write(dev->device_handle, data, length);
    
    if (bytes_written < 0 && errno == EAGAIN)
        bytes_written = 0;
    
    return bytes_written;
}



int HID_API_EXPORT hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
    int bytes_written;
    
    bytes_written = write(dev->device_handle, data, length);
    
    return bytes_written;
}


int HID_API_EXPORT hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
    int bytes_read;
    
    if (milliseconds != 0) {
        /* milliseconds is -1 or > 0. In both cases, we want to
         call poll() and wait for data to arrive. -1 means
         INFINITE. */
        int ret;
        struct pollfd fds;
        
        fds.fd = dev->device_handle;
        fds.events = POLLIN;
        fds.revents = 0;
        ret = poll(&fds, 1, milliseconds);
        if (ret == -1 || ret == 0)
        /* Error or timeout */
            return ret;
    }
    
    bytes_read = read(dev->device_handle, data, length);
    if (bytes_read < 0 && errno == EAGAIN)
        bytes_read = 0;
    
    if (bytes_read >= 0 &&
        kernel_version < KERNEL_VERSION(2,6,34) &&
        dev->uses_numbered_reports) {
        /* Work around a kernel bug. Chop off the first byte. */
        memmove(data, data+1, bytes_read);
        bytes_read--;
    }
    
    return bytes_read;
}

int HID_API_EXPORT hid_read(hid_device *dev, unsigned char *data, size_t length)
{
    return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

int HID_API_EXPORT hid_set_nonblocking(hid_device *dev, int nonblock)
{
    int flags, res;
    
    flags = fcntl(dev->device_handle, F_GETFL, 0);
    if (flags >= 0) {
        if (nonblock)
            res = fcntl(dev->device_handle, F_SETFL, flags | O_NONBLOCK);
        else
            res = fcntl(dev->device_handle, F_SETFL, flags & ~O_NONBLOCK);
    }
    else
        return -1;
    
    if (res < 0) {
        return -1;
    }
    else {
        dev->blocking = !nonblock;
        return 0; /* Success */
    }
}


int HID_API_EXPORT hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
    int res;
    
    res = ioctl(dev->device_handle, HIDIOCSFEATURE(length), data);
    if (res < 0)
        perror("ioctl (SFEATURE)");
    
    return res;
}

int HID_API_EXPORT hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
    int res;
    
    res = ioctl(dev->device_handle, HIDIOCGFEATURE(length), data);
    if (res < 0)
        perror("ioctl (GFEATURE)");
    
    
    return res;
}

#if HID_DEVICE_SUPPORT_CONNECT
//#pragma mark device connection notification

static struct hid_device_info* hid_device_info_create(struct udev_device *hid_dev)
{
    struct hid_device_info *dev_info = NULL;
    struct udev_device *intf_dev; // The device's interface (in the USB sense).
    struct udev_device *dev;
    const char *dev_path, *str;
    unsigned short dev_vid;
    unsigned short dev_pid;
    size_t len = 0;
    
      if( !hid_dev){
        return NULL;
    }
    
    /* The device pointed to by hid_dev contains information about
     the hidraw device. In order to get information about the
     USB device, get the parent device with the
     subsystem/devtype pair of "usb"/"usb_device". This will
     be several levels up the tree, but the function will find
     it.*/
    dev = udev_device_get_parent_with_subsystem_devtype(hid_dev,
                                                        "usb",
                                                        "usb_device");
    if (!dev) {
        /* Unable to find parent usb device. */
        return NULL;
    }
    
    str = udev_device_get_sysattr_value(dev,"idVendor");
    dev_vid = (str)? strtol(str, NULL, 16): 0x0;
    str = udev_device_get_sysattr_value(dev, "idProduct");
    dev_pid = (str)? strtol(str, NULL, 16): 0x0;
    
    dev_info = calloc(1,sizeof(struct hid_device_info));
    dev_info->next = NULL;
    
  
    // Set the Usage Page and Usage for this device.
    dev_info->usage_page = 0;
    dev_info->usage = 0;
    
    
    /* Fill out the record */
    dev_path = udev_device_get_devnode(hid_dev);
    str = dev_path;
    if (str) {
        len = strlen(str);
        dev_info->path = calloc(len+1, sizeof(char));
        strncpy(dev_info->path, str, len+1);
        dev_info->path[len] = '\0';
    }
    else
        dev_info->path = NULL;
    
    
    /* Serial Number */
    dev_info->serial_number
    = copy_udev_string(dev, "serial");
    
    
    /* Manufacturer and Product strings */
    dev_info->manufacturer_string
    = copy_udev_string(dev, "manufacturer");
    dev_info->product_string
    = copy_udev_string(dev, "product");
    
    
    /* VID/PID */
    dev_info->vendor_id = dev_vid;
    dev_info->product_id = dev_pid;
    
    
    /* Release Number */
    str = udev_device_get_sysattr_value(dev, "bcdDevice");
    dev_info->release_number = (str)? strtol(str, NULL, 16): 0x0;
    
    
    /* Interface Number */
    dev_info->interface_number = -1;
    /* Get a handle to the interface's udev node. */
    intf_dev = udev_device_get_parent_with_subsystem_devtype(
                                                             hid_dev,
                                                             "usb",
                                                             "usb_interface");
    
    if (intf_dev) {
        str = udev_device_get_sysattr_value(intf_dev, "bInterfaceNumber");
        dev_info->interface_number = (str)? strtol(str, NULL, 16): -1;
    }
    
    return dev_info;
}

static void hid_device_info_free(struct hid_device_info *dev_info)
{
    if(dev_info){
        if(dev_info->path)
          free(dev_info->path);
        if(dev_info->serial_number)
          free(dev_info->serial_number);
        if(dev_info->manufacturer_string)
          free(dev_info->manufacturer_string);
        if(dev_info->product_string)
          free(dev_info->product_string);
        free(dev_info);
    }
}
    
static void  hid_device_callback_connect_free(hid_device_callback_connect *dev_connect)
{
    if(dev_connect){
        free(dev_connect);
    }
}

static void hid_device_removal_callback_result(udev_notify* udev)
{
    hid_device_callback_connect *c = NULL;
    
    if(udev == NULL)
        return;
    if(udev->info == NULL)
        return;
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    c = connect_callback_list;
    while (c) {
        if (c->callback) {
            (*c->callback)(udev->info, device_removal, c->context);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&connect_callback_mutex);
}


static void hid_device_matching_callback_result(udev_notify* udev)
{
    hid_device_callback_connect *c = NULL;
    
    if(udev == NULL)
        return;
    
    if(udev->info == NULL)
        return;
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    c = connect_callback_list;
    while (c) {
        if (c->callback) {
            (*c->callback)(udev->info, device_arrival,c->context);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&connect_callback_mutex);
}

void hid_device_lock(udev_state *udev)
{
    pthread_mutex_lock(&udev->lock);
}

void hid_device_unlock(udev_state* udev)
{
    pthread_mutex_unlock(&udev->lock);
}
    
static void hid_dev_notify_free(udev_notify *dev_notify)
{
    if(dev_notify)
    {
        if(dev_notify->dev)
        {
            udev_device_unref(dev_notify->dev);
            dev_notify->dev = NULL;
        }
        free(dev_notify);
    }
}

static void hid_remove_notify(udev_notify *dev)
{
    udev_notify *c = NULL;
    
    if((!dev_state) || (!dev))
        return;
    
    pthread_mutex_lock(&dev_state->notify_lock);
    
    // Remove from list
    c = dev_state->udev_notify;
    if(c == dev){
        dev_state->udev_notify = c->next;
    }
    else
    {
        while( c ){
            if (c->next) {
                if (c->next == dev) {
                    c->next = c->next->next;
                    break;
                }
            }
            c = c->next;
        }
    }
    pthread_mutex_unlock(&dev_state->notify_lock);
    
    hid_dev_notify_free(dev);
}

static void hid_free_notify_devices()
{
    udev_notify *curr = NULL;
    udev_notify *next = NULL;
    
    pthread_mutex_lock(&dev_state->notify_lock);
    
    curr = dev_state->udev_notify;
    next = curr;
    while (NULL != curr) {
        next = curr->next;
        hid_dev_notify_free(curr);
        curr = next;
    }
    dev_state->udev_notify = NULL;
    
    pthread_mutex_unlock(&dev_state->notify_lock);
}

static udev_notify* hid_set_notify(struct udev_device *dev)
{
    udev_notify *curr = dev_state->udev_notify;
    //udev_notify *pred = NULL;    
    const char* dev_path = NULL;
    
    if(!dev)
        return NULL;
    
    dev_path = udev_device_get_devnode(dev);
    
    while (NULL != curr) {
        if (!strcmp(curr->path,dev_path))
            break;
        //pred = curr;            
        curr = curr->next;
    }
    if(NULL == curr)
    {
        udev_notify *tmp = malloc(sizeof(udev_notify));
        tmp->next = NULL;
        tmp->dev = dev;
        strcpy(tmp->path, dev_path);
        udev_device_ref(tmp->dev);
        
        tmp->info = hid_device_info_create(dev);
        
        if(dev_state->udev_notify){       
            tmp->next = dev_state->udev_notify;
            dev_state->udev_notify = tmp;
        }
        else{
            dev_state->udev_notify = tmp;
        }
        return tmp;
    }
    return curr;
}

static udev_notify *hid_get_notify(struct udev_device *dev)
{
    udev_notify *udev = NULL;
    udev_notify *curr = NULL;
    const char  *dev_path = NULL;
    
    if (!dev)
        return NULL; 
    
    dev_path = udev_device_get_devnode(dev);
    curr = dev_state->udev_notify;
    while (NULL != curr) {
        if(!strcmp(curr->path,dev_path))
        {
            udev = curr;
            break;
        }        
        curr = curr->next;
    }
    return udev;
}

static udev_notify *hid_enum_notify_devices()
{
    struct udev *udev;
    struct udev_enumerate *enumerate;
    struct udev_list_entry *devices, *dev_list_entry;
    
    udev_notify *root = NULL;
    udev_notify *curr = NULL;
    
    setlocale(LC_ALL,"");
    
    /* Create the udev object */
    udev = udev_new();
    if (!udev) {
        //printf("Can't create udev\n");
        return NULL;
    }
    
    /* Create a list of the devices in the 'hidraw' subsystem. */
    enumerate = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerate, "hidraw");
    udev_enumerate_scan_devices(enumerate);
    devices = udev_enumerate_get_list_entry(enumerate);
    /* For each item, see if it matches the vid/pid, and if so
     create a udev_device record for it */
    udev_list_entry_foreach(dev_list_entry, devices) {
        const char *sysfs_path;
        const char *dev_path;
        const char *str;
        struct udev_device *hid_dev; // The device's HID udev node.
        struct udev_device *dev; // The actual hardware device.
        struct udev_device *intf_dev; // Gets interface device        
        udev_notify *tmp;
        size_t len;        
        
        /* Get the filename of the /sys entry for the device
         and create a udev_device object (dev) representing it */
        sysfs_path = udev_list_entry_get_name(dev_list_entry);
        hid_dev = udev_device_new_from_syspath(udev, sysfs_path);
        dev_path = udev_device_get_devnode(hid_dev);
        dev = udev_device_get_parent_with_subsystem_devtype(
                                                            hid_dev,
                                                            "usb",
                                                            "usb_device");
        if (!dev) {
            /* Unable to find parent usb device. */
            goto next;
        }

        tmp = malloc(sizeof(udev_notify));
        if (curr) {
            curr->next = tmp;
        }
        else {
            root = tmp;
        }
        curr = tmp;
        /* Fill out the record */
        curr->next = NULL;
        str = dev_path;
        if (str) {
            len = strlen(str);
            strncpy(curr->path, str, len+1);
            curr->path[len] = '\0';
        }
        else
            curr->path[0] = '\0';
        udev_device_ref(hid_dev);
        curr->dev = hid_dev;
        curr->info = hid_device_info_create(hid_dev);
        
    next:
        udev_device_unref(hid_dev);
    }
    udev_enumerate_unref(enumerate);
    udev_unref(udev);
    return root;
}


static void *hid_monitor_thread(void *param)
{
    udev_state *devState = (udev_state*)param;
    struct udev_monitor *monitor = devState->udev_monitor;
    int fd = udev_monitor_get_fd(monitor);
    while(!devState->shutdown_thread)
    {
        struct udev_device* dev;
        struct timeval tv;
        fd_set fds;
        int ret;
     
        pthread_mutex_lock(&devState->lock);
        
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        
        ret = select(fd+1, &fds, NULL, NULL, &tv);
        /* Check if our file descriptor has received data. */
        if (ret > 0 && FD_ISSET(fd, &fds)) 
        {
            //printf("\nselect() says there should be data\n");
            /* Make the call to receive the device.
             select() ensured that this will not block. */
            dev = udev_monitor_receive_device(monitor);
            if (dev) {
                udev_notify* udev  = 0;
                const char *action = udev_device_get_action(dev);
                if(strcmp(action, DEV_ACTION_ADD) == 0)
                {
                    udev = hid_set_notify(dev);
                    if(udev){
                        hid_device_matching_callback_result(udev); //dev
                    }
                }
                else if(strcmp(action, DEV_ACTION_REMOVE) == 0){
                    udev = hid_get_notify(dev);
                    if(udev){
                      hid_device_removal_callback_result(udev); //dev 
                      hid_remove_notify(udev);
                    }
                }
                udev_device_unref(dev);
            }
            else {
                //printf("No Device from receive_device(). An error occured.\n");
            }                    
        }
#ifdef DEBUG
        printf("HID thread\n");
#endif    
        usleep(SLEEP_TIME);
        pthread_mutex_unlock(&devState->lock);
    }
    return NULL;
}
    
static int hid_monitor_startup()
{
    struct udev* udev = NULL;
    struct udev_monitor *udev_monitor = NULL;  
    
    if(NULL != dev_state)
        return -1;
    
    if(dev_state){
        if(!dev_state->shutdown_thread)
            return -1;
    }
    udev = udev_new();
    if (!udev) {
        return -1;
    }
    
    udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
    if(NULL != udev_monitor)
    {
        dev_state = calloc(1, sizeof(udev_state));
        dev_state->udev_monitor = udev_monitor;
        dev_state->udev_notify = hid_enum_notify_devices();    
        dev_state->shutdown_thread = 0;
        dev_state->start_add = 0;
        
        udev_monitor_filter_add_match_subsystem_devtype(dev_state->udev_monitor, "hidraw", NULL);
        udev_monitor_enable_receiving(dev_state->udev_monitor);
        
        pthread_mutex_init(&dev_state->lock, NULL);
        pthread_mutex_init(&dev_state->notify_lock, NULL);
        pthread_create(&dev_state->thread, NULL, hid_monitor_thread, dev_state);
         
        return 0;   
    }
    udev_unref(udev);
    return -1;
}

static int hid_monitor_shutdown(void)
{
    int ret = 0;
    struct udev_monitor *udev_monitor = NULL;
    struct udev *udev = NULL;
    
    if (dev_state) {
        dev_state->shutdown_thread = 1;
        pthread_join(dev_state->thread,NULL);
        pthread_mutex_lock(&dev_state->lock);
        udev_monitor = DRV_STATE_UDEV_MONITOR(dev_state);
        if (udev_monitor != NULL) {
            udev = udev_monitor_get_udev(udev_monitor);
            udev_monitor_unref(udev_monitor);
        }
        if (udev != NULL) {
            udev_unref(udev);
        }
        
        hid_free_notify_devices();
        
        pthread_mutex_unlock(&dev_state->lock);
        pthread_mutex_destroy(&dev_state->lock);
        pthread_mutex_destroy(&dev_state->notify_lock);
        
        free(dev_state);
        dev_state = NULL;
    }    
    
    return ret;
}

static int hid_connect_registered(hid_device_callback callBack, void *context)
{
    hid_device_callback_connect *c = NULL;
    if (!connect_callback_list){
        return -1;
    }
    c = connect_callback_list;
    while (c) 
    {
        if (c->callback == callBack && c->context == context) {
            return 0;
        }
        c = c->next;
    }
    return -1;
}

int hid_init_connect()
{
   hid_monitor_startup();
   return TRUE;
}

void hid_deinit_connect()
{
   hid_monitor_shutdown();
}

static int hid_register_add_callback(hid_device_callback callBack, hid_device_context context)
{
    int result = -1;
    hid_device_callback_connect *hid_connect = NULL;
    
    if(NULL == callBack)
        return result;
    
    if(!hid_init_connect())
        return -1;
    
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    // check if has been registered callback
    if(hid_connect_registered(callBack,context)==0){
        pthread_mutex_unlock(&connect_callback_mutex);
        return 0;
    }
    hid_connect = calloc(1, sizeof(hid_device_callback_connect));
    hid_connect->callback = callBack;
    hid_connect->context = context;
    hid_connect->next = NULL;
    
    // add callback in the list
    if (!connect_callback_list)
        connect_callback_list = hid_connect;
    else {
        hid_device_callback_connect *c = connect_callback_list;
        while (c->next) {
            c = c->next;
        }
        c->next = hid_connect;
    }
    pthread_mutex_unlock(&connect_callback_mutex);
    return 0;
}
    
static void hid_register_remove_callback(hid_device_callback callBack, hid_device_context context)
{
    hid_device_callback_connect *c = NULL;
    hid_device_callback_connect *d = NULL;
    
    if(NULL == callBack)
        return;
    
    if(NULL == connect_callback_list)
        return;
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    c = connect_callback_list;
    
    if (c->callback  == callBack && c->context == context) {
        d = connect_callback_list;
        connect_callback_list = c->next;
    }
    else {
        while (c) {
            if (c->next) {
                if (c->next->callback == callBack && c->next->context == context) {
                    d  = c->next; 
                    c->next = c->next->next;
                    break;
                }
            }
            c = c->next;
        }
    }
    if(NULL != d){
        if(d == connect_callback_list) {
            connect_callback_list = NULL;
        }
        hid_device_callback_connect_free(d);
        if(connect_callback_list==NULL){
            hid_device_info_free(connect_device_info);
            connect_device_info = NULL;
        }
    }
    pthread_mutex_unlock(&connect_callback_mutex);
}

int  HID_API_EXPORT HID_API_CALL hid_add_notification_callback(hid_device_callback callBack, void *context)
{
    int result = -1;
    /* register device matching callback */
    result = hid_register_add_callback(callBack, context);
    return result;
}

void HID_API_EXPORT HID_API_CALL hid_remove_notification_callback(hid_device_callback  callBack, void *context)
{
    /* register device remove callback */
    hid_register_remove_callback(callBack,context);
}

void HID_API_EXPORT HID_API_CALL hid_remove_all_notification_callbacks(void)
{
    hid_device_callback_connect *c = NULL;
    hid_device_callback_connect *p = NULL;
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    c = connect_callback_list;
    
    while (c) {
        p = c;
        c = c->next;
        hid_device_callback_connect_free(p);
    }
    hid_device_info_free(connect_device_info);
    connect_device_info = NULL;
    connect_callback_list = NULL;
    pthread_mutex_unlock(&connect_callback_mutex);
    hid_deinit_connect();
}

#endif

//#pragma mark device connection notification


void HID_API_EXPORT hid_close(hid_device *dev)
{
    if(!dev)
        return;
    
    if(!dev->ref_count)
        return;
    
    if(dev->ref_count > 1){
        dev->ref_count--;
        return;
    }
    
    dev->ref_count = 0; 
    
    free_hid_device(dev);
    
}

int HID_API_EXPORT_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    return get_device_string(dev, "manufacturer", string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    return get_device_string(dev, "product", string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    return get_device_string(dev, "serial", string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
    return -1;
}


HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
    return NULL;
}
