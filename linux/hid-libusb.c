/*******************************************************
HIDAPI - Multi-Platform library for
communication with HID devices.

Alan Ott
Signal 11 Software

8/22/2009
Linux Version - 6/2/2010
Libusb Version - 8/13/2010

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
#include <ctype.h>
#include <locale.h>
#include <errno.h>

/* Unix */
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <pthread.h>
#include <linux/hidraw.h>
#include <linux/version.h>
#include <libudev.h> 

/* GNU / LibUSB */
#include "libusb.h"
#include "iconv.h"

#include "hidapi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DEBUG_PRINTF
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...) do {} while (0)
#endif

#define  HID_DEVICE_SUPPORT_CONNECT 1
#define  TRUE                       1
#define  FALSE                      0
#define  KERN_SUCCESS               0
#define  OK                         0
#define  FAILURE                    -1
#define  MAX_PATH                   256
#define  INVALID_HANDLE             0xFFFF    
#define  SLEEP_TIME                 250 * 1000
//#define  DEBUG

#if HID_DEVICE_SUPPORT_CONNECT 

enum devicematch
{
    HID_ADD_MATCH = 0,
    HID_NO_MATCH = -2,
    HID_REMOVE_MATCH = -1    
};


typedef struct _libusb_state libusb_state;
typedef    struct _libusb_device_notify libusb_device_notify;

struct _libusb_device_notify {
    int interface_num;
    char path[MAX_PATH];
    unsigned short vid;
    unsigned short pid;
    int fconnect;        
    libusb_device_handle *handle;
    libusb_device *dev;
    libusb_device_notify *next;
};

struct _libusb_state {
    libusb_device_notify *dev_notify;
    int num_devs;
    int shutdown_thread;    
    pthread_mutex_t thread_lock;
    pthread_mutex_t notify_lock;
    pthread_t thread;
};


/* Register callbacks implementation*/

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
static libusb_state *dev_state  = NULL;

/* Static list of all the devices open. This way when a device gets
 disconnected, its hid_device structure can be marked as disconnected
 from hid_device_removal_callback(). */
static hid_device *device_list = NULL;
static pthread_mutex_t device_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t connect_callback_mutex = PTHREAD_MUTEX_INITIALIZER;


/* Declare of inner functions*/

int hid_init_connect();
void hid_deinit_connect();

#endif //HID_DEVICE_SUPPORT_CONNECT

/* Uncomment to enable the retrieval of Usage and Usage Page in
 hid_enumerate(). Warning, this is very invasive as it requires the detach
 and re-attach of the kernel driver. See comments inside hid_enumerate().
 Linux/libusb HIDAPI programs are encouraged to use the interface number
 instead to differentiate between interfaces on a composite HID device. */
/*#define INVASIVE_GET_USAGE*/

/* Linked List of input reports received from the device. */
struct input_report {
    uint8_t *data;
    size_t len;
    struct input_report *next;
};


struct hid_device_ {
    /* Handle to the actual device. */
    libusb_device_handle *device_handle;
    
    /* Endpoint information */
    int input_endpoint;
    int output_endpoint;
    int input_ep_max_packet_size;
    
    /* The interface number of the HID */    
    int interface;
    
    /* Indexes of Strings */
    int manufacturer_index;
    int product_index;
    int serial_index;
    
    /* Whether blocking reads are used */
    int blocking; /* boolean */
    
    /* Read thread objects */
    pthread_t thread;
    pthread_mutex_t mutex; /* Protects input_reports */
    pthread_cond_t condition;
    pthread_barrier_t barrier; /* Ensures correct startup sequence */
    int shutdown_thread;
    struct libusb_transfer *transfer;
    
    /* List of received input reports. */
    struct input_report *input_reports;
    int ref_count; /* Reference count of the opened devices */
    hid_device *next;    
};

static int initialized = 0;

uint16_t get_usb_code_for_current_locale(void);
static int return_data(hid_device *dev, unsigned char *data, size_t length);

static hid_device *new_hid_device(void)
{
    hid_device *dev = calloc(1, sizeof(hid_device));
    dev->device_handle = NULL;
    dev->input_endpoint = 0;
    dev->output_endpoint = 0;
    dev->input_ep_max_packet_size = 0;
    dev->interface = 0;
    dev->manufacturer_index = 0;
    dev->product_index = 0;
    dev->serial_index = 0;
    dev->blocking = 1;
    dev->shutdown_thread = 0;
    dev->transfer = NULL;
    dev->input_reports = NULL;
    
    pthread_mutex_init(&dev->mutex, NULL);
    pthread_cond_init(&dev->condition, NULL);
    pthread_barrier_init(&dev->barrier, NULL, 2);
    
    dev->ref_count = 1;
    
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
    
    /* Clean up the thread objects */
    pthread_barrier_destroy(&dev->barrier);
    pthread_cond_destroy(&dev->condition);
    pthread_mutex_destroy(&dev->mutex);
    
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
    
    /* Free the device itself */
    libusb_close(dev->device_handle);
    free(dev);
}

#if 0
//TODO: Implement this funciton on Linux.
static void register_error(hid_device *device, const char *op)
{
    
}
#endif

#ifdef INVASIVE_GET_USAGE
/* Get bytes from a HID Report Descriptor.
 Only call with a num_bytes of 0, 1, 2, or 4. */
static uint32_t get_bytes(uint8_t *rpt, size_t len, size_t num_bytes, size_t cur)
{
    /* Return if there aren't enough bytes. */
    if (cur + num_bytes >= len)
        return 0;
    
    if (num_bytes == 0)
        return 0;
    else if (num_bytes == 1) {
        return rpt[cur+1];
    }
    else if (num_bytes == 2) {
        return (rpt[cur+2] * 256 + rpt[cur+1]);
    }
    else if (num_bytes == 4) {
        return (rpt[cur+4] * 0x01000000 +
                rpt[cur+3] * 0x00010000 +
                rpt[cur+2] * 0x00000100 +
                rpt[cur+1] * 0x00000001);
    }
    else
        return 0;
}

/* Retrieves the device's Usage Page and Usage from the report
 descriptor. The algorithm is simple, as it just returns the first
 Usage and Usage Page that it finds in the descriptor.
 The return value is 0 on success and -1 on failure. */
static int get_usage(uint8_t *report_descriptor, size_t size,
                     unsigned short *usage_page, unsigned short *usage)
{
    int i = 0;
    int size_code;
    int data_len, key_size;
    int usage_found = 0, usage_page_found = 0;
    
    while (i < size) {
        int key = report_descriptor[i];
        int key_cmd = key & 0xfc;
        
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
        
        if (key_cmd == 0x4) {
            *usage_page  = get_bytes(report_descriptor, size, data_len, i);
            usage_page_found = 1;
            //printf("Usage Page: %x\n", (uint32_t)*usage_page);
        }
        if (key_cmd == 0x8) {
            *usage = get_bytes(report_descriptor, size, data_len, i);
            usage_found = 1;
            //printf("Usage: %x\n", (uint32_t)*usage);
        }
        
        if (usage_page_found && usage_found)
            return 0; /* success */
        
        /* Skip over this key and it's associated data */
        i += data_len + key_size;
    }
    
    return -1; /* failure */
}
#endif // INVASIVE_GET_USAGE


/* Get the first language the device says it reports. This comes from
 USB string #0. */
static uint16_t get_first_language(libusb_device_handle *dev)
{
    uint16_t buf[32];
    int len;
    
    /* Get the string from libusb. */
    len = libusb_get_string_descriptor(dev,
                                       0x0, /* String ID */
                                       0x0, /* Language */
                                       (unsigned char*)buf,
                                       sizeof(buf));
    if (len < 4)
        return 0x0;
    
    return buf[1]; // First two bytes are len and descriptor type.
}

static int is_language_supported(libusb_device_handle *dev, uint16_t lang)
{
    uint16_t buf[32];
    int len;
    int i;
    
    /* Get the string from libusb. */
    len = libusb_get_string_descriptor(dev,
                                       0x0, /* String ID */
                                       0x0, /* Language */
                                       (unsigned char*)buf,
                                       sizeof(buf));
    if (len < 4)
        return 0x0;
    
    
    len /= 2; /* language IDs are two-bytes each. */
    /* Start at index 1 because there are two bytes of protocol data. */
    for (i = 1; i < len; i++) {
        if (buf[i] == lang)
            return 1;
    }
    
    return 0;
}


/* This function returns a newly allocated wide string containing the USB
 device string numbered by the index. The returned string must be freed
 by using free(). */
static wchar_t *get_usb_string(libusb_device_handle *dev, uint8_t idx)
{
    char buf[512];
    int len;
    wchar_t *str = NULL;
    wchar_t wbuf[256];
    
    /* iconv variables */
    iconv_t ic;
    size_t inbytes;
    size_t outbytes;
    size_t res;
    char *inptr;
    char *outptr;
    
    /* Determine which language to use. */
    uint16_t lang;
    lang = get_usb_code_for_current_locale();
    if (!is_language_supported(dev, lang))
        lang = get_first_language(dev);
    
    /* Get the string from libusb. */
    len = libusb_get_string_descriptor(dev,
                                       idx,
                                       lang,
                                       (unsigned char*)buf,
                                       sizeof(buf));
    if (len < 0)
        return NULL;
    
    buf[sizeof(buf)-1] = '\0';
    
    if (len+1 < sizeof(buf))
        buf[len+1] = '\0';
    
    /* Initialize iconv. */
    ic = iconv_open("UTF-32", "UTF-16");
    if (ic == (iconv_t)-1)
        return NULL;
    
    /* Convert to UTF-32 (wchar_t on glibc systems).
     Skip the first character (2-bytes). */
    inptr = buf+2;
    inbytes = len-2;
    outptr = (char*) wbuf;
    outbytes = sizeof(wbuf);
    res = iconv(ic, &inptr, &inbytes, &outptr, &outbytes);
    if (res == (size_t)-1)
        goto err;
    
    /* Write the terminating NULL. */
    wbuf[sizeof(wbuf)/sizeof(wbuf[0])-1] = 0x00000000;
    if (outbytes >= sizeof(wbuf[0]))
        *((wchar_t*)outptr) = 0x00000000;
    
    /* Allocate and copy the string. */
    str = wcsdup(wbuf+1);
    
err:
    iconv_close(ic);
    
    return str;
}

static char *make_path(libusb_device *dev, int interface_number)
{
    char str[64];
    snprintf(str, sizeof(str), "%04x:%04x:%02x",
             libusb_get_bus_number(dev),
             libusb_get_device_address(dev),
             interface_number);
    str[sizeof(str)-1] = '\0';
    
    return strdup(str);
}

//#pragma mark get opened device

static hid_device* get_hid_device_path(const char *path)
{
#define BUF_LEN  256
    hid_device *d = NULL;
    hid_device *dev = NULL;    
    char *dev_path = NULL;
    
    pthread_mutex_lock(&device_list_mutex);
    
    if (!device_list){
        pthread_mutex_unlock(&device_list_mutex);
        return NULL;
    }
    else {
        d = device_list;
        while (d) {
            if (d->device_handle > 0) {
                libusb_device *usb_dev = libusb_get_device(d->device_handle);        
                dev_path = make_path(usb_dev, d->interface);
                if (!strcmp(dev_path, path)) {
                    d->ref_count++;
                    dev = d; 
                    free(dev_path);
                    break;
                }
                free(dev_path);
            }
            d = d->next;
        }
    }
    pthread_mutex_unlock(&device_list_mutex);
    
    return dev;
}


int hid_init(void)
{
    if (!initialized) {
        libusb_init(NULL);
        hid_init_connect();
        initialized = 1;
    }
    return 0;
}

int hid_exit(void)
{
    /* Nothing to do for this in the Linux/hidraw implementation. */
    if (initialized) {
        hid_deinit_connect();
        libusb_exit(NULL);
        initialized = 0;
    }
    return 0;
}

struct hid_device_info  HID_API_EXPORT *hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
    libusb_device **devs;
    libusb_device *dev;
    libusb_device_handle *handle;
    ssize_t num_devs;
    int i = 0;
    
    struct hid_device_info *root = NULL; // return object
    struct hid_device_info *cur_dev = NULL;
    
    setlocale(LC_ALL,"");
    
    if (!initialized) {
        libusb_init(NULL);
        initialized = 1;
    }
    
    num_devs = libusb_get_device_list(NULL, &devs);
    if (num_devs < 0)
        return NULL;
    while ((dev = devs[i++]) != NULL) {
        struct libusb_device_descriptor desc;
        struct libusb_config_descriptor *conf_desc = NULL;
        int j, k;
        int interface_num = 0;
        
        int res = libusb_get_device_descriptor(dev, &desc);
        unsigned short dev_vid = desc.idVendor;
        unsigned short dev_pid = desc.idProduct;
        
        /* HID's are defined at the interface level. */
        if (desc.bDeviceClass != LIBUSB_CLASS_PER_INTERFACE)
            continue;
        
        res = libusb_get_active_config_descriptor(dev, &conf_desc);
        if (res < 0)
            libusb_get_config_descriptor(dev, 0, &conf_desc);
        if (conf_desc) {
            for (j = 0; j < conf_desc->bNumInterfaces; j++) {
                const struct libusb_interface *intf = &conf_desc->interface[j];
                for (k = 0; k < intf->num_altsetting; k++) {
                    const struct libusb_interface_descriptor *intf_desc;
                    intf_desc = &intf->altsetting[k];
                    if (intf_desc->bInterfaceClass == LIBUSB_CLASS_HID) {
                        interface_num = intf_desc->bInterfaceNumber;
                        
                        /* Check the VID/PID against the arguments */
                        if ((vendor_id == 0x0 && product_id == 0x0) ||
                            (vendor_id == dev_vid && product_id == dev_pid)) {
                            struct hid_device_info *tmp;
                            
                            /* VID/PID match. Create the record. */
                            tmp = calloc(1, sizeof(struct hid_device_info));
                            if (cur_dev) {
                                cur_dev->next = tmp;
                            }
                            else {
                                root = tmp;
                            }
                            cur_dev = tmp;
                            
                            /* Fill out the record */
                            cur_dev->next = NULL;
                            cur_dev->path = make_path(dev, interface_num);
                            
                            res = libusb_open(dev, &handle);
                            
                            if (res >= 0) {
                                /* Serial Number */
                                if (desc.iSerialNumber > 0)
                                    cur_dev->serial_number =
                                    get_usb_string(handle, desc.iSerialNumber);
                                
                                /* Manufacturer and Product strings */
                                if (desc.iManufacturer > 0)
                                    cur_dev->manufacturer_string =
                                    get_usb_string(handle, desc.iManufacturer);
                                if (desc.iProduct > 0)
                                    cur_dev->product_string =
                                    get_usb_string(handle, desc.iProduct);
                                
#ifdef INVASIVE_GET_USAGE
                                /*
                                 This section is removed because it is too
                                 invasive on the system. Getting a Usage Page
                                 and Usage requires parsing the HID Report
                                 descriptor. Getting a HID Report descriptor
                                 involves claiming the interface. Claiming the
                                 interface involves detaching the kernel driver.
                                 Detaching the kernel driver is hard on the system
                                 because it will unclaim interfaces (if another
                                 app has them claimed) and the re-attachment of
                                 the driver will sometimes change /dev entry names.
                                 It is for these reasons that this section is
                                 #if 0. For composite devices, use the interface
                                 field in the hid_device_info struct to distinguish
                                 between interfaces. */
                                int detached = 0;
                                unsigned char data[256];
                                
                                /* Usage Page and Usage */
                                res = libusb_kernel_driver_active(handle, interface_num);
                                if (res == 1) {
                                    res = libusb_detach_kernel_driver(handle, interface_num);
                                    if (res < 0)
                                        LOG("Couldn't detach kernel driver, even though a kernel driver was attached.");
                                    else
                                        detached = 1;
                                }
                                res = libusb_claim_interface(handle, interface_num);
                                if (res >= 0) {
                                    /* Get the HID Report Descriptor. */
                                    res = libusb_control_transfer(handle, LIBUSB_ENDPOINT_IN|LIBUSB_RECIPIENT_INTERFACE, LIBUSB_REQUEST_GET_DESCRIPTOR, (LIBUSB_DT_REPORT << 8)|interface_num, 0, data, sizeof(data), 5000);
                                    if (res >= 0) {
                                        unsigned short page=0, usage=0;
                                        /* Parse the usage and usage page
                                         out of the report descriptor. */
                                        get_usage(data, res,  &page, &usage);
                                        cur_dev->usage_page = page;
                                        cur_dev->usage = usage;
                                    }
                                    else
                                        LOG("libusb_control_transfer() for getting the HID report failed with %d\n", res);
                                    
                                    /* Release the interface */
                                    res = libusb_release_interface(handle, interface_num);
                                    if (res < 0)
                                        LOG("Can't release the interface.\n");
                                }
                                else
                                    LOG("Can't claim interface %d\n", res);
                                
                                /* Re-attach kernel driver if necessary. */
                                if (detached) {
                                    res = libusb_attach_kernel_driver(handle, interface_num);
                                    if (res < 0)
                                        LOG("Couldn't re-attach kernel driver.\n");
                                }
#endif /*******************/
                                
                                libusb_close(handle);
                            }
                            /* VID/PID */
                            cur_dev->vendor_id = dev_vid;
                            cur_dev->product_id = dev_pid;
                            
                            /* Release Number */
                            cur_dev->release_number = desc.bcdDevice;
                            
                            /* Interface Number */
                            cur_dev->interface_number = interface_num;
                        }
                    }
                } /* altsettings */
            } /* interfaces */
            libusb_free_config_descriptor(conf_desc);
        }
    }
    
    libusb_free_device_list(devs, 1);
    
    return root;
}

void  HID_API_EXPORT hid_free_enumeration(struct hid_device_info *devs)
{
    struct hid_device_info *d = devs;
    while (d) {
        struct hid_device_info *next = d->next;
        if(d->path){
          free(d->path);
        }
        if(d->serial_number){
          free(d->serial_number);
        }
        if(d->manufacturer_string){
          free(d->manufacturer_string);
        }
        if(d->product_string) {
          free(d->product_string);
        }
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

static void read_callback(struct libusb_transfer *transfer)
{
    hid_device *dev = transfer->user_data;
    
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        
        struct input_report *rpt = malloc(sizeof(*rpt));
        rpt->data = malloc(transfer->actual_length);
        memcpy(rpt->data, transfer->buffer, transfer->actual_length);
        rpt->len = transfer->actual_length;
        rpt->next = NULL;
        
        pthread_mutex_lock(&dev->mutex);
        
        /* Attach the new report object to the end of the list. */
        if (dev->input_reports == NULL) {
            /* The list is empty. Put it at the root. */
            dev->input_reports = rpt;
            pthread_cond_signal(&dev->condition);
        }
        else {
            /* Find the end of the list and attach. */
            struct input_report *cur = dev->input_reports;
            int num_queued = 0;
            while (cur->next != NULL) {
                cur = cur->next;
                num_queued++;
            }
            
            cur->next = rpt;
            
            /* Pop one off if we've reached 30 in the queue. This
             way we don't grow forever if the user never reads
             anything from the device. */
            if (num_queued > 30) {
                return_data(dev, NULL, 0);
            }            
        }
        pthread_mutex_unlock(&dev->mutex);
    }
    else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        dev->shutdown_thread = 1;
        return;
    }
    else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        dev->shutdown_thread = 1;
        return;
    }
    else if (transfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
        //LOG("Timeout (normal)\n");
    }
    else {
        LOG("Unknown transfer code: %d\n", transfer->status);
    }
    
    /* Re-submit the transfer object. */
    libusb_submit_transfer(transfer);
}


static void *read_thread(void *param)
{
    hid_device *dev = param;
    unsigned char *buf;
    const size_t length = dev->input_ep_max_packet_size;
    
    /* Set up the transfer object. */
    buf = malloc(length);
    dev->transfer = libusb_alloc_transfer(0);
    libusb_fill_interrupt_transfer(dev->transfer,
                                   dev->device_handle,
                                   dev->input_endpoint,
                                   buf,
                                   length,
                                   read_callback,
                                   dev,
                                   5000/*timeout*/);
    
    /* Make the first submission. Further submissions are made
     from inside read_callback() */
    libusb_submit_transfer(dev->transfer);
    
    // Notify the main thread that the read thread is up and running.
    pthread_barrier_wait(&dev->barrier);
    
    /* Handle all the events. */
    while (!dev->shutdown_thread) {
        int res;
        struct timeval tv;
        
        tv.tv_sec = 0;
        tv.tv_usec = 100; //TODO: Fix this value.
        res = libusb_handle_events_timeout(NULL, &tv);
        if (res < 0) {
            /* There was an error. Break out of this loop. */
            break;
        }
    }
    
    /* Cancel any transfer that may be pending. This call will fail
     if no transfers are pending, but that's OK. */
    if (libusb_cancel_transfer(dev->transfer) == 0) {
        /* The transfer was cancelled, so wait for its completion. */
        libusb_handle_events(NULL);
    }
    
    /* The dev->transfer->buffer and dev->transfer objects are cleaned up
     in hid_close(). They are not cleaned up here because this thread
     could end either due to a disconnect or due to a user
     call to hid_close(). In both cases the objects can be safely
     cleaned up after the call to pthread_join() (in hid_close()), but
     since hid_close() calls libusb_cancel_transfer(), on these objects,
     they can not be cleaned up here. */
    
    return NULL;
}


hid_device * HID_API_EXPORT hid_open_path(const char *path)
{
    hid_device *dev = NULL;
    
    dev = get_hid_device_path(path);
    
    if(dev)
        return dev;
    
    dev = new_hid_device();
    
    libusb_device **devs;
    libusb_device *usb_dev;
    ssize_t num_devs;
    int res;
    int d = 0;
    int good_open = 0;
    
    setlocale(LC_ALL,"");
    
    if (!initialized) {
        libusb_init(NULL);
        initialized = 1;
    }
    
    num_devs = libusb_get_device_list(NULL, &devs);
    if (num_devs < 0)
        return NULL;
    while ((usb_dev = devs[d++]) != NULL) {
        struct libusb_device_descriptor desc;
        struct libusb_config_descriptor *conf_desc = NULL;
        int i,j,k;
        libusb_get_device_descriptor(usb_dev, &desc);
        
        if (libusb_get_active_config_descriptor(usb_dev, &conf_desc) < 0)
            continue;
        for (j = 0; j < conf_desc->bNumInterfaces; j++) {
            const struct libusb_interface *intf = &conf_desc->interface[j];
            for (k = 0; k < intf->num_altsetting; k++) {
                const struct libusb_interface_descriptor *intf_desc;
                intf_desc = &intf->altsetting[k];
                if (intf_desc->bInterfaceClass == LIBUSB_CLASS_HID) {
                    char *dev_path = make_path(usb_dev, intf_desc->bInterfaceNumber);
                    if (!strcmp(dev_path, path)) {
                        /* Matched Paths. Open this device */
                        
                        // OPEN HERE //
                        res = libusb_open(usb_dev, &dev->device_handle);
                        if (res < 0) {
                            LOG("can't open device\n");
                            break;
                        }
                        good_open = 1;
                        
                        /* Detach the kernel driver, but only if the
                         device is managed by the kernel */
                        if (libusb_kernel_driver_active(dev->device_handle, intf_desc->bInterfaceNumber) == 1) {
                            res = libusb_detach_kernel_driver(dev->device_handle, intf_desc->bInterfaceNumber);
                            if (res < 0) {
                                libusb_close(dev->device_handle);
                                LOG("Unable to detach Kernel Driver\n");
                                good_open = 0;
                                break;
                            }
                        }
                        
                        res = libusb_claim_interface(dev->device_handle, intf_desc->bInterfaceNumber);
                        if (res < 0) {
                            LOG("can't claim interface %d: %d\n", intf_desc->bInterfaceNumber, res);
                            libusb_close(dev->device_handle);
                            good_open = 0;
                            break;
                        }
                        
                        /* Store off the string descriptor indexes */
                        dev->manufacturer_index = desc.iManufacturer;
                        dev->product_index      = desc.iProduct;
                        dev->serial_index       = desc.iSerialNumber;
                        
                        /* Store off the interface number */
                        dev->interface = intf_desc->bInterfaceNumber;
                        
                        /* Find the INPUT and OUTPUT endpoints. An
                         OUTPUT endpoint is not required. */
                        for (i = 0; i < intf_desc->bNumEndpoints; i++) {
                            const struct libusb_endpoint_descriptor *ep
                            = &intf_desc->endpoint[i];
                            
                            /* Determine the type and direction of this
                             endpoint. */
                            int is_interrupt =
                            (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK)
                            == LIBUSB_TRANSFER_TYPE_INTERRUPT;
                            int is_output = 
                            (ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK)
                            == LIBUSB_ENDPOINT_OUT;
                            int is_input = 
                            (ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK)
                            == LIBUSB_ENDPOINT_IN;
                            
                            /* Decide whether to use it for intput or output. */
                            if (dev->input_endpoint == 0 &&
                                is_interrupt && is_input) {
                                /* Use this endpoint for INPUT */
                                dev->input_endpoint = ep->bEndpointAddress;
                                dev->input_ep_max_packet_size = ep->wMaxPacketSize;
                            }
                            if (dev->output_endpoint == 0 &&
                                is_interrupt && is_output) {
                                /* Use this endpoint for OUTPUT */
                                dev->output_endpoint = ep->bEndpointAddress;
                            }
                        }
                        
                        pthread_create(&dev->thread, NULL, read_thread, dev);
                        
                        // Wait here for the read thread to be initialized.
                        pthread_barrier_wait(&dev->barrier);
                        
                    }
                    free(dev_path);
                }
            }
        }
        libusb_free_config_descriptor(conf_desc);
        
    }
    
    libusb_free_device_list(devs, 1);
    
    // If we have a good handle, return it.
    if (good_open) {
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
    int res;
    int report_number = data[0];
    int skipped_report_id = 0;
    
    if (report_number == 0x0) {
        data++;
        length--;
        skipped_report_id = 1;
    }
    
    if (dev->output_endpoint <= 0) {
        /* No interrput out endpoint. Use the Control Endpoint */
        res = libusb_control_transfer(dev->device_handle,
                                      LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE|LIBUSB_ENDPOINT_OUT,
                                      0x09/*HID Set_Report*/,
                                      (2/*HID output*/ << 8) | report_number,
                                      dev->interface,
                                      (unsigned char *)data, length,
                                      milliseconds/*timeout millis*/);
            
        if (res < 0)
            return -1;
            
        if (skipped_report_id)
            length++;
            
        return length;
    }
    else {
        /* Use the interrupt out endpoint */
        int actual_length;
        res = libusb_interrupt_transfer(dev->device_handle,
                                        dev->output_endpoint,
                                        (unsigned char*)data,
                                        length,
                                        &actual_length, milliseconds);
            
        if (res < 0)
            return -1;
            
        if (skipped_report_id)
            actual_length++;
        
        return actual_length;
    }
}
    
int HID_API_EXPORT hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
    return hid_write_timeout(dev, data, length, 0);// was 1000 
}
    

/* Helper function, to simplify hid_read().
 This should be called with dev->mutex locked. */
static int return_data(hid_device *dev, unsigned char *data, size_t length)
{
    /* Copy the data out of the linked list item (rpt) into the
     return buffer (data), and delete the liked list item. */
    struct input_report *rpt = dev->input_reports;
    size_t len = (length < rpt->len)? length: rpt->len;
    if (len > 0)
        memcpy(data, rpt->data, len);
    dev->input_reports = rpt->next;
    free(rpt->data);
    free(rpt);
    return len;
}

int HID_API_EXPORT hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
    int bytes_read = -1;
       
#if 0
    int transferred;
    int res = libusb_interrupt_transfer(dev->device_handle, dev->input_endpoint, data, length, &transferred, 5000);
    LOG("transferred: %d\n", transferred);
    return transferred;
#endif
        
    pthread_mutex_lock(&dev->mutex);
        
    /* There's an input report queued up. Return it. */
    if (dev->input_reports) {
        /* Return the first one */
        bytes_read = return_data(dev, data, length);
        goto ret;
    }
        
    if (dev->shutdown_thread) {
        /* This means the device has been disconnected.
         An error code of -1 should be returned. */
        bytes_read = -1;
        goto ret;
    }
    
    if (dev->blocking) {
        pthread_cond_wait(&dev->condition, &dev->mutex);
        bytes_read = return_data(dev, data, length);
    }
    else if (milliseconds > 0) {
        /* Non-blocking, but called with timeout. */
        int res;
        struct timespec ts;
        struct timeval tv;
        TIMEVAL_TO_TIMESPEC(&tv, &ts);
        ts.tv_sec += milliseconds / 1000;
        ts.tv_nsec += (milliseconds % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        res = cond_timedwait(dev, &dev->condition, &dev->mutex, &ts);
        if (res == 0)
            bytes_read = return_data(dev, data, length);
        else if (res == ETIMEDOUT)
            bytes_read = 0;
        else
            bytes_read = -1;
    }
    else {
        bytes_read = 0;
    }
        
ret:
    pthread_mutex_unlock(&dev->mutex);
        
    return bytes_read;
}
    
int HID_API_EXPORT hid_read(hid_device *dev, unsigned char *data, size_t length)
{
    return hid_read_timeout(dev, data, length, 0);    
}

int HID_API_EXPORT hid_set_nonblocking(hid_device *dev, int nonblock)
{
    dev->blocking = !nonblock;
    
    return 0;
}


int HID_API_EXPORT hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
    int res = -1;
    int skipped_report_id = 0;
    int report_number = data[0];
    
    if (report_number == 0x0) {
        data++;
        length--;
        skipped_report_id = 1;
    }
    
    res = libusb_control_transfer(dev->device_handle,
                                  LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE|LIBUSB_ENDPOINT_OUT,
                                  0x09/*HID set_report*/,
                                  (3/*HID feature*/ << 8) | report_number,
                                  dev->interface,
                                  (unsigned char *)data, length,
                                  1000/*timeout millis*/);
    
    if (res < 0)
        return -1;
    
    /* Account for the report ID */
    if (skipped_report_id)
        length++;
    
    return length;
}

int HID_API_EXPORT hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
    int res = -1;
    int skipped_report_id = 0;
    int report_number = data[0];
    
    if (report_number == 0x0) {
        /* Offset the return buffer by 1, so that the report ID
         will remain in byte 0. */
        data++;
        length--;
        skipped_report_id = 1;
    }
    res = libusb_control_transfer(dev->device_handle,
                                  LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE|LIBUSB_ENDPOINT_IN,
                                  0x01/*HID get_report*/,
                                  (3/*HID feature*/ << 8) | report_number,
                                  dev->interface,
                                  (unsigned char *)data, length,
                                  1000/*timeout millis*/);
    
    if (res < 0)
        return -1;
    
    if (skipped_report_id)
        res++;
    
    return res;
}

#if HID_DEVICE_SUPPORT_CONNECT

static struct hid_device_info* hid_device_info_create(libusb_device_notify *hid_dev)
{
    struct hid_device_info *dev_info = NULL;
    struct libusb_device *dev = NULL;
    struct libusb_device_handle *handle = NULL;
    struct libusb_device_descriptor desc;
    int res = -1;
    
    if(!hid_dev){
        return NULL;
    }
    setlocale(LC_ALL,"");
    
    dev_info = calloc(1, sizeof(struct hid_device_info));
    dev_info->path = strdup(hid_dev->path);
    
    dev = hid_dev->dev;
    handle = hid_dev->handle;
    
    dev_info->vendor_id =  hid_dev->vid;
    dev_info->product_id = hid_dev->pid;
    
    if(dev){
        libusb_get_device_descriptor(dev, &desc);
        if(NULL == handle){
            res = libusb_open(dev, &handle);
        }
    }
    dev_info->serial_number = 0;
    dev_info->manufacturer_string = 0;
    dev_info->product_string = 0;
        
    if(res>=0)
    {
        /* Serial Number */
        if (desc.iSerialNumber > 0)
            dev_info->serial_number = get_usb_string(handle, desc.iSerialNumber);
        /* Manufacturer and Product strings */
        if (desc.iManufacturer > 0)
            dev_info->manufacturer_string = get_usb_string(handle, desc.iManufacturer);
        if (desc.iProduct > 0)
            dev_info->product_string = get_usb_string(handle, desc.iProduct);
        libusb_close(handle);
        hid_dev->handle = NULL;
    } 
    dev_info->interface_number = hid_dev->interface_num;
    
    return dev_info;
}

static void hid_device_info_free(struct hid_device_info *dev_info)
{
    if(dev_info){
        if(dev_info->path){
          free(dev_info->path);
        }
        if(dev_info->serial_number){
          free(dev_info->serial_number);
        }
        if(dev_info->manufacturer_string){
          free(dev_info->manufacturer_string);
        }
        if(dev_info->product_string){
          free(dev_info->product_string);
        }
        free(dev_info);
    }
}


static void  hid_device_callback_connect_free(hid_device_callback_connect *dev_connect)
{
    if(dev_connect){
        free(dev_connect);
    }
}

static void hid_device_removal_callback_result(libusb_device_notify *hid_dev)
{
    hid_device_callback_connect *c = NULL;
    
    if(NULL!=connect_device_info){
        hid_device_info_free(connect_device_info);
    }
    connect_device_info = hid_device_info_create(hid_dev);
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    c = connect_callback_list;
    while (c) {
        if (c->callback) {
            (*c->callback)(connect_device_info, device_removal, c->context);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&connect_callback_mutex);
}

static void hid_device_matching_callback_result(libusb_device_notify *hid_dev)
{
    hid_device_callback_connect *c = NULL;
    
    if(NULL!=connect_device_info){
        hid_device_info_free(connect_device_info);
    }
    
    connect_device_info = hid_device_info_create(hid_dev);
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    c = connect_callback_list;
    while (c) {
        if (c->callback) {
            (*c->callback)(connect_device_info, device_arrival,c->context);
        }
        c = c->next;
    }
    
    pthread_mutex_unlock(&connect_callback_mutex);
}

void hid_dev_notify_free(libusb_device_notify *dev_notify)
{
    if(dev_notify){
        if(dev_notify->handle > 0)
        {
            libusb_close(dev_notify->handle);
            dev_notify->handle = NULL;
        }
        if(dev_notify->dev)
        {
            libusb_unref_device(dev_notify->dev);
            dev_notify->dev = NULL;
        }
        free(dev_notify);
    }
}

void hid_remove_notify(libusb_device_notify *dev)
{
    libusb_device_notify *d = NULL;
    libusb_device_notify *c = NULL;
    
    if((!dev_state) || (!dev))
        return;
    
    pthread_mutex_lock(&dev_state->notify_lock);
    // Remove from list
    c = dev_state->dev_notify;
    if(c == dev){
        dev_state->dev_notify = c->next;
        d = dev;
    }
    else
    {
        while( c ){
            if (c->next) {
                if (c->next == dev) {
                    d  = dev; 
                    c->next = c->next->next;
                    break;
                }
            }
            c = c->next;
        }
    }
    
    if(d){
        dev_state->num_devs-=1;
    }
    pthread_mutex_unlock(&dev_state->notify_lock);
    hid_dev_notify_free(dev);
}

void hid_free_notify_devices()
{
    libusb_device_notify *curr = NULL;
    libusb_device_notify *next = NULL;
    
    pthread_mutex_lock(&dev_state->notify_lock);
    
    curr = dev_state->dev_notify;
    next = curr;
    while (NULL != curr) {
        next = curr->next;
        hid_dev_notify_free(curr);
        curr = next;
    }
    dev_state->dev_notify = NULL;
    dev_state->num_devs = 0;
    
    pthread_mutex_unlock(&dev_state->notify_lock);
}

libusb_device_notify *hid_enum_notify_devices()
{
    libusb_device **devs;
    libusb_device *dev;
    
    libusb_device_notify *root = NULL;
    libusb_device_notify *curr = NULL;
    
    ssize_t num_devs;
    ssize_t num_devs_notify;
    
    int i = 0;
    
    setlocale(LC_ALL,"");
    
    if (!initialized)
        hid_init();
    
    num_devs = libusb_get_device_list(NULL, &devs);
    if (num_devs < 0)
        return NULL;
    num_devs_notify = 0;
    while ((dev = devs[i++]) != NULL) {
        struct libusb_device_descriptor desc;
        struct libusb_config_descriptor *conf_desc = NULL;
        int j, k;
        int interface_num = 0;
        int res = libusb_get_device_descriptor(dev, &desc);
        unsigned short dev_vid = desc.idVendor;
        unsigned short dev_pid = desc.idProduct;
        
        if (desc.bDeviceClass != LIBUSB_CLASS_PER_INTERFACE)
            continue;
        res = libusb_get_active_config_descriptor(dev, &conf_desc);
        if (res < 0)
            libusb_get_config_descriptor(dev, 0, &conf_desc);
        if (conf_desc) {
            for (j = 0; j < conf_desc->bNumInterfaces; j++) {
                const struct libusb_interface *intf = &conf_desc->interface[j];
                for (k = 0; k < intf->num_altsetting; k++) {
                    const struct libusb_interface_descriptor *intf_desc;
                    intf_desc = &intf->altsetting[k];
                    if (intf_desc->bInterfaceClass == LIBUSB_CLASS_HID) {
                        interface_num = intf_desc->bInterfaceNumber;
                        libusb_device_notify *tmp;
                        char *dev_path = NULL;
                        tmp = calloc(1, sizeof(libusb_device_notify));
                        if (curr) {
                            curr->next = tmp;
                        }
                        else {
                            root = tmp;
                        }
                        curr = tmp;
                        num_devs_notify++;
                        /* Fill out the record */
                        dev_path = make_path(dev, interface_num);
                        strcpy(curr->path, dev_path);
                        free(dev_path);
                        curr->dev = libusb_ref_device(dev);
                        curr->interface_num = interface_num;
                        curr->fconnect = HID_ADD_MATCH;
                        curr->vid = dev_vid;
                        curr->pid = dev_pid;
                        curr->handle = NULL;
                        curr->next = NULL;
                    }
                }
            }/* interfaces*/
            libusb_free_config_descriptor(conf_desc);
        }
    }
    libusb_free_device_list(devs, 1);
    dev_state->num_devs = num_devs_notify;
    return root;
}    


int set_devices_flag_connect(int flag)
{
    libusb_device_notify *d = dev_state->dev_notify;
    while(d)
    {
        d->fconnect = flag;
        d=d->next;
    }
    return 0;
}

int printf_devices_info()
{
    libusb_device_notify *d = dev_state->dev_notify;
    while(d)
    {
        LOG("Device name = %s type: %04hx %04hx\n", d->path, d->vid, d->pid);
        d = d->next;
    }
    return 0;
}
    
    
int check_devices_on_connect(libusb_device_notify **dev_notify)
{
    libusb_device **devs;
    libusb_device *dev;
    libusb_device_notify *root = NULL;
    libusb_state *state = dev_state;
    ssize_t num_devs = 0;
    ssize_t num_hid_devs = 0;
    
    int isConnect = HID_NO_MATCH;
    int isQuit = 0;
    int i = 0;
    
    if(!dev_notify || !state)
        return -1;
    
    root = state->dev_notify;
    
    (*dev_notify) = NULL;
    
    setlocale(LC_ALL,"");
    
    num_devs = libusb_get_device_list(NULL, &devs);
    if (num_devs < 0){
        *dev_notify = NULL;
        return HID_NO_MATCH;
    }
    // set devices to remove
    set_devices_flag_connect(HID_REMOVE_MATCH);
    
    while ((dev = devs[i++]) != NULL && (!isQuit)) {
        struct libusb_device_descriptor desc;
        struct libusb_config_descriptor *conf_desc = NULL;
        int j, k;
        int interface_num = 0;
        int res = libusb_get_device_descriptor(dev, &desc);
        unsigned short dev_vid = desc.idVendor;
        unsigned short dev_pid = desc.idProduct;
        
        if (desc.bDeviceClass != LIBUSB_CLASS_PER_INTERFACE)
            continue;
        res = libusb_get_active_config_descriptor(dev, &conf_desc);
        if (res < 0)
            libusb_get_config_descriptor(dev, 0, &conf_desc);
        if (conf_desc) {
            for (j = 0; j < conf_desc->bNumInterfaces && (!isQuit); j++) {
                const struct libusb_interface *intf = &conf_desc->interface[j];
                for (k = 0; k < intf->num_altsetting && (!isQuit); k++) {
                    const struct libusb_interface_descriptor *intf_desc;
                    intf_desc = &intf->altsetting[k];
                    if (intf_desc->bInterfaceClass == LIBUSB_CLASS_HID) {
                        libusb_device_notify *d = root;
                        isConnect = HID_ADD_MATCH;
                        while(d){
                            if(d->vid == dev_vid && d->pid == dev_pid) {
                                isConnect = HID_REMOVE_MATCH;
                                d->fconnect = HID_NO_MATCH;
                                break;    
                            }
                            d=d->next;
                        }
                        num_hid_devs++;
                        if(isConnect == HID_ADD_MATCH) { 
                            //new device
                            interface_num = intf_desc->bInterfaceNumber;
                            char *dev_path = make_path(dev, interface_num);
                            libusb_device_notify *tmp = calloc(1, sizeof(libusb_device_notify));
                            tmp->next = NULL;
                            if(root) {
                                tmp->next = root;
                            }
                            root = tmp;                
                            /* Fill out the record */
                            strcpy(tmp->path, dev_path);
                            free(dev_path);
                            tmp->vid = dev_vid;
                            tmp->pid = dev_pid;
                            tmp->dev = libusb_ref_device(dev);
                            tmp->interface_num = interface_num;
                            tmp->handle = NULL;
                            tmp->fconnect = HID_ADD_MATCH;                        
                            (*dev_notify) = tmp;
                            isQuit = 1;
                        }
                    }
                }
            }
            libusb_free_config_descriptor(conf_desc);
        }
    }
    libusb_free_device_list(devs, 1);
    dev_state->dev_notify = root;
    if(isConnect == HID_ADD_MATCH){ 
        state->num_devs++;
    }
    else
    {
        if(num_hid_devs == state->num_devs){
            isConnect = HID_NO_MATCH;
        }
    }
    return isConnect;
}

int check_devices_on_disconnect(libusb_device_notify **dev)
{
    int res = -1;
    if(dev && dev_state)
    {
        libusb_device_notify *root = dev_state->dev_notify;
        libusb_device_notify *d = root;
        (*dev) = NULL;
        while(d){
            if(d->fconnect==HID_REMOVE_MATCH)
                break;
            d = d->next;    
        }
        if(d){        
            (*dev) = d;
            res = 0;
        }
    }
    return res;
}

static int hid_init_register_connect_callbacks()
{
    // register inner callbacks
    if(pthread_mutex_init(&connect_callback_mutex,NULL)){
        LOG("Could not init a mutex\n");
        return -1;
    }
    return 0;
}
    
static int hid_deinit_register_connect_callbacks()
{
    // unregister inner callbacks
    pthread_mutex_destroy(&connect_callback_mutex);
    return 0;
}
    
static void *hid_monitor_thread(void *param)
{
    int res = HID_NO_MATCH;
    libusb_state *devState = (libusb_state *)param;
    if(NULL == devState)
        return NULL;    
    
    pthread_mutex_lock(&devState->thread_lock);
    
    while(!devState->shutdown_thread)
    {
        libusb_device_notify *dev = NULL;
        res = check_devices_on_connect(&dev);
        if(res >= 0)
        {
            // arrival new device
            hid_device_matching_callback_result(dev);    
        }
        else
        {
            // removal device    
            if(HID_NO_MATCH != res) {
                res = check_devices_on_disconnect(&dev);                   
                if(res >= 0 )
                {
                    hid_device_removal_callback_result(dev);
                    hid_remove_notify(dev);
                }
            }
        }
#ifdef DEBUG
        printf("HID thread\n");
#endif        
        usleep(SLEEP_TIME);
    }
    pthread_mutex_unlock(&devState->thread_lock);
    return NULL;
}


static int hid_monitor_startup()
{
    if(NULL != dev_state)
        return -1;
    
    if(dev_state){
        if(!dev_state->shutdown_thread)
            return -1;
    }
    
    dev_state = calloc(1, sizeof(libusb_state));
    dev_state->shutdown_thread = 0;
    dev_state->dev_notify = NULL;
    dev_state->dev_notify = hid_enum_notify_devices();
    
    pthread_mutex_init(&dev_state->thread_lock, NULL);
    pthread_mutex_init(&dev_state->notify_lock, NULL);
    pthread_create(&dev_state->thread, NULL, hid_monitor_thread, dev_state);
    
    return 0;
}

static int hid_monitor_shutdown(void)
{
    int ret = 0;
    if (dev_state) 
    {
        dev_state->shutdown_thread = 1;
        pthread_join(dev_state->thread,NULL);
        
        pthread_mutex_unlock(&dev_state->thread_lock);
        pthread_mutex_destroy(&dev_state->thread_lock);
        
        hid_free_notify_devices();
        
        pthread_mutex_unlock(&dev_state->notify_lock);
        pthread_mutex_destroy(&dev_state->notify_lock);
        
        free(dev_state);
        
        dev_state = NULL;
    }    
    return ret;
}


static int hid_connect_registered(hid_device_callback callBack, hid_device_context context)
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
    if(NULL==connect_callback_list){
        hid_monitor_startup();
    }
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

static void hid_register_remove_callback(hid_device_callback callBack, hid_device_context context )
{
    hid_device_callback_connect *c = NULL;
    hid_device_callback_connect *d = NULL;
    
    if (NULL == callBack)
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
    
    if (NULL != d) {
        if (d == connect_callback_list) {
            connect_callback_list = NULL;
        }
        
        hid_device_callback_connect_free(d);
        
        if (connect_callback_list==NULL) {
            hid_device_info_free(connect_device_info);
            connect_device_info = NULL;
        }
    }
    pthread_mutex_unlock(&connect_callback_mutex);
}

int  HID_API_EXPORT HID_API_CALL hid_add_notification_callback(hid_device_callback callBack, void *context)
{
    int result = -1;
    
    /* Set up the HID Manager if it hasn't been done */
    
    if (KERN_SUCCESS != hid_init())
        return result;
    
    /* register device matching callback */
    
    result = hid_register_add_callback(callBack, context);
    
    return result;
}

void HID_API_EXPORT HID_API_CALL hid_remove_notification_callback(hid_device_callback  callBack, void *context)
{
    /* Set up the HID Manager if it hasn't been done */
    if(KERN_SUCCESS != hid_init())
        return;
    /* register device remove callback */
    hid_register_remove_callback(callBack, context);
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

void HID_API_EXPORT hid_close(hid_device *dev)
{
    if (!dev)
        return;
    
    if (!dev->ref_count)
        return;
    
    if (dev->ref_count > 1) {
        dev->ref_count--;
        return;
    }
    
    dev->ref_count = 0; 
    
    /* Cause read_thread() to stop. */
    dev->shutdown_thread = 1;
    libusb_cancel_transfer(dev->transfer);
    
    /* Wait for read_thread() to end. */
    pthread_join(dev->thread, NULL);
    
    /* Clean up the Transfer objects allocated in read_thread(). */
    free(dev->transfer->buffer);
    libusb_free_transfer(dev->transfer);
    
    /* release the interface */
    libusb_release_interface(dev->device_handle, dev->interface);
    
    /* Close the handle */
    libusb_close(dev->device_handle);
    
    /* Clear out the queue of received reports. */
    pthread_mutex_lock(&dev->mutex);
    while (dev->input_reports) {
        return_data(dev, NULL, 0);
    }
    pthread_mutex_unlock(&dev->mutex);
    
    free_hid_device(dev);
}


int HID_API_EXPORT_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    return hid_get_indexed_string(dev, dev->manufacturer_index, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    return hid_get_indexed_string(dev, dev->product_index, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    return hid_get_indexed_string(dev, dev->serial_index, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
    wchar_t *str;
    
    str = get_usb_string(dev->device_handle, string_index);
    if (str) {
        wcsncpy(string, str, maxlen);
        string[maxlen-1] = L'\0';
        free(str);
        return 0;
    }
    else
        return -1;
}


HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
    return NULL;
}


struct lang_map_entry {
    const char *name;
    const char *string_code;
    uint16_t usb_code;
};

#define LANG(name,code,usb_code) { name, code, usb_code }
static struct lang_map_entry lang_map[] = {
    LANG("Afrikaans", "af", 0x0436),
    LANG("Albanian", "sq", 0x041C),
    LANG("Arabic - United Arab Emirates", "ar_ae", 0x3801),
    LANG("Arabic - Bahrain", "ar_bh", 0x3C01),
    LANG("Arabic - Algeria", "ar_dz", 0x1401),
    LANG("Arabic - Egypt", "ar_eg", 0x0C01),
    LANG("Arabic - Iraq", "ar_iq", 0x0801),
    LANG("Arabic - Jordan", "ar_jo", 0x2C01),
    LANG("Arabic - Kuwait", "ar_kw", 0x3401),
    LANG("Arabic - Lebanon", "ar_lb", 0x3001),
    LANG("Arabic - Libya", "ar_ly", 0x1001),
    LANG("Arabic - Morocco", "ar_ma", 0x1801),
    LANG("Arabic - Oman", "ar_om", 0x2001),
    LANG("Arabic - Qatar", "ar_qa", 0x4001),
    LANG("Arabic - Saudi Arabia", "ar_sa", 0x0401),
    LANG("Arabic - Syria", "ar_sy", 0x2801),
    LANG("Arabic - Tunisia", "ar_tn", 0x1C01),
    LANG("Arabic - Yemen", "ar_ye", 0x2401),
    LANG("Armenian", "hy", 0x042B),
    LANG("Azeri - Latin", "az_az", 0x042C),
    LANG("Azeri - Cyrillic", "az_az", 0x082C),
    LANG("Basque", "eu", 0x042D),
    LANG("Belarusian", "be", 0x0423),
    LANG("Bulgarian", "bg", 0x0402),
    LANG("Catalan", "ca", 0x0403),
    LANG("Chinese - China", "zh_cn", 0x0804),
    LANG("Chinese - Hong Kong SAR", "zh_hk", 0x0C04),
    LANG("Chinese - Macau SAR", "zh_mo", 0x1404),
    LANG("Chinese - Singapore", "zh_sg", 0x1004),
    LANG("Chinese - Taiwan", "zh_tw", 0x0404),
    LANG("Croatian", "hr", 0x041A),
    LANG("Czech", "cs", 0x0405),
    LANG("Danish", "da", 0x0406),
    LANG("Dutch - Netherlands", "nl_nl", 0x0413),
    LANG("Dutch - Belgium", "nl_be", 0x0813),
    LANG("English - Australia", "en_au", 0x0C09),
    LANG("English - Belize", "en_bz", 0x2809),
    LANG("English - Canada", "en_ca", 0x1009),
    LANG("English - Caribbean", "en_cb", 0x2409),
    LANG("English - Ireland", "en_ie", 0x1809),
    LANG("English - Jamaica", "en_jm", 0x2009),
    LANG("English - New Zealand", "en_nz", 0x1409),
    LANG("English - Phillippines", "en_ph", 0x3409),
    LANG("English - Southern Africa", "en_za", 0x1C09),
    LANG("English - Trinidad", "en_tt", 0x2C09),
    LANG("English - Great Britain", "en_gb", 0x0809),
    LANG("English - United States", "en_us", 0x0409),
    LANG("Estonian", "et", 0x0425),
    LANG("Farsi", "fa", 0x0429),
    LANG("Finnish", "fi", 0x040B),
    LANG("Faroese", "fo", 0x0438),
    LANG("French - France", "fr_fr", 0x040C),
    LANG("French - Belgium", "fr_be", 0x080C),
    LANG("French - Canada", "fr_ca", 0x0C0C),
    LANG("French - Luxembourg", "fr_lu", 0x140C),
    LANG("French - Switzerland", "fr_ch", 0x100C),
    LANG("Gaelic - Ireland", "gd_ie", 0x083C),
    LANG("Gaelic - Scotland", "gd", 0x043C),
    LANG("German - Germany", "de_de", 0x0407),
    LANG("German - Austria", "de_at", 0x0C07),
    LANG("German - Liechtenstein", "de_li", 0x1407),
    LANG("German - Luxembourg", "de_lu", 0x1007),
    LANG("German - Switzerland", "de_ch", 0x0807),
    LANG("Greek", "el", 0x0408),
    LANG("Hebrew", "he", 0x040D),
    LANG("Hindi", "hi", 0x0439),
    LANG("Hungarian", "hu", 0x040E),
    LANG("Icelandic", "is", 0x040F),
    LANG("Indonesian", "id", 0x0421),
    LANG("Italian - Italy", "it_it", 0x0410),
    LANG("Italian - Switzerland", "it_ch", 0x0810),
    LANG("Japanese", "ja", 0x0411),
    LANG("Korean", "ko", 0x0412),
    LANG("Latvian", "lv", 0x0426),
    LANG("Lithuanian", "lt", 0x0427),
    LANG("F.Y.R.O. Macedonia", "mk", 0x042F),
    LANG("Malay - Malaysia", "ms_my", 0x043E),
    LANG("Malay – Brunei", "ms_bn", 0x083E),
    LANG("Maltese", "mt", 0x043A),
    LANG("Marathi", "mr", 0x044E),
    LANG("Norwegian - Bokml", "no_no", 0x0414),
    LANG("Norwegian - Nynorsk", "no_no", 0x0814),
    LANG("Polish", "pl", 0x0415),
    LANG("Portuguese - Portugal", "pt_pt", 0x0816),
    LANG("Portuguese - Brazil", "pt_br", 0x0416),
    LANG("Raeto-Romance", "rm", 0x0417),
    LANG("Romanian - Romania", "ro", 0x0418),
    LANG("Romanian - Republic of Moldova", "ro_mo", 0x0818),
    LANG("Russian", "ru", 0x0419),
    LANG("Russian - Republic of Moldova", "ru_mo", 0x0819),
    LANG("Sanskrit", "sa", 0x044F),
    LANG("Serbian - Cyrillic", "sr_sp", 0x0C1A),
    LANG("Serbian - Latin", "sr_sp", 0x081A),
    LANG("Setsuana", "tn", 0x0432),
    LANG("Slovenian", "sl", 0x0424),
    LANG("Slovak", "sk", 0x041B),
    LANG("Sorbian", "sb", 0x042E),
    LANG("Spanish - Spain (Traditional)", "es_es", 0x040A),
    LANG("Spanish - Argentina", "es_ar", 0x2C0A),
    LANG("Spanish - Bolivia", "es_bo", 0x400A),
    LANG("Spanish - Chile", "es_cl", 0x340A),
    LANG("Spanish - Colombia", "es_co", 0x240A),
    LANG("Spanish - Costa Rica", "es_cr", 0x140A),
    LANG("Spanish - Dominican Republic", "es_do", 0x1C0A),
    LANG("Spanish - Ecuador", "es_ec", 0x300A),
    LANG("Spanish - Guatemala", "es_gt", 0x100A),
    LANG("Spanish - Honduras", "es_hn", 0x480A),
    LANG("Spanish - Mexico", "es_mx", 0x080A),
    LANG("Spanish - Nicaragua", "es_ni", 0x4C0A),
    LANG("Spanish - Panama", "es_pa", 0x180A),
    LANG("Spanish - Peru", "es_pe", 0x280A),
    LANG("Spanish - Puerto Rico", "es_pr", 0x500A),
    LANG("Spanish - Paraguay", "es_py", 0x3C0A),
    LANG("Spanish - El Salvador", "es_sv", 0x440A),
    LANG("Spanish - Uruguay", "es_uy", 0x380A),
    LANG("Spanish - Venezuela", "es_ve", 0x200A),
    LANG("Southern Sotho", "st", 0x0430),
    LANG("Swahili", "sw", 0x0441),
    LANG("Swedish - Sweden", "sv_se", 0x041D),
    LANG("Swedish - Finland", "sv_fi", 0x081D),
    LANG("Tamil", "ta", 0x0449),
    LANG("Tatar", "tt", 0X0444),
    LANG("Thai", "th", 0x041E),
    LANG("Turkish", "tr", 0x041F),
    LANG("Tsonga", "ts", 0x0431),
    LANG("Ukrainian", "uk", 0x0422),
    LANG("Urdu", "ur", 0x0420),
    LANG("Uzbek - Cyrillic", "uz_uz", 0x0843),
    LANG("Uzbek – Latin", "uz_uz", 0x0443),
    LANG("Vietnamese", "vi", 0x042A),
    LANG("Xhosa", "xh", 0x0434),
    LANG("Yiddish", "yi", 0x043D),
    LANG("Zulu", "zu", 0x0435),
    LANG(NULL, NULL, 0x0),    
};

uint16_t get_usb_code_for_current_locale(void)
{
    char *locale;
    char search_string[64];
    char *ptr;
    
    /* Get the current locale. */
    locale = setlocale(0, NULL);
    if (!locale)
        return 0x0;
    
    /* Make a copy of the current locale string. */
    strncpy(search_string, locale, sizeof(search_string));
    search_string[sizeof(search_string)-1] = '\0';
    
    /* Chop off the encoding part, and make it lower case. */
    ptr = search_string;
    while (*ptr) {
        *ptr = tolower(*ptr);
        if (*ptr == '.') {
            *ptr = '\0';
            break;
        }
        ptr++;
    }
    
    /* Find the entry which matches the string code of our locale. */
    struct lang_map_entry *lang = lang_map;
    while (lang->string_code) {
        if (!strcmp(lang->string_code, search_string)) {
            return lang->usb_code;
        }    
        lang++;
    }
    
    /* There was no match. Find with just the language only. */
    /* Chop off the variant. Chop it off at the '_'. */
    ptr = search_string;
    while (*ptr) {
        *ptr = tolower(*ptr);
        if (*ptr == '_') {
            *ptr = '\0';
            break;
        }
        ptr++;
    }
    
#if 0 // TODO: Do we need this?
    /* Find the entry which matches the string code of our language. */
    lang = lang_map;
    while (lang->string_code) {
        if (!strcmp(lang->string_code, search_string)) {
            return lang->usb_code;
        }    
        lang++;
    }
#endif
    
    /* Found nothing. */
    return 0x0;
}

#ifdef __cplusplus
}
#endif
