/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.
    
 Alan Ott
 Signal 11 Software

 2010-07-03
            
 Copyright 2010, All Rights Reserved.
 
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
     
/* See Apple Technical Note TN2187 for details on IOHidManager. */

#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include <wchar.h>
#include <locale.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

#include "hidapi.h"
#define MAX_PATH 256
//#define HID_DEBUG
    
        
typedef struct _device_notify device_notify;
    
struct _device_notify {
    IOHIDDeviceRef device;
    char path[MAX_PATH];
    device_notify *next;
};

static device_notify *dev_notify_list = NULL;
static Boolean start_dev_list = true; 
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
static void hid_device_matching_callback( void *context, IOReturn result,
                                          void *sender, IOHIDDeviceRef dev_ref );

static void hid_device_removal_callback( void *context, IOReturn result,
                                         void *sender, IOHIDDeviceRef dev_ref );

static hid_device_callback_connect *connect_callback_list = NULL;
static struct hid_device_info *connect_device_info = NULL;
static pthread_mutex_t connect_callback_mutex = PTHREAD_MUTEX_INITIALIZER;
    
    
/* Barrier implementation because Mac OSX doesn't have pthread_barrier.
   It also doesn't have clock_gettime(). So much for POSIX and SUSv2.
   This implementation came from Brent Priddy and was posted on
StackOverflow. It is used with his permission. */
typedef int pthread_barrierattr_t;
typedef struct pthread_barrier {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int trip_count;
} pthread_barrier_t;

static int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,unsigned int count)
{
    if(count == 0) {
       errno = EINVAL;
       return -1;
    }

    if(pthread_mutex_init(&barrier->mutex, 0) < 0) {
      return -1;
    }
    if(pthread_cond_init(&barrier->cond, 0) < 0) {
       pthread_mutex_destroy(&barrier->mutex);
       return -1;
    }
    barrier->trip_count = count;
    barrier->count = 0;

    return 0;
}
    
static int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    pthread_cond_destroy(&barrier->cond);
    pthread_mutex_destroy(&barrier->mutex);
    return 0;
}

static int pthread_barrier_wait(pthread_barrier_t *barrier)
{
    pthread_mutex_lock(&barrier->mutex);
    
    ++(barrier->count);
    if(barrier->count >= barrier->trip_count)
    {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    }
    else
    {
        pthread_cond_wait(&barrier->cond, &(barrier->mutex));
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

static int return_data(hid_device *dev, unsigned char *data, size_t length);

/* Linked List of input reports received from the device. */
struct input_report {
    uint8_t *data;
    size_t len;
    struct input_report *next;
};

struct hid_device_ {
    IOHIDDeviceRef device_handle;
    int blocking;
    int uses_numbered_reports;
    int disconnected;
    CFStringRef run_loop_mode;
    CFRunLoopRef run_loop;
    CFRunLoopSourceRef source;
    uint8_t *input_report_buf;
    CFIndex max_input_report_len;
    struct input_report *input_reports;
    
    pthread_t thread;
    pthread_mutex_t mutex; /* Protects input_reports */
    pthread_cond_t condition;
    pthread_barrier_t barrier; /* Ensures correct startup sequence */
    pthread_barrier_t shutdown_barrier; /* Ensures correct shutdown sequence */
    int shutdown_thread;
    int ref_count; /* refcount on open/close device */
    
    hid_device *next;
};

/* Static list of all the devices open. This way when a device gets
   disconnected, its hid_device structure can be marked as disconnected
   from hid_device_removal_callback(). */
static hid_device *device_list = NULL;
static pthread_mutex_t device_list_mutex = PTHREAD_MUTEX_INITIALIZER;
    
static int exist_hid_device(hid_device *dev)
{
    hid_device *d = NULL;
    int isExist = FALSE;
    
    pthread_mutex_trylock(&device_list_mutex);
    d = device_list;
    while (d) {
       if (d == dev) {
          isExist = TRUE;
          break;
        }
       d = d->next;
    }
    pthread_mutex_unlock(&device_list_mutex);
    return isExist;
}

static hid_device *new_hid_device(void)
{
    hid_device *dev = calloc(1, sizeof(hid_device));
    dev->device_handle = NULL;
    dev->blocking = 1;
    dev->uses_numbered_reports = 0;
    dev->disconnected = 0;
    dev->run_loop_mode = NULL;
    dev->run_loop = NULL;
    dev->source = NULL;
    dev->input_report_buf = NULL;
    dev->input_reports = NULL;
    dev->shutdown_thread = 0;
    dev->ref_count = 1;
    dev->next = NULL;
    
    /* Thread objects */
    pthread_mutex_init(&dev->mutex, NULL);
    pthread_cond_init(&dev->condition, NULL);
    pthread_barrier_init(&dev->barrier, NULL, 2);
    pthread_barrier_init(&dev->shutdown_barrier, NULL, 2);
    
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
    
    /* Delete any input reports still left over. */
    struct input_report *rpt = dev->input_reports;
    while (rpt) {
      struct input_report *next = rpt->next;
      free(rpt->data);
      free(rpt);
      rpt = next;
    }
    
    /* Free the string and the report buffer. The check for NULL
       is necessary here as CFRelease() doesn't handle NULL like
       free() and others do. */
    if (dev->run_loop_mode)
       CFRelease(dev->run_loop_mode);
    if (dev->source)
       CFRelease(dev->source);
    free(dev->input_report_buf);
    
    /* Clean up the thread objects */
    pthread_barrier_destroy(&dev->shutdown_barrier);
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
    /* Free the structure itself. */
    free(dev);
}

static     IOHIDManagerRef hid_mgr = 0x0;


#if 0
static void register_error(hid_device *device, const char *op)
{

}
#endif


static int32_t get_int_property(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef ref;
    int32_t value;
    
    ref = IOHIDDeviceGetProperty(device, key);
    if (ref) {
       if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
           CFNumberGetValue((CFNumberRef) ref, kCFNumberSInt32Type, &value);
           return value;
       }
    }
    return 0;
}

static unsigned short get_vendor_id(IOHIDDeviceRef device)
{
    return get_int_property(device, CFSTR(kIOHIDVendorIDKey));
}

static unsigned short get_product_id(IOHIDDeviceRef device)
{
    return get_int_property(device, CFSTR(kIOHIDProductIDKey));
}


static int32_t get_max_report_length(IOHIDDeviceRef device)
{
    return get_int_property(device, CFSTR(kIOHIDMaxInputReportSizeKey));
}

static int get_string_property(IOHIDDeviceRef device, CFStringRef prop, wchar_t *buf, size_t len)
{
    CFStringRef str;
    if (!len) return 0;
    
    str = IOHIDDeviceGetProperty(device, prop);
    
    buf[0] = 0;
    
    if (str) {
        len --;
        CFIndex strLen = CFStringGetLength(str);
        CFRange range;
        range.location = 0;
        range.length = strLen > len ? len : strLen;
        CFIndex used_buf_len;
          CFIndex numCharsCopied;
        numCharsCopied = CFStringGetBytes(str,
                         range,
                         kCFStringEncodingUTF32LE,
                         (char)'?',
                         FALSE,
                         (UInt8*)buf,
                         len,
                         &used_buf_len);
        buf[numCharsCopied] = 0;
        //return numCharsCopied;
        return 0; 
    }
    else
       //return 0;
       return -1;
}

static int get_string_property_utf8(IOHIDDeviceRef device, CFStringRef prop, char *buf, size_t len)
{
    CFStringRef str;
    if (!len) return 0;
    
    str = IOHIDDeviceGetProperty(device, prop);
    
    buf[0] = 0;
    
    if (str) {
        len--;
        CFIndex strLen = CFStringGetLength(str);
        CFRange range;
        range.location = 0;
        range.length = strLen > len ? len : strLen;
        CFIndex used_buf_len;
        CFIndex numCharsCopied;
        numCharsCopied = CFStringGetBytes(str,
                         range,
                         kCFStringEncodingUTF8,
                         (char)'?',
                         FALSE,
                         (UInt8*)buf,
                         len,
                         &used_buf_len);
        buf[numCharsCopied] = 0;
        return (int)used_buf_len;
    }
    else
        return 0;
}

static int get_serial_number(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
    return get_string_property(device, CFSTR(kIOHIDSerialNumberKey), buf, len);
}

static int get_manufacturer_string(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
    return get_string_property(device, CFSTR(kIOHIDManufacturerKey), buf, len);
}

static int get_product_string(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
    return get_string_property(device, CFSTR(kIOHIDProductKey), buf, len);
}


/* Implementation of wcsdup() for Mac. */
static wchar_t *dup_wcs(const wchar_t *s)
{
    size_t len = wcslen(s);
    wchar_t *ret = malloc((len+1)*sizeof(wchar_t));
    wcscpy(ret, s);

    return ret;
}


static int make_path(IOHIDDeviceRef device, char *buf, size_t len)
{
    int res;
    unsigned short vid, pid;
    char transport[32];

    buf[0] = '\0';

    res = get_string_property_utf8(
          device, CFSTR(kIOHIDTransportKey),
          transport, sizeof(transport));
    
    if (!res)
       return -1;

    vid = get_vendor_id(device);
    pid = get_product_id(device);

    res = snprintf(buf, len, "%s_%04hx_%04hx_%p",
                   transport, vid, pid, device);
    
    
    buf[len-1] = '\0';
    return res+1;
}

#pragma mark get opened device


static hid_device* get_hid_device_path(const char *path)
{
#define BUF_LEN  256
    hid_device* dev = NULL;
    char cbuf[BUF_LEN];
    size_t len = 0;
    
    pthread_mutex_lock(&device_list_mutex);
    
    if (!device_list){
        pthread_mutex_unlock(&device_list_mutex);
        return NULL;
    }
    else {
        hid_device *d = device_list;
        while (d) {
            if( !d->disconnected ){
                len = make_path(d->device_handle, cbuf, sizeof(cbuf));
                if (!strcmp(cbuf, path)) {
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

static device_notify* hid_find_notify_device(IOHIDDeviceRef devRef)
{
    device_notify *curr = NULL;
    device_notify *res = NULL;
    curr = dev_notify_list;
    while (NULL != curr) {
        char cbuf[MAX_PATH];
        size_t len = 0;
        len = make_path(devRef, cbuf, sizeof(cbuf));
        if(!strcmp(curr->path,cbuf)){
            res = curr;
            break;
        }
        curr = curr->next;
    }
    return res;
}

static void hid_free_notify_devices()
{
    device_notify *curr = NULL;
    device_notify *next = NULL;
    
    curr = dev_notify_list;
    next = curr;
    while (NULL != curr) {
        next = curr->next;
        CFRelease(curr->device);
        free(curr);
        curr = next;
    }
    dev_notify_list = NULL;
}

static void hid_remove_notify(device_notify *dev)
{
    device_notify *c = dev_notify_list;
    
    if((!dev_notify_list) || (!dev))
        return;
    
    if(c == dev){
        dev_notify_list = c->next;
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
    CFRelease(dev->device);
    free(dev);
}

static void hid_insert_notify(device_notify *dev)
{
    if(dev){
        dev->next = dev_notify_list;  
        dev_notify_list = dev;
    }
}

static device_notify *hid_new_notify(IOHIDDeviceRef dev_ref)
{
    device_notify *dev;
    char cbuf[MAX_PATH];
    size_t len = 0;
    
    len = make_path(dev_ref, cbuf, sizeof(cbuf));
    dev = calloc(1, sizeof(device_notify));
 
    /* Fill out the record */
    dev->device = dev_ref;
    CFRetain(dev->device);
    strcpy(dev->path, cbuf);
    dev->next = NULL;
    return dev;
}

static device_notify* hid_enum_notify_devices()
{
    device_notify *root = NULL;
    device_notify *curr = NULL;
    
    if(dev_notify_list){
        hid_free_notify_devices(); 
    }
    // Building the list devices available in system
    // create single thread monitoring to check add new devices
    CFSetRef device_set = IOHIDManagerCopyDevices(hid_mgr);
    CFIndex i=0, num_devices = CFSetGetCount(device_set);
    IOHIDDeviceRef *device_array = calloc(num_devices, sizeof(IOHIDDeviceRef));
    CFSetGetValues(device_set, (const void **) device_array);    
    for (i = 0; i < num_devices; i++) {
        IOHIDDeviceRef dev = device_array[i];
        device_notify *tmp;
        char cbuf[MAX_PATH];
        size_t len = 0;
        
        len = make_path(dev, cbuf, sizeof(cbuf));
        
        tmp = calloc(1, sizeof(device_notify));
        if (curr) {
            curr->next = tmp;
        }
        else {
            root = tmp;
        }
        curr = tmp;
        /* Fill out the record */
        curr->device = dev;
        CFRetain(curr->device);
        strcpy(curr->path, cbuf);
        curr->next = NULL;
    }
    free(device_array);
    CFRelease(device_set);
    dev_notify_list = root;
    return root;
}


static int hid_init_register_connect_callbacks()
{
    // register inner callbacks
    if(nil != hid_mgr){ 
        IOHIDManagerRegisterDeviceMatchingCallback(hid_mgr, hid_device_matching_callback, NULL);
        IOHIDManagerRegisterDeviceRemovalCallback(hid_mgr, hid_device_removal_callback, NULL);
        return 0;
    }
    return -1;
}


static int hid_deinit_register_connect_callbacks()
{
    // register inner callbacks
    if(nil != hid_mgr){ 
        IOHIDManagerRegisterDeviceMatchingCallback(hid_mgr, NULL, NULL);
        IOHIDManagerRegisterDeviceRemovalCallback(hid_mgr,  NULL, NULL);
        return 0;
    }
    return -1;
}


static int init_hid_manager(void)
{
    IOReturn res;
    
    /* Initialize all the HID Manager Objects */
    hid_mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if(hid_mgr) {
       IOHIDManagerSetDeviceMatching(hid_mgr, NULL);
       IOHIDManagerScheduleWithRunLoop(hid_mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
       return 0;
    }
    return -1;
}

int HID_API_EXPORT hid_init(void)
{
    if (!hid_mgr) {
       if (init_hid_manager() < 0) {
           hid_exit();
           return -1;
       }
       hid_enum_notify_devices(); 
       hid_init_register_connect_callbacks();    
    }
    return 0;
}


int HID_API_EXPORT hid_exit(void)
{
     if (hid_mgr) {
        
        hid_deinit_register_connect_callbacks();
        hid_free_notify_devices();
          hid_remove_all_notification_callbacks();
  
       /* Close the HID manager. */
        IOHIDManagerClose(hid_mgr, kIOHIDOptionsTypeNone);
        CFRelease(hid_mgr);
        hid_mgr = NULL;
    }
        
    return 0;
}

static void process_pending_events() {
    SInt32 res;
    do {
        res = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, FALSE);
    } while(res != kCFRunLoopRunFinished && res != kCFRunLoopRunTimedOut);
}

struct hid_device_info  HID_API_EXPORT *hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
    struct hid_device_info *root = NULL; // return object
    struct hid_device_info *cur_dev = NULL;
    CFIndex num_devices;
    int i;
    
    setlocale(LC_ALL,"");

    /* Set up the HID Manager if it hasn't been done */
    if(hid_init() < 0)
       return NULL;
    
    /* give the IOHIDManager a chance to update itself */
    process_pending_events();
    
    /* Get a list of the Devices */
    CFSetRef device_set = IOHIDManagerCopyDevices(hid_mgr);
    
    if(NULL == device_set)
        return NULL;
    
    /* Convert the list into a C array so we can iterate easily. */    
    num_devices = CFSetGetCount(device_set);
    IOHIDDeviceRef *device_array = calloc(num_devices, sizeof(IOHIDDeviceRef));
    CFSetGetValues(device_set, (const void **) device_array);
    
    /* Iterate over each device, making an entry for it. */    
    for (i = 0; i < num_devices; i++) {
        unsigned short dev_vid;
        unsigned short dev_pid;
        #define BUF_LEN 256
        wchar_t buf[BUF_LEN];
        char cbuf[BUF_LEN];
        
        IOHIDDeviceRef dev = device_array[i];

        if (!dev) {
           continue;
        }
        dev_vid = get_vendor_id(dev);
        dev_pid = get_product_id(dev);

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

            // Get the Usage Page and Usage for this device.
            cur_dev->usage_page = get_int_property(dev, CFSTR(kIOHIDPrimaryUsagePageKey));
            cur_dev->usage = get_int_property(dev, CFSTR(kIOHIDPrimaryUsageKey));

            /* Fill out the record */
            cur_dev->next = NULL;
            len = make_path(dev, cbuf, sizeof(cbuf));
            cur_dev->path = strdup(cbuf);
            /* Serial Number */
            get_serial_number(dev, buf, BUF_LEN);
            cur_dev->serial_number = dup_wcs(buf);

            /* Manufacturer and Product strings */
            get_manufacturer_string(dev, buf, BUF_LEN);
            cur_dev->manufacturer_string = dup_wcs(buf);
            get_product_string(dev, buf, BUF_LEN);
            cur_dev->product_string = dup_wcs(buf);
            
            /* VID/PID */
            cur_dev->vendor_id = dev_vid;
            cur_dev->product_id = dev_pid;

            /* Release Number */
            cur_dev->release_number = get_int_property(dev, CFSTR(kIOHIDVersionNumberKey));

            /* Interface Number (Unsupported on Mac)*/
            cur_dev->interface_number = -1;
       }
    }
    
    free(device_array);
    CFRelease(device_set);
    return root;
}

void  HID_API_EXPORT hid_free_enumeration(struct hid_device_info *devs)
{
    /* This function is identical to the Linux version. Platform independent. */
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

hid_device * HID_API_EXPORT hid_open(unsigned short vendor_id, unsigned short product_id, wchar_t *serial_number)
{
    /* This function is identical to the Linux version. Platform independent. */
    struct hid_device_info *devs, *cur_dev;
    const char *path_to_open = NULL;
    hid_device * handle = NULL;
    
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


#pragma mark device connection callback

static int hid_check_removal_register()
{
    int isRemove = TRUE;
    if(isRemove){
        pthread_mutex_lock(&connect_callback_mutex);
        if(NULL != connect_callback_list)
            isRemove = FALSE;
        pthread_mutex_unlock(&connect_callback_mutex);
    }
    if(isRemove)
    {
        // check if open devices
        pthread_mutex_lock(&device_list_mutex);
        if(NULL != device_list){
            hid_device *d = device_list; 
            while (d) {
                if (d->disconnected) {
                    isRemove = FALSE;
                    break;
                }
                d = d->next;
            }
        }
        pthread_mutex_unlock(&device_list_mutex);
    }
    return isRemove;    
}

static void hid_device_info_free(struct hid_device_info *dev_info)
{
    if(dev_info){
        free(dev_info->path);
        free(dev_info->serial_number);
        free(dev_info->manufacturer_string);
        free(dev_info->product_string);
        free(dev_info);
    }
}

static struct hid_device_info* hid_device_info_create(IOHIDDeviceRef dev_ref)
{
#define BUF_LEN 256
    int len;
    wchar_t buf[BUF_LEN];
    char cbuf[BUF_LEN];
    struct hid_device_info *dev_info = NULL;
    
    if( !dev_ref){
        return NULL;
    }
    
    setlocale(LC_ALL,"");
    
    dev_info = malloc(sizeof(struct hid_device_info));
    
    // Get the Usage Page and Usage for this device.
    dev_info->usage_page = get_int_property(dev_ref, CFSTR(kIOHIDPrimaryUsagePageKey));
    dev_info->usage = get_int_property(dev_ref, CFSTR(kIOHIDPrimaryUsageKey));
    
    /* Fill out the record */
    dev_info->next = NULL;
    len = make_path(dev_ref, cbuf, sizeof(cbuf));
    dev_info->path = strdup(cbuf);
    
    /* Serial Number */
    get_serial_number(dev_ref, buf, BUF_LEN);
    dev_info->serial_number = dup_wcs(buf);
    
    /* Manufacturer and Product strings */
    get_manufacturer_string(dev_ref, buf, BUF_LEN);
    dev_info->manufacturer_string = dup_wcs(buf);
    get_product_string(dev_ref, buf, BUF_LEN);
    dev_info->product_string = dup_wcs(buf);
    
    /* VID/PID */
    dev_info->vendor_id  = get_vendor_id(dev_ref);
    dev_info->product_id = get_product_id(dev_ref);
    
    /* Release Number */
    dev_info->release_number = get_int_property(dev_ref, CFSTR(kIOHIDVersionNumberKey));
    
    /* Interface Number (Unsupported on Mac)*/
    dev_info->interface_number = -1;
    
    return dev_info;
    
}

static void  hid_device_callback_connect_free(hid_device_callback_connect *dev_connect)
{
    if(dev_connect){
        free(dev_connect);
    }
}

static void CFSetApplierFunctionCopyToCFArray(const void * value, void * context) {
    CFArrayAppendValue( (CFMutableArrayRef) context, value );
}  

static void hid_rebuild_devices(void) {
 // get the set of devices from the IOHID manager
  CFSetRef devCFSetRef = IOHIDManagerCopyDevices(hid_mgr);
  static CFMutableArrayRef aDeviceCFArrayRef = NULL;
  if (devCFSetRef) {
      if (aDeviceCFArrayRef) {
          CFRelease(aDeviceCFArrayRef);
      }
    aDeviceCFArrayRef = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    CFSetApplyFunction(devCFSetRef, CFSetApplierFunctionCopyToCFArray, (void *) aDeviceCFArrayRef);
    CFRelease(devCFSetRef);
  }
}  


static void hid_device_removal_callback(void *context, IOReturn result,
                                        void *sender, IOHIDDeviceRef dev_ref)
{
    hid_device_callback_connect *c = NULL;
    device_notify *dev = NULL;
    
   /* Stop the Run Loop for this device. */
    hid_device *d = nil;
    pthread_mutex_lock(&device_list_mutex);
    d = device_list;
    while (d) {
     if (d->device_handle == dev_ref) {
         d->disconnected = 1;
         CFRunLoopStop(d->run_loop);
     }
        
     d = d->next;
    }
    pthread_mutex_unlock(&device_list_mutex);
    
    dev = hid_find_notify_device(dev_ref);
    
    if(NULL == dev){
        return;
    }
    
    /* Call back for this device */
  
    if(nil!=connect_device_info){
        hid_device_info_free(connect_device_info);
    }
    connect_device_info = hid_device_info_create(dev_ref);
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    c = connect_callback_list;
    while (c) {
        if (c->callback) {
            (*c->callback)(connect_device_info, device_removal, c->context);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&connect_callback_mutex);
    
    hid_remove_notify(dev);
}

static void hid_device_matching_callback(void *context, IOReturn result,
                                void *sender, IOHIDDeviceRef dev_ref)
{
    hid_device_callback_connect *c = NULL;
    
    if(start_dev_list){
      if(hid_find_notify_device(dev_ref)){
          return;
      }
      else
      {
        start_dev_list = false;
      }
    }
    if(!start_dev_list)
    {
        device_notify *dev = hid_find_notify_device(dev_ref);
        if(NULL == dev){
            hid_insert_notify(hid_new_notify(dev_ref));
        }
    }
    if(nil!=connect_device_info){
        hid_device_info_free(connect_device_info);
    }
    connect_device_info = hid_device_info_create(dev_ref);
    
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

#pragma mark device connection callback

/* The Run Loop calls this function for each input report received.
   This function puts the data into a linked list to be picked up by
   hid_read(). */
static void hid_report_callback(void *context, IOReturn result, void *sender,
                         IOHIDReportType report_type, uint32_t report_id,
                         uint8_t *report, CFIndex report_length)
{
    struct input_report *rpt;
    hid_device *dev = context;
    
    // appears that on OSX 10.7, the context isn't reliable, so use the IOHIDDeviceRef (sender)
    // to find it in the list
    pthread_mutex_lock(&device_list_mutex);
    dev = device_list;
    while (dev && dev->device_handle != sender)
    {
        dev = dev->next;
    }
    pthread_mutex_unlock(&device_list_mutex);
    // if we don't have a device, something weird happened, lets ignore
    if (dev == NULL) return;

    /* Make a new Input Report object */
    rpt = calloc(1, sizeof(struct input_report));
    rpt->data = calloc(1, report_length);
    memcpy(rpt->data, report, report_length);
    rpt->len = report_length;
    rpt->next = NULL;

    /* Lock this section */
    pthread_mutex_lock(&dev->mutex);
    
    /* Attach the new report object to the end of the list. */
    if (dev->input_reports == NULL) {
    /* The list is empty. Put it at the root. */
    dev->input_reports = rpt;
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

       /*  Pop one off if we've reached 30 in the queue. This
           way we don't grow forever if the user never reads
           anything from the device. */
       if (num_queued > 30) {
          return_data(dev, NULL, 0);
       }
    }

    /* Signal a waiting thread that there is data. */
    pthread_cond_signal(&dev->condition);

    /* Unlock */
    pthread_mutex_unlock(&dev->mutex);

}

/* This gets called when the read_thred's run loop gets signaled by
   hid_close(), and serves to stop the read_thread's run loop. */
static void perform_signal_callback(void *context)
{
    hid_device *dev = context;
    CFRunLoopStop(dev->run_loop); //TODO: CFRunLoopGetCurrent()
}

static void *read_thread(void *param)
{
    hid_device *dev = param;
    
    /* Move the device's run loop to this thread. */
    IOHIDDeviceScheduleWithRunLoop(dev->device_handle, CFRunLoopGetCurrent(), dev->run_loop_mode);

    /* Create the RunLoopSource which is used to signal the
      event loop to stop when hid_close() is called. */
    CFRunLoopSourceContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.version = 0;
    ctx.info = dev;
    ctx.perform = &perform_signal_callback;
    dev->source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0/*order*/, &ctx);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), dev->source, dev->run_loop_mode);
    
    /* Store off the Run Loop so it can be stopped from hid_close()
    and on device disconnection. */
    dev->run_loop = CFRunLoopGetCurrent();

    /* Notify the main thread that the read thread is up and running. */
    pthread_barrier_wait(&dev->barrier);

    /* Run the Event Loop. CFRunLoopRunInMode() will dispatch HID input
       reports into the hid_report_callback(). */
    SInt32 code;
    while (!dev->shutdown_thread && !dev->disconnected) {
        code = CFRunLoopRunInMode(dev->run_loop_mode, 1000/*sec*/, FALSE);
      /* Return if the device has been disconnected */
        if (code == kCFRunLoopRunFinished) {
            dev->disconnected = 1;
            break;
        }


      /* Break if The Run Loop returns Finished or Stopped. */
       if (code != kCFRunLoopRunTimedOut &&
           code != kCFRunLoopRunHandledSource) {
      /* There was some kind of error. Setting
         shutdown seems to make sense, but
         there may be something else more appropriate */
         dev->shutdown_thread = 1;
         break;
       }
    }

    /* Now that the read thread is stopping, Wake any threads which are
       waiting on data (in hid_read_timeout()). Do this under a mutex to
       make sure that a thread which is about to go to sleep waiting on
       the condition acutally will go to sleep before the condition is
       signaled. */
    pthread_mutex_lock(&dev->mutex);
    pthread_cond_broadcast(&dev->condition);
    pthread_mutex_unlock(&dev->mutex);

    
    /* Wait here until hid_close() is called and makes it past
       the call to CFRunLoopWakeUp(). This thread still needs to
       be valid when that function is called on the other thread. */
    pthread_barrier_wait(&dev->shutdown_barrier);

    return NULL;
}
    
hid_device * HID_API_EXPORT hid_open_path(const char *path)
{
    int i;
    hid_device *dev = NULL;
    CFIndex num_devices;
    
    /* Set up the HID Manager if it hasn't been done */
    if(hid_init() < 0)
        return NULL;
    
    /* give the IOHIDManager a chance to update itself */
    process_pending_events();
    
    dev = get_hid_device_path(path);
    if(dev)
        return dev;
    
    dev = new_hid_device();

    CFSetRef device_set = IOHIDManagerCopyDevices(hid_mgr);
    
    num_devices = CFSetGetCount(device_set);
    IOHIDDeviceRef *device_array = calloc(num_devices, sizeof(IOHIDDeviceRef));
    CFSetGetValues(device_set, (const void **) device_array);    
    for (i = 0; i < num_devices; i++) {
        char cbuf[BUF_LEN];
        size_t len;
        IOHIDDeviceRef os_dev = device_array[i];
        
        len = make_path(os_dev, cbuf, sizeof(cbuf));
        if (!strcmp(cbuf, path)) {
            // Matched Paths. Open this Device.
            IOReturn ret = IOHIDDeviceOpen(os_dev, kIOHIDOptionsTypeNone);
            if (ret == kIOReturnSuccess) {
                char str[32];

                free(device_array);
                CFRelease(device_set);
                dev->device_handle = os_dev;
                
                /* Create the buffers for receiving data */
                dev->max_input_report_len = (CFIndex) get_max_report_length(os_dev);
                dev->input_report_buf = calloc(dev->max_input_report_len, sizeof(uint8_t));
                
                /* Create the Run Loop Mode for this device.
                   printing the reference seems to work. */
                sprintf(str, "HIDAPI_%p", os_dev);
                dev->run_loop_mode = 
                    CFStringCreateWithCString(NULL, str, kCFStringEncodingASCII);
                
                /* Attach the device to a Run Loop */
                IOHIDDeviceRegisterInputReportCallback(
                    os_dev, dev->input_report_buf, dev->max_input_report_len,
                    &hid_report_callback, dev);
                
                IOHIDManagerRegisterDeviceRemovalCallback(hid_mgr, hid_device_removal_callback, NULL);
                
                /* Start the read thread */
                pthread_create(&dev->thread, NULL, read_thread, dev);

                /* Wait here for the read thread to be initialized. */
                pthread_barrier_wait(&dev->barrier);
                
                return dev;
            }
            else {
                goto return_error;
            }
        }
    }

return_error:
    free(device_array);
    CFRelease(device_set);
    free_hid_device(dev);
    return NULL;
}

static int set_report(hid_device *dev, IOHIDReportType type, const unsigned char *data, size_t length)
{
    const unsigned char *data_to_send;
    size_t length_to_send;
    IOReturn res;

    /* Return if the device has been disconnected. */
       if (dev->disconnected)
           return -1;

    if (data[0] == 0x0) {
        /* Not using numbered Reports.
           Don't send the report number. */
        data_to_send = data+1;
        length_to_send = length-1;
    }
    else {
        /* Using numbered Reports.
           Send the Report Number */
        data_to_send = data;
        length_to_send = length;
    }
    
    if (!dev->disconnected) {
        res = IOHIDDeviceSetReport(dev->device_handle,
                       type,
                       data[0], /* Report ID*/
                       data_to_send, length_to_send);
    
        if (res == kIOReturnSuccess) {
            return (int)length;
        }
        else
            return -1;
    }
    
    return -1;
}


static void output_callback(void *context, IOReturn ret, void *sender,
                     IOHIDReportType type, uint32_t id, uint8_t *data, CFIndex len)
{
#ifdef HID_DEBUG    
    printf("output_callback, r=%d\n", ret);
#endif
    if (ret == kIOReturnSuccess) {
        *(int *)context = (int)len;
    } else {
        //  no timeout  
        *(int *)context = 0;
    }
}

static int set_report_timeout(hid_device *dev, IOHIDReportType type, const unsigned char *data, size_t length, int milliseconds)
{
    const unsigned char *data_to_send;
    size_t length_to_send;
    IOReturn res;
    
    /* Return if the device has been disconnected. */
    if (dev->disconnected)
        return -1;
    
    if (data[0] == 0x0) {
        /* Not using numbered Reports.
         Don't send the report number. */
        data_to_send = data+1;
        length_to_send = length-1;
    }
    else {
        /* Using numbered Reports.
         Send the Report Number */
        data_to_send = data;
        length_to_send = length;
    }
        
    if (!dev->disconnected) {
        IOHIDDeviceSetReportWithCallback(dev->device_handle, 
                                         type,
                                         data[0],
                                         data_to_send, length_to_send, 
                                         (double)milliseconds / 1000.0, 
                                         output_callback, &res);
        return res;
    }
    
    return -1;
}

int HID_API_EXPORT hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
    return set_report(dev, kIOHIDReportTypeOutput, data, length);
}

int HID_API_EXPORT hid_write_timeout(hid_device *dev, const unsigned char *data, size_t length, int milliseconds)
{
    return set_report_timeout(dev, kIOHIDReportTypeOutput, data, length, milliseconds);
    
}

/* Helper function, so that this isn't duplicated in hid_read(). */
static int return_data(hid_device *dev, unsigned char *data, size_t length)
{
    /* Copy the data out of the linked list item (rpt) into the
       return buffer (data), and delete the liked list item. */
    struct input_report *rpt = dev->input_reports;
    size_t len = (length < rpt->len)? length: rpt->len;
    memcpy(data, rpt->data, len);
    dev->input_reports = rpt->next;
    free(rpt->data);
    free(rpt);
    return (int)len;
}

static int cond_wait(const hid_device *dev, pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    while (!dev->input_reports) {
        int res = pthread_cond_wait(cond, mutex);
        if (res != 0)
            return res;

        /* A res of 0 means we may have been signaled or it may
           be a spurious wakeup. Check to see that there's acutally
           data in the queue before returning, and if not, go back
           to sleep. See the pthread_cond_timedwait() man page for
           details. */
        
        if (dev->shutdown_thread || dev->disconnected)
            return -1;
    }
    
    return 0;
}

static int cond_timedwait(const hid_device *dev, pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
    while (!dev->input_reports) {
        int res = pthread_cond_timedwait(cond, mutex, abstime);
        if (res != 0)
            return res;

        /* A res of 0 means we may have been signaled or it may
           be a spurious wakeup. Check to see that there's acutally
           data in the queue before returning, and if not, go back
           to sleep. See the pthread_cond_timedwait() man page for
           details. */
        
        if (dev->shutdown_thread || dev->disconnected)
            return -1;
    }
    
    return 0;

}

int HID_API_EXPORT hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
    int bytes_read = -1;

    /* Lock the access to the report list. */
    pthread_mutex_lock(&dev->mutex);
    
    /* There's an input report queued up. Return it. */
    if (dev->input_reports) {
        /* Return the first one */
        bytes_read = return_data(dev, data, length);
        goto ret;
    }

    /* Return if the device has been disconnected. */
    if (dev->disconnected) {
        bytes_read = -1;
        goto ret;
    }
    
    if (dev->shutdown_thread) {
        /* This means the device has been closed (or there
           has been an error. An error code of -1 should
           be returned. */
        bytes_read = -1;
        goto ret;
    }

    /* There is no data. Go to sleep and wait for data. */
    
    if (milliseconds == -1) {
        /* Blocking */
        int res;
        res = cond_wait(dev, &dev->condition, &dev->mutex);
        if (res == 0)
            bytes_read = return_data(dev, data, length);
        else {
            /* There was an error, or a device disconnection. */
            bytes_read = -1;
        }
    }
    else if (milliseconds > 0) {
        /* Non-blocking, but called with timeout. */
        int res;
        struct timespec ts;
        struct timeval tv;
        gettimeofday(&tv, NULL);
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
        /* Purely non-blocking */
        bytes_read = 0;
    }

ret:
    /* Unlock */
    pthread_mutex_unlock(&dev->mutex);
    return bytes_read;
}

int HID_API_EXPORT hid_read(hid_device *dev, unsigned char *data, size_t length)
{
    return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

int HID_API_EXPORT hid_set_nonblocking(hid_device *dev, int nonblock)
{
    /* All Nonblocking operation is handled by the library. */
    dev->blocking = !nonblock;
    
    return 0;
}

int HID_API_EXPORT hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
    return set_report(dev, kIOHIDReportTypeFeature, data, length);
}

int HID_API_EXPORT hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
    CFIndex len = length;
    IOReturn res;

    /* Return if the device has been unplugged. */
    if (dev->disconnected)
        return -1;

    res = IOHIDDeviceGetReport(dev->device_handle,
                               kIOHIDReportTypeFeature,
                               data[0], /* Report ID */
                               data, &len);
    if (res == kIOReturnSuccess)
        return (int)len;
    else
        return -1;
}


void HID_API_EXPORT hid_close(hid_device *dev)
{
    if (!dev)
        return;
    
    if(!exist_hid_device(dev))
        return;
    
    if(!dev->ref_count)
        return;
    
    if(dev->ref_count > 1){
      dev->ref_count--;
      return;
    }
    
    dev->ref_count = 0; 
    
    /* Disconnect the report callback before close. */
    if (!dev->disconnected) {
        IOHIDDeviceRegisterInputReportCallback(
            dev->device_handle, dev->input_report_buf, dev->max_input_report_len,
            NULL, dev);
        if( hid_check_removal_register() ){
            IOHIDManagerRegisterDeviceRemovalCallback(hid_mgr, NULL, dev);
        }
        IOHIDDeviceUnscheduleFromRunLoop(dev->device_handle, dev->run_loop, dev->run_loop_mode);
        IOHIDDeviceScheduleWithRunLoop(dev->device_handle, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    }
    
    /* Cause read_thread() to stop. */
    dev->shutdown_thread = 1;
    
    /* Wake up the run thread's event loop so that the thread can exit. */
    CFRunLoopSourceSignal(dev->source);
    CFRunLoopWakeUp(dev->run_loop);
    
    /* Notify the read thread that it can shut down now. */
    pthread_barrier_wait(&dev->shutdown_barrier);

    /* Wait for read_thread() to end. */
    pthread_join(dev->thread, NULL);

    /* Close the OS handle to the device, but only if it's not
       been unplugged. If it's been unplugged, then calling
       IOHIDDeviceClose() will crash. */
    if (!dev->disconnected) {
        IOHIDDeviceClose(dev->device_handle, kIOHIDOptionsTypeNone);
    }
    
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
    return get_manufacturer_string(dev->device_handle, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    return get_product_string(dev->device_handle, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    return get_serial_number(dev->device_handle, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
    // TODO:

    return 0;
}


HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
    // TODO:

    return NULL;
}

#pragma  mark device register connection callbacks


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

    
static int hid_register_add_callback(hid_device_callback callBack, hid_device_context context)
{
    int result = -1;
    hid_device_callback_connect *hid_connect = nil;
    
    if(nil == callBack)
        return result;
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    // check if has been registered callback
    if(hid_connect_registered(callBack,context)==0){
        pthread_mutex_unlock(&connect_callback_mutex);
        return 0;
    }
    
    if(!connect_callback_list){
        hid_init_register_connect_callbacks();
    }
       
    hid_connect = calloc(1, sizeof(hid_device_callback_connect));
    hid_connect->callback = callBack;
    hid_connect->context = context;
    hid_connect->next = nil;
   
    // add callback in the list
    if (!connect_callback_list)
          connect_callback_list = hid_connect;
    else {
          hid_device_callback_connect *c = connect_callback_list;
            hid_device_callback_connect *d = c; 
            while (c->next) {
                c = c->next;
          }
          c->next = hid_connect;
            connect_callback_list = d;
    }
    pthread_mutex_unlock(&connect_callback_mutex);
    return 0;
}

static void hid_register_remove_callback(hid_device_callback callBack, hid_device_context context)
{
    hid_device_callback_connect *c = nil;
    hid_device_callback_connect *d = nil;
    
    if(nil == callBack)
        return;
    
     if(nil == connect_callback_list)
          return;
     
     pthread_mutex_lock(&connect_callback_mutex);
    
    c = connect_callback_list;
    
    if(NULL == connect_callback_list){
       pthread_mutex_unlock(&connect_callback_mutex);
        return;
     }
    if (c->callback  == callBack && c->context == context) {
        d = c;
        connect_callback_list = c->next;
    }
    else {
      while (c) {
          if (c->next) {
             if ((c->next->callback) == callBack && c->next->context == context) {
                 d  = c->next; 
                 c->next = c->next->next;
                     break;
             }
          }
         c = c->next;
      }
    }
    if(nil != d){
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
    
    /* Set up the HID Manager if it hasn't been done */
    if(!hid_mgr)
          return result;
  
    /* register device matching callback */
    result = hid_register_add_callback(callBack, context);
    return result;
}

void HID_API_EXPORT HID_API_CALL hid_remove_notification_callback(hid_device_callback  callBack, void *context)
{
    /* register device remove callback */
    hid_register_remove_callback(callBack, context);
}

void HID_API_EXPORT HID_API_CALL hid_remove_all_notification_callbacks(void)
{
    hid_device_callback_connect *c = nil;
    hid_device_callback_connect *p = nil;
    
    pthread_mutex_lock(&connect_callback_mutex);
    
    c = connect_callback_list;

    while (c) {
        p = c;
        c = c->next;
        hid_device_callback_connect_free(p);
     }
    hid_device_info_free(connect_device_info);
    connect_device_info = nil;
    connect_callback_list = nil;
    pthread_mutex_unlock(&connect_callback_mutex);
}

#pragma mark device register connection callback

#if 0
static int32_t get_location_id(IOHIDDeviceRef device)
{
    return get_int_property(device, CFSTR(kIOHIDLocationIDKey));
}


static int32_t get_usage(IOHIDDeviceRef device)
{
    int32_t res;
    res = get_int_property(device, CFSTR(kIOHIDDeviceUsageKey));
    if (!res)
        res = get_int_property(device, CFSTR(kIOHIDPrimaryUsageKey));
    return res;
}

static int32_t get_usage_page(IOHIDDeviceRef device)
{
    int32_t res;
    res = get_int_property(device, CFSTR(kIOHIDDeviceUsagePageKey));
    if (!res)
        res = get_int_property(device, CFSTR(kIOHIDPrimaryUsagePageKey));
    return res;
}

static int get_transport(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
    return get_string_property(device, CFSTR(kIOHIDTransportKey), buf, len);
}


int main(void)
{
    IOHIDManagerRef mgr;
    int i;
    
    mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(mgr, NULL);
    IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    
    CFSetRef device_set = IOHIDManagerCopyDevices(mgr);
    
    CFIndex num_devices = CFSetGetCount(device_set);
    IOHIDDeviceRef *device_array = calloc(num_devices, sizeof(IOHIDDeviceRef));
    CFSetGetValues(device_set, (const void **) device_array);
    
    setlocale(LC_ALL, "");
    
    for (i = 0; i < num_devices; i++) {
        IOHIDDeviceRef dev = device_array[i];
        printf("Device: %p\n", dev);
        printf("  %04hx %04hx\n", get_vendor_id(dev), get_product_id(dev));

        wchar_t serial[256], buf[256];
        char cbuf[256];
        get_serial_number(dev, serial, 256);

        
        printf("  Serial: %ls\n", serial);
        printf("  Loc: %ld\n", get_location_id(dev));
        get_transport(dev, buf, 256);
        printf("  Trans: %ls\n", buf);
        make_path(dev, cbuf, 256);
        printf("  Path: %s\n", cbuf);
        
    }

    return 0;
}
#endif
