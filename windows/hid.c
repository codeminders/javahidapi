/*******************************************************
HIDAPI - Multi-Platform library for
communication with HID devices.

Alan Ott
Signal 11 Software

8/22/2009
    
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
    
#include <windows.h>
                
#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif
    
#ifdef __MINGW32__
#include <ntdef.h>
#include <winbase.h>
#endif
    
#ifdef __CYGWIN__
#include <ntdef.h>
#define _wcsdup wcsdup
#endif
#include "process.h"
    
//#define HIDAPI_USE_DDK
#define HID_DEVICE_SUPPORT_CONNECT  // support connect nonification
#define HID_DEVICE_NOTIFY          // hook notification on device 
//#define HID_DEBUG                 // hid debugger
    
#ifdef __cplusplus
extern "C" {
#endif
#include <setupapi.h>
#include <winioctl.h>
#ifdef HIDAPI_USE_DDK
#include <hidsdi.h>
#endif

// Copied from inc/ddk/hidclass.h, part of the Windows DDK.
#define HID_OUT_CTL_CODE(id)  \
CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_HID_GET_FEATURE                   HID_OUT_CTL_CODE(100)

#ifdef __cplusplus
} // extern "C"
#endif

#include <stdio.h>
#include <stdlib.h>

#ifdef HID_DEVICE_SUPPORT_CONNECT
#include <strsafe.h>
#include <dbt.h>
#endif


#include "hidapi.h"

#ifdef _MSC_VER
// Thanks Microsoft, but I know how to use strncpy().
#pragma warning(disable:4996)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HIDAPI_USE_DDK
// Since we're not building with the DDK, and the HID header
// files aren't part of the SDK, we have to define all this
// stuff here. In lookup_functions(), the function pointers
// defined below are set.
typedef struct _HIDD_ATTRIBUTES{
    ULONG Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

typedef USHORT USAGE;
typedef struct _HIDP_CAPS {
    USAGE Usage;
    USAGE UsagePage;
    USHORT InputReportByteLength;
    USHORT OutputReportByteLength;
    USHORT FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT fields_not_used_by_hidapi[10];
} HIDP_CAPS, *PHIDP_CAPS;
typedef char* HIDP_PREPARSED_DATA;
#define HIDP_STATUS_SUCCESS 0x110000

typedef BOOLEAN (__stdcall *HidD_GetAttributes_)(HANDLE device, PHIDD_ATTRIBUTES attrib);
typedef BOOLEAN (__stdcall *HidD_GetSerialNumberString_)(HANDLE device, PVOID buffer, ULONG buffer_len);
typedef BOOLEAN (__stdcall *HidD_GetManufacturerString_)(HANDLE handle, PVOID buffer, ULONG buffer_len);
typedef BOOLEAN (__stdcall *HidD_GetProductString_)(HANDLE handle, PVOID buffer, ULONG buffer_len);
typedef BOOLEAN (__stdcall *HidD_SetFeature_)(HANDLE handle, PVOID data, ULONG length);
typedef BOOLEAN (__stdcall *HidD_GetFeature_)(HANDLE handle, PVOID data, ULONG length);
typedef BOOLEAN (__stdcall *HidD_GetIndexedString_)(HANDLE handle, ULONG string_index, PVOID buffer, ULONG buffer_len);
typedef BOOLEAN (__stdcall *HidD_GetPreparsedData_)(HANDLE handle, HIDP_PREPARSED_DATA **preparsed_data);
typedef BOOLEAN (__stdcall *HidD_FreePreparsedData_)(HIDP_PREPARSED_DATA *preparsed_data);
typedef BOOLEAN (__stdcall *HidP_GetCaps_)(HIDP_PREPARSED_DATA *preparsed_data, HIDP_CAPS *caps);
typedef void (__stdcall *HidD_GetHidGuid_)(__out  LPGUID HidGuid);
static HidD_GetAttributes_ HidD_GetAttributes;
static HidD_GetSerialNumberString_ HidD_GetSerialNumberString;
static HidD_GetManufacturerString_ HidD_GetManufacturerString;
static HidD_GetProductString_ HidD_GetProductString;
static HidD_SetFeature_ HidD_SetFeature;
static HidD_GetFeature_ HidD_GetFeature;
static HidD_GetIndexedString_ HidD_GetIndexedString;
static HidD_GetPreparsedData_ HidD_GetPreparsedData;
static HidD_FreePreparsedData_ HidD_FreePreparsedData;
static HidP_GetCaps_ HidP_GetCaps;
static HidD_GetHidGuid_ HidD_GetHidGuid; 

static HMODULE lib_handle = NULL;
static BOOLEAN initialized = FALSE;
static volatile BOOL quit = FALSE;
#endif // HIDAPI_USE_DDK

struct hid_device_ {
    HANDLE device_handle;
    BOOL blocking;
    USHORT output_report_length;
    size_t input_report_length;
    void *last_error_str;
    DWORD last_error_num;
    BOOL read_pending;
    BOOL write_pending;
    char *read_buf;
    OVERLAPPED ol_read;
    OVERLAPPED ol_write;
    int ref_count;
    hid_device *next;
};

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_WND_THREAD_MUTEX "HID_THREAD_MUTEX" 
static HANDLE hid_lib_init_mutex = NULL;

#ifdef HID_DEVICE_SUPPORT_CONNECT

#define DEVICE_WND_CLASS_NAME  "HID_DEVICE_WND"

#ifdef HID_DEVICE_NOTIFY
    
typedef
struct HID_DEVICE_NOTIFY_INFO
{
   HANDLE       hDevice; // file handle
   HDEVNOTIFY   hHandleNotification; // notification handle
   CHAR         DevicePath[MAX_PATH];// device path
   struct hid_device_info*  devInfo; // device info
   struct HID_DEVICE_NOTIFY_INFO* next;
}HID_DEVICE_NOTIFY_INFO;

typedef struct HID_DEVICE_NOTIFY_INFO *PHID_DEVICE_NOTIFY_INFO;

#define PLENHID_DEVICE_NOTIFY_INFO   sizeof(struct HID_DEVICE_NOTIFY_INFO)

PHID_DEVICE_NOTIFY_INFO hid_enum_notify_devices(HWND hWnd);
PHID_DEVICE_NOTIFY_INFO hid_find_notify_device(HDEVNOTIFY deev_notify);
void hid_remove_notify(PHID_DEVICE_NOTIFY_INFO);
void hid_add_notify(PHID_DEVICE_NOTIFY_INFO);
void hid_free_notify_devices();

static PHID_DEVICE_NOTIFY_INFO pHidDeviceNotify = NULL;

#endif

    /* Register callbacks implementation*/
    
    typedef void *hid_device_context;
    typedef
    struct hid_device_callback_connect
    {
        hid_device_callback callback;
        hid_device_context context; 
        struct hid_device_callback_connect *next;
    }hid_device_callback_connect;
    
    /* Static list of all the callbacks registered.*/
    
    static struct hid_device_callback_connect *connect_callback_list = NULL;
    static struct hid_device_info *connect_device_info = NULL;
    static HANDLE connect_callback_mutex = NULL;
    
    
    /* Add the opened device list*/
    
    // Window device connection interface
    
    void hid_deinit_connect();
    BOOL hid_init_connect();
    
    BOOL init_device_notification();
    void deinit_device_notification();
    BOOL init_device_thread_window(HWND hWindow);
    void deinit_device_thread_window();
    
    int hid_init_register_connect_callbacks();
    int hid_deinit_register_connect_callbacks();
    
    static HWND  hDeviceWindow = NULL;
    static HANDLE hThreadDeviceWnd = NULL;
    static DWORD dwThreadID = 0;
    static HDEVNOTIFY hDeviceNotify = NULL;
    
#endif
   static hid_device *device_list = NULL;
   static HANDLE device_list_mutex = NULL;
#ifdef __cplusplus
}
#endif


static hid_device *new_hid_device()
{   
    hid_device *d = NULL; 
    hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
    dev->device_handle = INVALID_HANDLE_VALUE;
    dev->blocking = TRUE;
    dev->output_report_length = 0;
    dev->input_report_length = 0;
    dev->last_error_str = NULL;
    dev->last_error_num = 0;
    dev->read_pending = FALSE;
    dev->write_pending = FALSE;
    dev->read_buf = NULL;
    dev->next = NULL;
    dev->ref_count = 1;
    memset(&dev->ol_read, 0, sizeof(dev->ol_read));
    memset(&dev->ol_write, 0, sizeof(dev->ol_write));
    dev->ol_read.hEvent = CreateEvent(NULL, FALSE, FALSE /*inital state f=nonsignaled*/, NULL);
    dev->ol_write.hEvent = CreateEvent(NULL, FALSE, FALSE /*inital state f=nonsignaled*/, NULL);
    
    if(NULL == device_list_mutex)
    {
        device_list_mutex = CreateMutex(NULL,FALSE,NULL);
    }
    WaitForSingleObject(device_list_mutex, INFINITE);
    if(!device_list){
        device_list = dev;
    }
    else{
        d = device_list;
        while (d) {
            if (!d->next) {
                d->next = dev;
                break;
            }
            d = d->next;
        }
    }
    ReleaseMutex(device_list_mutex);
    return dev;
}

static void register_error(hid_device *device, const char *op)
{
    WCHAR *ptr, *msg;
    
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                   FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL,
                   GetLastError(),
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPWSTR)&msg, 0/*sz*/,
                   NULL);
    
    // Get rid of the CR and LF that FormatMessage() sticks at the
    // end of the message. Thanks Microsoft!
    ptr = msg;
    while (*ptr) {
        if (*ptr == '\r') {
            *ptr = 0x0000;
            break;
        }
        ptr++;
    }
    
    // Store the message off in the Device entry so that 
    // the hid_error() function can pick it up.
    LocalFree(device->last_error_str);
    device->last_error_str = msg;
}

#ifndef HIDAPI_USE_DDK
static int lookup_functions()
{
    lib_handle = LoadLibraryA("hid.dll");
    if (lib_handle) {
#define RESOLVE(x) x = (x##_)GetProcAddress(lib_handle, #x); if (!x) return -1;
        RESOLVE(HidD_GetAttributes);
        RESOLVE(HidD_GetSerialNumberString);
        RESOLVE(HidD_GetManufacturerString);
        RESOLVE(HidD_GetProductString);
        RESOLVE(HidD_SetFeature);
        RESOLVE(HidD_GetFeature);
        RESOLVE(HidD_GetIndexedString);
        RESOLVE(HidD_GetPreparsedData);
        RESOLVE(HidD_FreePreparsedData);
        RESOLVE(HidP_GetCaps);
        RESOLVE(HidD_GetHidGuid);
#undef RESOLVE
    }
    else
        return -1;
    
    return 0;
}
#endif

static HANDLE open_device(const char *path, BOOL enumerate)
{
    HANDLE handle;
    DWORD desired_access = (enumerate)? 0: (GENERIC_WRITE | GENERIC_READ);
    DWORD share_mode = (enumerate)?
                        FILE_SHARE_READ|FILE_SHARE_WRITE:
                        FILE_SHARE_READ;
    handle = CreateFileA(path,
                         desired_access,
                         share_mode,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,//FILE_ATTRIBUTE_NORMAL,
                         0);
    return handle;
}

void hid_devices_close()
{
    hid_device *d = NULL;
    hid_device *dev = NULL;
   
    /* Remove all from the device list. */
    WaitForSingleObject(device_list_mutex, INFINITE);
    
    d = device_list;
    
    while ( d ) {
       dev = d;
       d = d->next;
       CancelIo(dev->device_handle);
       CloseHandle(dev->ol_read.hEvent);
       CloseHandle(dev->ol_write.hEvent);
       CloseHandle(dev->device_handle);
       LocalFree(dev->last_error_str);
       free(dev->read_buf);
       free(dev);    
    }
   ReleaseMutex(device_list_mutex);
   CloseHandle(device_list_mutex);
   device_list_mutex = NULL;
#ifdef HID_DEBUG
    printf("Success close devices \n");
#endif
}

//#pragma mark get opened device


struct hid_device_info* hid_device_info_create( HANDLE device_handle, 
                                               const char *device_path)
{
    #define WSTR_LEN 512
    struct hid_device_info *dev;
    HIDP_PREPARSED_DATA *pp_data = NULL;
    HIDD_ATTRIBUTES attrib;
    HIDP_CAPS caps;
    BOOLEAN res;
    NTSTATUS nt_res;
    wchar_t wstr[WSTR_LEN]; // TODO: Determine Size
    const char *str;
    size_t len;
    
    if(INVALID_HANDLE_VALUE == device_handle || NULL == device_handle)
        return NULL;
    
    /* VID/PID match. Create the record. */
    dev = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
    dev->next = NULL;
    // Get the Usage Page and Usage for this device.
    res = HidD_GetPreparsedData(device_handle, &pp_data);
    if (res) {
        nt_res = HidP_GetCaps(pp_data, &caps);
        if (nt_res == HIDP_STATUS_SUCCESS) {
            dev->usage_page = caps.UsagePage;
            dev->usage = caps.Usage;
        }
        HidD_FreePreparsedData(pp_data);
    }
    /* Fill out the record */
    str = device_path;
    if (str) {
        len = strlen(str);
        dev->path = (char*) calloc(len+1, sizeof(char));
        strncpy(dev->path, str, len+1);
        dev->path[len] = '\0';
    }
    else{
        dev->path = NULL;
    }/* Serial Number */
    res = HidD_GetSerialNumberString(device_handle, wstr, sizeof(wstr));
    wstr[WSTR_LEN-1] = 0x0000;
    if (res) {
        dev->serial_number = _wcsdup(wstr);
    }
    /* Manufacturer String */
    res = HidD_GetManufacturerString(device_handle, wstr, sizeof(wstr));
    wstr[WSTR_LEN-1] = 0x0000;
    if (res) {
        dev->manufacturer_string = _wcsdup(wstr);
    }
    /* Product String */
    res = HidD_GetProductString(device_handle, wstr, sizeof(wstr));
    wstr[WSTR_LEN-1] = 0x0000;
    if (res) {
        dev->product_string = _wcsdup(wstr);
    }
    
    // Get the Vendor ID and Product ID for this device.
    attrib.Size = sizeof(HIDD_ATTRIBUTES);
    HidD_GetAttributes(device_handle, &attrib);
    
    /* VID/PID */
    dev->vendor_id = attrib.VendorID;
    dev->product_id = attrib.ProductID;
    
    /* Release Number */
    dev->release_number = attrib.VersionNumber;
    dev->interface_number = -1;
    if (dev->path) {
        char *interface_component = strstr(dev->path, "&mi_");
        if (interface_component) {
            char *hex_str = interface_component + 4;
            char *endptr = NULL;
            dev->interface_number = strtol(hex_str, &endptr, 16);
            if (endptr == hex_str) {
                /* The parsing failed. Set interface_number to -1. */
                dev->interface_number = -1;
            }
        }
    }
    return dev;
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
        if(dev_info)
           free(dev_info);
    }
}

#if defined(HID_DEVICE_NOTIFY) && defined(HID_DEVICE_SUPPORT_CONNECT) 

PHID_DEVICE_NOTIFY_INFO hid_enum_notify_devices(HWND hWnd)
{
    BOOL res;
    struct HID_DEVICE_NOTIFY_INFO *root    = NULL; // return object
    struct HID_DEVICE_NOTIFY_INFO *cur_dev = NULL;

    SP_DEVINFO_DATA devinfo_data;
    SP_DEVICE_INTERFACE_DATA device_interface_data;
    SP_DEVICE_INTERFACE_DETAIL_DATA_A *device_interface_detail_data = NULL;
    HDEVINFO device_info_set = INVALID_HANDLE_VALUE;
    DEV_BROADCAST_HANDLE  filter;

    int device_index = 0;

    // Windows objects for interacting with the driver.
    GUID InterfaceClassGuid = {0x4d1e55b2, 0xf16f, 0x11cf, 
        {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30} };

    hid_free_notify_devices();
       // Windows objects for interacting with the driver.
    // Initialize the Windows objects.
    devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);
    device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    // Get information for all the devices belonging to the HID class.
    device_info_set = SetupDiGetClassDevsA(&InterfaceClassGuid, 
        NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    // Iterate over each device in the HID class, looking for the right one.
    for (;;) {
        HANDLE device_handle = INVALID_HANDLE_VALUE;
        DWORD required_size = 0;
        res = SetupDiEnumDeviceInterfaces(device_info_set,
            NULL,
            &InterfaceClassGuid,
            device_index,
            &device_interface_data);
        if (!res) {
            // A return of FALSE from this function means that
            // there are no more devices.
            break;
        }
        // Call with 0-sized detail size, and let the function
        // tell us how long the detail struct needs to be. The
        // size is put in &required_size.
        res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
            &device_interface_data,
            NULL,
            0,
            &required_size,
            NULL);
        // Allocate a long enough structure for device_interface_detail_data.
        device_interface_detail_data = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*) malloc(required_size);
        device_interface_detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        // Get the detailed data for this device. The detail data gives us
        // the device path for this device, which is then passed into
        // CreateFile() to get a handle to the device.
        res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
            &device_interface_data,
            device_interface_detail_data,
            required_size,
            NULL,
            NULL);
        if (!res) {
            // register_error(dev, "Unable to call SetupDiGetDeviceInterfaceDetail");
            // Continue to the next device.
            goto cont;
        }
        //printf("HandleName: %s\n", device_interface_detail_data->DevicePath);
        
        device_handle = open_device(device_interface_detail_data->DevicePath, TRUE);
        
        if (device_handle == INVALID_HANDLE_VALUE) {
            // Unable to open the device.
            goto cont; //continue
        }
        else 
        {
           PHID_DEVICE_NOTIFY_INFO tmp;
           tmp = (PHID_DEVICE_NOTIFY_INFO)calloc(1, PLENHID_DEVICE_NOTIFY_INFO);
           if (cur_dev) {
                cur_dev->next = tmp;
           }
           else {
                root = tmp;
           }
            cur_dev = tmp;
          cur_dev->hDevice = device_handle;
          
          StringCchCopyA(cur_dev->DevicePath, MAX_PATH, 
             device_interface_detail_data->DevicePath);
                 
          memset (&filter, 0, sizeof(filter)); //zero the structure
          filter.dbch_size = sizeof(filter);
          filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
          filter.dbch_handle = cur_dev->hDevice;
          cur_dev->hHandleNotification = RegisterDeviceNotification(hWnd, &filter, 0);
          cur_dev->devInfo = hid_device_info_create(cur_dev->hDevice,cur_dev->DevicePath);
        }    
  cont:
        // We no longer need the detail data. It can be freed
        free(device_interface_detail_data);
        device_index++;
    }
    // Close the device information handle.
    SetupDiDestroyDeviceInfoList(device_info_set);
    
    pHidDeviceNotify = root;
    
    return root;
}


PHID_DEVICE_NOTIFY_INFO hid_find_notify_device(HDEVNOTIFY hDevNotify)
{
    PHID_DEVICE_NOTIFY_INFO c = pHidDeviceNotify;
    PHID_DEVICE_NOTIFY_INFO d = NULL;
    while( c ){
      if (c->hHandleNotification == hDevNotify) {
          d  = c; 
         break;
     }
      c = c->next;
    }
    return d;
}

PHID_DEVICE_NOTIFY_INFO hid_find_notify_device_path(char *path)
{
    PHID_DEVICE_NOTIFY_INFO c = pHidDeviceNotify;
    PHID_DEVICE_NOTIFY_INFO d = NULL;
    if(NULL == path)
        return NULL;
    while( c ){
      if (strcmp(c->DevicePath,path) == 0) {
          d  = c; 
         break;
     }
      c = c->next;
    }
    return d;
}

void hid_remove_notify(PHID_DEVICE_NOTIFY_INFO dev)
{
    PHID_DEVICE_NOTIFY_INFO c = pHidDeviceNotify;
    PHID_DEVICE_NOTIFY_INFO d = NULL;
    if(c == dev){
       d  = dev;
       pHidDeviceNotify = c->next;
    }
    else
    {
      while( c ){
        if (c->next) {
               if (c->next == dev) {
                  d  = c->next; 
                   c->next = c->next->next;
                  break;
              }
        }
        c = c->next;
      }
    }
    if(d){
      free(d);
    }
}

void hid_insert_notify(PHID_DEVICE_NOTIFY_INFO dev)
{
    PHID_DEVICE_NOTIFY_INFO root = pHidDeviceNotify;
    if(dev)
    {
        if(root){
            dev->next = root;
        }
        pHidDeviceNotify = dev;
    }
}

void hid_free_notify_devices()
{
    PHID_DEVICE_NOTIFY_INFO cur = pHidDeviceNotify;
    PHID_DEVICE_NOTIFY_INFO next = cur;
    while (NULL != cur) {
        next = cur->next;
        if (cur->hHandleNotification) {
            UnregisterDeviceNotification(cur->hHandleNotification);
            cur->hHandleNotification = NULL;
        }
        if ( cur->hDevice != INVALID_HANDLE_VALUE &&
             cur->hDevice != NULL) {
            CloseHandle(cur->hDevice);
            cur->hDevice = INVALID_HANDLE_VALUE;
        }
        hid_device_info_free(cur->devInfo);
        cur->devInfo = NULL;
        free(cur);
        cur = next;
    }
    pHidDeviceNotify = NULL;
}

PHID_DEVICE_NOTIFY_INFO hid_device_register(PDEV_BROADCAST_DEVICEINTERFACE_A b)
{
   PHID_DEVICE_NOTIFY_INFO pDevNotify = NULL;
   DEV_BROADCAST_HANDLE filter;
   const char *path_to_open = NULL;
   size_t len = 0;
   HANDLE hDevice = NULL;

   path_to_open = b->dbcc_name;
   hDevice = open_device( path_to_open, TRUE);
   if(hDevice){
       if(hid_find_notify_device_path((char*)path_to_open)){
          // has been register
          return NULL;
       }
   }
   memset (&filter, 0, sizeof(filter)); //zero the structure
   
   if(INVALID_HANDLE_VALUE != hDevice && hDevice )
   {
     pDevNotify = (PHID_DEVICE_NOTIFY_INFO)calloc(1, PLENHID_DEVICE_NOTIFY_INFO);
    
     filter.dbch_size = sizeof(filter);
     filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
     filter.dbch_handle = hDevice;
     pDevNotify->hHandleNotification = RegisterDeviceNotification(hDeviceWindow, &filter, 0);
     pDevNotify->hDevice = hDevice;
     
     len = strlen( path_to_open );
     strncpy(pDevNotify->DevicePath, path_to_open, len+1);
     pDevNotify->DevicePath[len] = '\0';
     pDevNotify->devInfo = hid_device_info_create(pDevNotify->hDevice, pDevNotify->DevicePath);
     
     pDevNotify->next = NULL;
     
     hid_insert_notify(pDevNotify);
   }
   return pDevNotify;
}
#endif

static hid_device* get_hid_device_path(const char *path)
{
#define BUF_LEN  256
    HANDLE handle = 0;
    hid_device* dev = NULL;
    HIDD_ATTRIBUTES attrib;
    
    unsigned short vendor_id = 0;
    unsigned short product_id = 0;
    
    if(NULL == device_list_mutex)
    {
        device_list_mutex = CreateMutex(NULL,FALSE,NULL);
    }
    handle = open_device(path, FALSE);
    
    if(INVALID_HANDLE_VALUE == handle)
        return NULL;        
    else{
        HidD_GetAttributes(handle, &attrib);
        vendor_id  = attrib.VendorID;
        product_id = attrib.ProductID; 
        CloseHandle(handle);
    }
    WaitForSingleObject(device_list_mutex, INFINITE);
    
    if (!device_list){
        ReleaseMutex(device_list_mutex);
           return NULL;
    }
    else {
        hid_device *d = device_list;
        while (d) {
            HidD_GetAttributes(d->device_handle, &attrib);
            if (attrib.VendorID == vendor_id && attrib.ProductID == product_id) {
                d->ref_count++;
                dev = d;
                break;
            }
            d = d->next;
        }
    }
    ReleaseMutex(device_list_mutex);
    return dev;
}

int HID_API_EXPORT hid_init(void)
{
#ifndef HIDAPI_USE_DDK
    if (!initialized) {
        if (lookup_functions() < 0) {
            hid_exit();
            return -1;
        }
#ifdef HID_DEVICE_SUPPORT_CONNECT
        hid_init_connect();
#endif
        initialized = TRUE;
    }
#endif
    return 0;
}

int HID_API_EXPORT hid_exit(void)
{
#ifdef HID_DEVICE_SUPPORT_CONNECT
    hid_remove_all_notification_callbacks();
    hid_deinit_connect();
#endif
    hid_devices_close();
#ifndef HIDAPI_USE_DDK
    if (lib_handle)
        FreeLibrary(lib_handle);
    lib_handle = NULL;
    initialized = FALSE;
#endif
#ifdef HID_DEBUG
    printf("Free library!\n");
#endif
    return 0;
}

#ifdef HID_DEVICE_SUPPORT_CONNECT

void hid_deinit_connect()
{
    hid_deinit_register_connect_callbacks();
    deinit_device_thread_window();
}

BOOL hid_init_connect()
{
    BOOL bResult = TRUE;
    bResult = init_device_thread_window(NULL);
    if(!bResult) 
        return FALSE;
    if(NULL==connect_callback_list){
        int result = hid_init_register_connect_callbacks();
        if(result == -1){
            return FALSE;
        }
    }
    return bResult;
}
#endif

struct hid_device_info HID_API_EXPORT * HID_API_CALL hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
    BOOL res;
    struct hid_device_info *root = NULL; // return object
    struct hid_device_info *cur_dev = NULL;
    // Windows objects for interacting with the driver.
    GUID InterfaceClassGuid = {0x4d1e55b2, 0xf16f, 0x11cf, 
        {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30} };
    
    SP_DEVINFO_DATA devinfo_data;
    SP_DEVICE_INTERFACE_DATA device_interface_data;
    SP_DEVICE_INTERFACE_DETAIL_DATA_A *device_interface_detail_data = NULL;
    HDEVINFO device_info_set = INVALID_HANDLE_VALUE;
    int device_index = 0;
    int i;
    
    if (hid_init() < 0)
        return NULL;
    
    // Initialize the Windows objects.
    memset(&devinfo_data, 0x0, sizeof(devinfo_data));
    devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);
    device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    
    // Get information for all the devices belonging to the HID class.
    device_info_set = SetupDiGetClassDevsA(&InterfaceClassGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    
    // Iterate over each device in the HID class, looking for the right one.
    
    for (;;) {
        HANDLE write_handle = INVALID_HANDLE_VALUE;
        DWORD required_size = 0;
        HIDD_ATTRIBUTES attrib;
        
        res = SetupDiEnumDeviceInterfaces(device_info_set,
                                          NULL,
                                          &InterfaceClassGuid,
                                          device_index,
                                          &device_interface_data);
        
        if (!res) {
            // A return of FALSE from this function means that
            // there are no more devices.
            break;
        }
        
        // Call with 0-sized detail size, and let the function
        // tell us how long the detail struct needs to be. The
        // size is put in &required_size.
        res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
                                               &device_interface_data,
                                               NULL,
                                               0,
                                               &required_size,
                                               NULL);
        
        // Allocate a long enough structure for device_interface_detail_data.
        device_interface_detail_data = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*) malloc(required_size);
        device_interface_detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        
        // Get the detailed data for this device. The detail data gives us
        // the device path for this device, which is then passed into
        // CreateFile() to get a handle to the device.
        res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
                                               &device_interface_data,
                                               device_interface_detail_data,
                                               required_size,
                                               NULL,
                                               NULL);
        
        if (!res) {
            //register_error(dev, "Unable to call SetupDiGetDeviceInterfaceDetail");
            // Continue to the next device.
            goto cont;
        }
        
		// Make sure this device is of Setup Class "HIDClass" and has a
		// driver bound to it.
		for (i = 0; ; i++) {
			char driver_name[256];

			// Populate devinfo_data. This function will return failure
			// when there are no more interfaces left.
			res = SetupDiEnumDeviceInfo(device_info_set, i, &devinfo_data);
			if (!res)
				goto cont;

			res = SetupDiGetDeviceRegistryPropertyA(device_info_set, &devinfo_data,
			               SPDRP_CLASS, NULL, (PBYTE)driver_name, sizeof(driver_name), NULL);
			if (!res)
				goto cont;

			if (strcmp(driver_name, "HIDClass") == 0) {
				// See if there's a driver bound.
				res = SetupDiGetDeviceRegistryPropertyA(device_info_set, &devinfo_data,
				           SPDRP_DRIVER, NULL, (PBYTE)driver_name, sizeof(driver_name), NULL);
				if (res)
					break;
			}
		}

        //wprintf(L"HandleName: %s\n", device_interface_detail_data->DevicePath);
        
        // Open a handle to the device
        write_handle = open_device(device_interface_detail_data->DevicePath, TRUE);
        
        // Check validity of write_handle.
        if (write_handle == INVALID_HANDLE_VALUE) {
            // Unable to open the device.
            goto cont_close;
        }        
        
        // Get the Vendor ID and Product ID for this device.
        attrib.Size = sizeof(HIDD_ATTRIBUTES);
        HidD_GetAttributes(write_handle, &attrib);
        //wprintf(L"Product/Vendor: %x %x\n", attrib.ProductID, attrib.VendorID);
        
        // Check the VID/PID to see if we should add this
        // device to the enumeration list.
        if ((vendor_id == 0x0 && product_id == 0x0) || 
            (attrib.VendorID == vendor_id && attrib.ProductID == product_id)) {
            
            #define WSTR_LEN 512
            const char *str;
            struct hid_device_info *tmp;
            HIDP_PREPARSED_DATA *pp_data = NULL;
            HIDP_CAPS caps;
            BOOLEAN res;
            NTSTATUS nt_res;
            wchar_t wstr[WSTR_LEN]; // TODO: Determine Size
            size_t len;
            
            /* VID/PID match. Create the record. */
            tmp = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
            if (cur_dev) {
                cur_dev->next = tmp;
            }
            else {
                root = tmp;
            }
            cur_dev = tmp;
            
            // Get the Usage Page and Usage for this device.
            res = HidD_GetPreparsedData(write_handle, &pp_data);
            if (res) {
                nt_res = HidP_GetCaps(pp_data, &caps);
                if (nt_res == HIDP_STATUS_SUCCESS) {
                    cur_dev->usage_page = caps.UsagePage;
                    cur_dev->usage = caps.Usage;
                }
                
                HidD_FreePreparsedData(pp_data);
            }
            
            /* Fill out the record */
            cur_dev->next = NULL;
            str = device_interface_detail_data->DevicePath;
            if (str) {
                len = strlen(str);
                cur_dev->path = (char*) calloc(len+1, sizeof(char));
                strncpy(cur_dev->path, str, len+1);
                cur_dev->path[len] = '\0';
            }
            else
                cur_dev->path = NULL;
            
            /* Serial Number */
            res = HidD_GetSerialNumberString(write_handle, wstr, sizeof(wstr));
            wstr[WSTR_LEN-1] = 0x0000;
            if (res) {
                cur_dev->serial_number = _wcsdup(wstr);
            }
            
            /* Manufacturer String */
            res = HidD_GetManufacturerString(write_handle, wstr, sizeof(wstr));
            wstr[WSTR_LEN-1] = 0x0000;
            if (res) {
                cur_dev->manufacturer_string = _wcsdup(wstr);
            }
            
            /* Product String */
            res = HidD_GetProductString(write_handle, wstr, sizeof(wstr));
            wstr[WSTR_LEN-1] = 0x0000;
            if (res) {
                cur_dev->product_string = _wcsdup(wstr);
            }
            
            /* VID/PID */
            cur_dev->vendor_id = attrib.VendorID;
            cur_dev->product_id = attrib.ProductID;
            
            /* Release Number */
            cur_dev->release_number = attrib.VersionNumber;
            
            /* Interface Number. It can sometimes be parsed out of the path
             on Windows if a device has multiple interfaces. See
             http://msdn.microsoft.com/en-us/windows/hardware/gg487473 or
             search for "Hardware IDs for HID Devices" at MSDN. If it's not
             in the path, it's set to -1. */
            cur_dev->interface_number = -1;
            if (cur_dev->path) {
                char *interface_component = strstr(cur_dev->path, "&mi_");
                if (interface_component) {
                    char *hex_str = interface_component + 4;
                    char *endptr = NULL;
                    cur_dev->interface_number = strtol(hex_str, &endptr, 16);
                    if (endptr == hex_str) {
                        /* The parsing failed. Set interface_number to -1. */
                        cur_dev->interface_number = -1;
                    }
                }
            }
        }
        
    cont_close:
        CloseHandle(write_handle);
    cont:
        // We no longer need the detail data. It can be freed
        free(device_interface_detail_data);
        
        device_index++;
        
    }
    
    // Close the device information handle.
    SetupDiDestroyDeviceInfoList(device_info_set);
    
    return root;
    
}

void  HID_API_EXPORT HID_API_CALL hid_free_enumeration(struct hid_device_info *devs)
{
    // TODO: Merge this with the Linux version. This function is platform-independent.
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


HID_API_EXPORT hid_device * HID_API_CALL hid_open(unsigned short vendor_id, unsigned short product_id, wchar_t *serial_number)
{
    // TODO: Merge this functions with the Linux version. This function should be platform independent.
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

HID_API_EXPORT hid_device * HID_API_CALL hid_open_path(const char *path)
{
    hid_device *dev;
    HIDP_CAPS caps;
    HIDP_PREPARSED_DATA *pp_data = NULL;
    BOOLEAN res;
    NTSTATUS nt_res;
    
    if (hid_init() < 0) {
        return NULL;
    }
    dev = get_hid_device_path(path);
    
    if(!dev)
        dev = new_hid_device();
    else
        return dev;
    
    // Open a handle to the device
    dev->device_handle = open_device(path, FALSE);
    
    // Check validity of write_handle.
    if (dev->device_handle == INVALID_HANDLE_VALUE) {
        // Unable to open the device.
        register_error(dev, "CreateFile");
        goto err;
    }
    
    // Get the Input Report length for the device.
    res = HidD_GetPreparsedData(dev->device_handle, &pp_data);
    if (!res) {
        register_error(dev, "HidD_GetPreparsedData");
        goto err;
    }
    nt_res = HidP_GetCaps(pp_data, &caps);
    if (nt_res != HIDP_STATUS_SUCCESS) {
        register_error(dev, "HidP_GetCaps");    
        goto err_pp_data;
    }
    dev->output_report_length = caps.OutputReportByteLength;
    dev->input_report_length = caps.InputReportByteLength;
    HidD_FreePreparsedData(pp_data);
    
    dev->read_buf = (char*) malloc(dev->input_report_length);
    
    return dev;
    
err_pp_data:
    HidD_FreePreparsedData(pp_data);
err:    
    CloseHandle(dev->device_handle);
    free(dev);
    return NULL;
}



int HID_API_EXPORT HID_API_CALL hid_write_timeout(hid_device *dev, const unsigned char *data, size_t length, int milliseconds)
{
    DWORD bytes_written = 0;
    unsigned char *buf = 0;
    BOOL res;
    HANDLE ev = dev->ol_write.hEvent;
        
    if (ev == NULL)
        return FALSE;

    if (!dev->write_pending) {
        // Start an Overlapped I/O read.
        dev->write_pending = TRUE;
        if (length >= dev->output_report_length) 
        {
            buf = (unsigned char *)data;
        } 
        else 
        {
            buf = (unsigned char *)malloc(dev->output_report_length);
            memcpy(buf, data, length);
            memset(buf + length, 0, dev->output_report_length - length);
            length = dev->output_report_length;
        }
        ResetEvent(ev);
        res = WriteFile(dev->device_handle, buf, (DWORD)length, &bytes_written, &dev->ol_write);
        if (!res || bytes_written != length) {
            if (GetLastError() != ERROR_IO_PENDING) {
                // WriteFile() has failed.
                // Clean up and return error.
                register_error(dev, "WriteFile");
                CancelIo(dev->device_handle);
                dev->write_pending = FALSE;
                bytes_written = -1;
                goto end_of_function;
            }
        }
    }
    if (milliseconds >= 0) {
        // See if there is any data yet.
        res = WaitForSingleObject(ev, milliseconds);
        if (res != WAIT_OBJECT_0) {
            // There was no data this time. Return zero bytes available,
            // but leave the Overlapped I/O running.
            if (buf && buf != data)
                free(buf);
            return 0;
        }
    }
    // Wait here until the write is done. This makes
    // hid_write() synchronous.
    res = GetOverlappedResult(dev->device_handle, &dev->ol_write, &bytes_written, TRUE);
    
    // Set pending back to false, even if GetOverlappedResult() returned error.
    dev->write_pending = TRUE;
    if (!res) {
        // The Write operation failed.
        register_error(dev, "GetOverlappedResult");
        bytes_written = -1;
        goto end_of_function;
    }
    
end_of_function:
    
    if (buf && buf != data)
        free(buf);
    
    return bytes_written;
}

int HID_API_EXPORT HID_API_CALL hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
    DWORD bytes_read = 0;
    BOOL res;
    
    // Copy the handle for convenience.
    HANDLE ev = dev->ol_read.hEvent;
    
    if (ev == NULL)
        return 0;
    
    if (!dev->read_pending) {
        // Start an Overlapped I/O read.
        dev->read_pending = TRUE;
        memset(dev->read_buf, 0, dev->input_report_length);
        ResetEvent(ev);
        res = ReadFile(dev->device_handle, dev->read_buf, (DWORD)dev->input_report_length, &bytes_read, &dev->ol_read);
        if (!res) {
            if (GetLastError() != ERROR_IO_PENDING) {
                // ReadFile() has failed.
                // Clean up and return error.
                CancelIo(dev->device_handle);
                dev->read_pending = FALSE;
                register_error(dev, "ReadFile");
                return -1;
            }
        }
    }
    
   if (milliseconds >= 0)
        {
        // See if there is any data yet.
        res = WaitForSingleObject(ev, milliseconds);
        if (res != WAIT_OBJECT_0) {
            // There was no data this time. Return zero bytes available,
            // but leave the Overlapped I/O running.
            return 0;
        }
    }
    
    // Either WaitForSingleObject() told us that ReadFile has completed, or
    // we are in non-blocking mode. Get the number of bytes read. The actual
    // data has been copied to the data[] array which was passed to ReadFile().
    res = GetOverlappedResult(dev->device_handle, &dev->ol_read, &bytes_read, TRUE/*wait*/);
    
    // Set pending back to false, even if GetOverlappedResult() returned error.
    dev->read_pending = FALSE;
    
    if (res && bytes_read > 0) {
        if (dev->read_buf[0] == 0x0) {
            /* If report numbers aren't being used, but Windows sticks a report
             number (0x0) on the beginning of the report anyway. To make this
             work like the other platforms, and to make it work more like the
             HID spec, we'll skip over this byte. */
            size_t copy_len;
            bytes_read--;
            copy_len = length > bytes_read ? bytes_read : length;
            memcpy(data, dev->read_buf+1, copy_len);
        }
        else {
            /* Copy the whole buffer, report number and all. */
            size_t copy_len = length > bytes_read ? bytes_read : length;
            memcpy(data, dev->read_buf, copy_len);
        }
    }
   if (!res) {
        register_error(dev, "GetOverlappedResult");
        return -1;
    }
    return bytes_read;
}


int HID_API_EXPORT HID_API_CALL hid_write(hid_device *dev, const unsigned char *data, size_t length)

{
    DWORD bytes_written;
    BOOL res;
    OVERLAPPED ol;
    unsigned char *buf;
    memset(&ol, 0, sizeof(ol));
    if (length >= dev->output_report_length) 
    { 
        buf = (unsigned char *)data;
    } 
    else 
    {
       buf = (unsigned char *)malloc(dev->output_report_length);
       memcpy(buf, data, length);
       memset(buf + length, 0, dev->output_report_length - length);
       length = dev->output_report_length;
    }
    res = WriteFile(dev->device_handle, buf, (DWORD)length, NULL, &ol);
    if (!res) {
        if (GetLastError() != ERROR_IO_PENDING) {
            // WriteFile() failed. Return error.
            register_error(dev, "WriteFile");
            if (buf != data)
                free(buf);
            return -1;
        }
    }

    // Wait here until the write is done. This makes
    // hid_write() synchronous.
    res = GetOverlappedResult(dev->device_handle, &ol, &bytes_written, TRUE/*wait*/);
    if (!res) {
        // The Write operation failed.
        register_error(dev, "WriteFile");
        if (buf != data)
            free(buf);
        return -1;
    }
    if (buf != data)
        free(buf);
    return bytes_written;
}

int HID_API_EXPORT HID_API_CALL hid_read(hid_device *dev, unsigned char *data, size_t length)
{
    return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

int HID_API_EXPORT HID_API_CALL hid_set_nonblocking(hid_device *dev, int nonblock)
{
    dev->blocking = !nonblock;
    return 0; /* Success */
}

int HID_API_EXPORT HID_API_CALL hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
    BOOL res = HidD_SetFeature(dev->device_handle, (PVOID)data, (ULONG)length);
    if (!res) {
        register_error(dev, "HidD_SetFeature");
        return -1;
    }
    
    return (int)length;
}


int HID_API_EXPORT HID_API_CALL hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
    BOOL res;
#if 0
    res = HidD_GetFeature(dev->device_handle, data, length);
    if (!res) {
        register_error(dev, "HidD_GetFeature");
        return -1;
    }
    return 0; /* HidD_GetFeature() doesn't give us an actual length, unfortunately */
#else
    DWORD bytes_returned;
    
    OVERLAPPED ol;
    memset(&ol, 0, sizeof(ol));
    
    res = DeviceIoControl(dev->device_handle,
                          IOCTL_HID_GET_FEATURE,
                          data, (DWORD)length,
                          data, (DWORD)length,
                          &bytes_returned, &ol);
    
    if (!res) {
        if (GetLastError() != ERROR_IO_PENDING) {
            // DeviceIoControl() failed. Return error.
            register_error(dev, "Get Feature Report DeviceIoControl");
            return -1;
        }
    }
    
    // Wait here until the write is done. This makes
    // hid_get_feature_report() synchronous.
    res = GetOverlappedResult(dev->device_handle, &ol, &bytes_returned, TRUE/*wait*/);
    if (!res) {
        // The operation failed.
        register_error(dev, "Get Feature Report GetOverLappedResult");
        return -1;
    }
    return bytes_returned;
#endif
}

void HID_API_EXPORT HID_API_CALL hid_close(hid_device *dev)
{
    hid_device * d = NULL;

    if (!dev)
        return;
    
    if(dev->ref_count == 0)
        return;

    if(dev->ref_count > 1){
       dev->ref_count--;
       return;
    }
    dev->ref_count = 0; 

    /* Remove it from the device list. */
    WaitForSingleObject(device_list_mutex, INFINITE);
    
    d = device_list;
    
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
    ReleaseMutex(device_list_mutex);
   
    CancelIo(dev->device_handle);
    CloseHandle(dev->ol_read.hEvent);
    CloseHandle(dev->ol_write.hEvent);
    CloseHandle(dev->device_handle);
    LocalFree(dev->last_error_str);
    free(dev->read_buf);
    free(dev);
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    BOOL res;
    
    res = HidD_GetManufacturerString(dev->device_handle, string, (ULONG)(2 * maxlen));
    if (!res) {
        register_error(dev, "HidD_GetManufacturerString");
        return -1;
    }
    
    return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    BOOL res;
    
    res = HidD_GetProductString(dev->device_handle, string, (ULONG)(2 * maxlen));
    if (!res) {
        register_error(dev, "HidD_GetProductString");
        return -1;
    }
    
    return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
    BOOL res;
    
    res = HidD_GetSerialNumberString(dev->device_handle, string, (ULONG)(2 * maxlen));
    if (!res) {
        register_error(dev, "HidD_GetSerialNumberString");
        return -1;
    }
    
    return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
    BOOL res;
    
    res = HidD_GetIndexedString(dev->device_handle, string_index, string, (ULONG)(2 * maxlen));
    if (!res) {
        register_error(dev, "HidD_GetIndexedString");
        return -1;
    }
    
    return 0;
}
    

HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
    return (wchar_t*)dev->last_error_str;
}

#ifdef HID_DEVICE_SUPPORT_CONNECT

void pthread_mutex_lock(HANDLE mutex)
{
    WaitForSingleObject(mutex, INFINITE);
}

void pthread_mutex_unlock(HANDLE mutex)
{
    ReleaseMutex(mutex);
}

static void  hid_device_callback_connect_free(struct hid_device_callback_connect *dev_connect)
{
    if(dev_connect){
        free(dev_connect);
    }
}

static int hid_init_register_connect_callbacks()
{
    connect_callback_mutex = CreateMutex(NULL, FALSE, NULL);
    return connect_callback_mutex?0:-1;
}

static int hid_deinit_register_connect_callbacks()
{
    if(connect_callback_mutex)
        CloseHandle(connect_callback_mutex);
    return 0;
}
static int hid_connect_registered(hid_device_callback callBack, hid_device_context context)
{
    struct hid_device_callback_connect *c = NULL;
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
    struct hid_device_callback_connect *hid_connect;
    int result = -1;
    if(NULL == callBack)
    {
        return -1;
    }
    
    pthread_mutex_lock(connect_callback_mutex);
    
    // check if has been registered callback
    if(hid_connect_registered(callBack,context)==0){
        pthread_mutex_unlock(connect_callback_mutex);
        return 0;
    }
    hid_connect = (hid_device_callback_connect*)calloc(1, sizeof(struct hid_device_callback_connect));
    hid_connect->callback = callBack;
    hid_connect->context = context;
    hid_connect->next = NULL;
    
    // add callback in the list
    if (!connect_callback_list)
        connect_callback_list = hid_connect;
    else {
        struct hid_device_callback_connect *c = connect_callback_list;
        while (c->next) {
            c = c->next;
        }
        c->next = hid_connect;
    }
    pthread_mutex_unlock(connect_callback_mutex);
    return 0;
}

static void hid_register_remove_callback(hid_device_callback callBack, hid_device_context context)
{
    struct hid_device_callback_connect *c = NULL;
    struct hid_device_callback_connect *d = NULL;
    
    if(NULL == callBack || NULL == connect_callback_list)
    {
        return;
    }
    
    pthread_mutex_lock(connect_callback_mutex);
    
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
    pthread_mutex_unlock(connect_callback_mutex);
}

int HID_API_EXPORT HID_API_CALL hid_add_notification_callback(hid_device_callback callBack, void *context)
{
    int result = -1;
    
    /* register device matching callback */
    result = hid_register_add_callback(callBack, context);
    return result;
}

void HID_API_EXPORT HID_API_CALL hid_remove_notification_callback(hid_device_callback  callBack, void *context)
{
    hid_register_remove_callback(callBack, context);
}

void HID_API_EXPORT HID_API_CALL hid_remove_all_notification_callbacks(void)
{
    hid_device_callback_connect *c = NULL;
    hid_device_callback_connect *p = NULL;
    
    if(NULL == connect_callback_list)
        return;
    pthread_mutex_lock(connect_callback_mutex);
    
    c = connect_callback_list;
    while (c) {
        p = c;
        c = c->next;
        hid_device_callback_connect_free(p);
    }
    hid_device_info_free(connect_device_info);
    connect_device_info = NULL;
    connect_callback_list = NULL;
        
    pthread_mutex_unlock(connect_callback_mutex);
}

#if defined(HID_DEVICE_NOTIFY) && defined(HID_DEVICE_SUPPORT_CONNECT)
static struct hid_device_info* hid_device_dup(struct hid_device_info *info)
{
 struct hid_device_info* dev_info;
 
 if(NULL == info)
     return NULL;

  dev_info = (struct hid_device_info*)malloc(sizeof(struct hid_device_info));
  if(dev_info){
     dev_info->path = strdup(info->path);
     dev_info->product_id = info->product_id;
     dev_info->vendor_id = info->vendor_id;
     dev_info->serial_number = _wcsdup(info->serial_number);
     dev_info->product_string = _wcsdup(info->product_string);
     dev_info->manufacturer_string = _wcsdup(info->manufacturer_string);
     dev_info->usage =  info->usage;
     dev_info->usage_page =  info->usage_page;
     dev_info->interface_number = info->interface_number;
     dev_info->release_number = info->release_number;
     dev_info->next = NULL;
  }
  return dev_info;
}
// this is call on WM_DEVICECHANGE
static void hid_device_notify_removal_callback(PHID_DEVICE_NOTIFY_INFO device_notify)
{
    struct hid_device_callback_connect *c = NULL; 
    if(NULL == device_notify)
        return;
    pthread_mutex_lock(connect_callback_mutex);
    c = connect_callback_list;
    if(NULL != connect_device_info){
        hid_device_info_free(connect_device_info);
        connect_device_info = NULL;
    }
    connect_device_info  = hid_device_dup(device_notify->devInfo);
    while (c) {
        if (c->callback) {
            (*c->callback)(connect_device_info, device_removal, c->context);
        }
        c = c->next;
    }
    pthread_mutex_unlock(connect_callback_mutex);
}

// this is call on WM_DEVICECHANGE
static void hid_device_notify_matching_callback(PHID_DEVICE_NOTIFY_INFO device_notify)
{
    struct hid_device_callback_connect *c = NULL;
    if(NULL == device_notify)
        return;
    pthread_mutex_lock(connect_callback_mutex);
    c = connect_callback_list;
    if(NULL != connect_device_info){
        hid_device_info_free(connect_device_info);
        connect_device_info = NULL;
    }
    connect_device_info = hid_device_dup(device_notify->devInfo);
    while (c) {
        if (c->callback) {
            (*c->callback)(connect_device_info, device_arrival, c->context);
        }
        c = c->next;
    }
    pthread_mutex_unlock(connect_callback_mutex);
}


BOOL handle_device_interface_change( UINT nEventType, PDEV_BROADCAST_DEVICEINTERFACE_A pdev)
{
    if(!pdev)
        return FALSE;
    switch (nEventType)
    {
      case DBT_DEVICEARRIVAL:
           {
             PHID_DEVICE_NOTIFY_INFO pdevInfo = NULL;
             if(pdevInfo = hid_device_register(pdev)){
                hid_device_notify_matching_callback(pdevInfo);
             }
           }
           break;
      case DBT_DEVICEREMOVECOMPLETE:
           break;
      }
    return TRUE;
}

BOOL handle_device_change(UINT nEventType, PDEV_BROADCAST_HANDLE pdev)
{
    BOOL ret = FALSE;
    PHID_DEVICE_NOTIFY_INFO pdevice = NULL;
    if(!pdev)
        return ret;
    pdevice = hid_find_notify_device(pdev->dbch_hdevnotify);
    if(!pdevice)
        return ret;
    switch(nEventType){
        case DBT_DEVICEQUERYREMOVE:
            hid_device_notify_removal_callback(pdevice);
            if(pdevice->hDevice){
               CloseHandle(pdevice->hDevice);
               pdevice->hDevice = NULL;
            }
            break;
        case DBT_DEVICEREMOVEPENDING:
            hid_device_notify_removal_callback(pdevice);
            if(pdevice->hHandleNotification){
               UnregisterDeviceNotification(pdevice->hHandleNotification);
               pdevice->hHandleNotification = NULL;
               pdevice->hDevice = NULL;
            }
            hid_remove_notify(pdevice);
            break;
        case DBT_DEVICEREMOVECOMPLETE:
            if(pdevice->hHandleNotification){
               UnregisterDeviceNotification(pdevice->hHandleNotification);
               pdevice->hHandleNotification = NULL;
            }
            if(pdevice->hDevice){
               hid_device_notify_removal_callback(pdevice);
               CloseHandle(pdevice->hDevice);
               pdevice->hDevice = NULL;
            }
            hid_remove_notify(pdevice);
            break;
     }
    return ret;
}

BOOL hid_device_change(UINT nEventType, DWORD_PTR dwData)
{
    BOOL ret = FALSE;
    PDEV_BROADCAST_DEVICEINTERFACE_A p = (PDEV_BROADCAST_DEVICEINTERFACE_A) dwData;
    if (!p)
        return FALSE;
     
    if (p->dbcc_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
         ret = handle_device_interface_change( nEventType, (PDEV_BROADCAST_DEVICEINTERFACE_A) p);
    } 
    else if (p->dbcc_devicetype == DBT_DEVTYP_HANDLE) {
         ret = handle_device_change( nEventType, (PDEV_BROADCAST_HANDLE) p);
    }
    return ret; 
}
#else
// this call 
static void hid_device_removal_callback(HANDLE handle, const char* device_path)
{
    HANDLE device_handle = NULL;
    hid_device_callback_connect *c = NULL;
    if(NULL != connect_device_info){
        hid_device_info_free(connect_device_info);
        connect_device_info = NULL;
    }
    if(NULL == handle || INVALID_HANDLE_VALUE == handle){
        device_handle = open_device(device_path, FALSE );
        if(device_handle && INVALID_HANDLE_VALUE != device_handle){
           connect_device_info = hid_device_info_create(device_handle, device_path);
        }
        else {
            return;
        }
    }
    else {
        connect_device_info = hid_device_info_create(handle, device_path);
    }
    c = connect_callback_list;
    while (c) {
        if (c->callback) {
            (*c->callback)(connect_device_info, device_removal, c->context);
        }
        c = c->next;
    }
    if(device_handle){
       CloseHandle(device_handle);
    }
}

static void hid_device_matching_callback(const char *device_path)
{
    HANDLE device_handle = NULL;
    struct hid_device_callback_connect *c = NULL; 
    if(NULL!=connect_device_info){
        hid_device_info_free(connect_device_info);
    }
    device_handle = open_device(device_path, FALSE);
    if(NULL == device_handle)
        return;

    connect_device_info = hid_device_info_create(device_handle,device_path);
    c = connect_callback_list;
    while (c) {
        if (c->callback) {
            (*c->callback)(connect_device_info, device_arrival, c->context);
        }
        c = c->next;
    }
    CloseHandle(device_handle);
}

BOOL hid_device_arrival(DWORD_PTR dwData)
{
    //callbacks
    PDEV_BROADCAST_DEVICEINTERFACE_A b = (PDEV_BROADCAST_DEVICEINTERFACE_A)dwData;
    hid_device_matching_callback(b->dbcc_name);
    return TRUE;
}

BOOL hid_device_removal(HANDLE handle,DWORD_PTR dwData)
{
    // callbacks
    PDEV_BROADCAST_DEVICEINTERFACE_A b = (PDEV_BROADCAST_DEVICEINTERFACE_A)dwData;  
    hid_device_removal_callback(handle,b->dbcc_name);
    return TRUE;
}

BOOL hid_device_change(UINT nEventType, DWORD_PTR dwData)
{
    PDEV_BROADCAST_DEVICEINTERFACE_A b = (PDEV_BROADCAST_DEVICEINTERFACE_A) dwData;
    switch (nEventType)
    {
        case DBT_DEVICEARRIVAL:
            hid_device_arrival(dwData);
            break;
        case DBT_DEVICEREMOVECOMPLETE:
            hid_device_removal(NULL,dwData);
            break;
        case DBT_DEVNODES_CHANGED:
            break;
    }
    return TRUE;
}
#endif

BOOL register_device_interface_to_hwnd(OUT HDEVNOTIFY *hDeviceNotify )
{
    // Windows objects for interacting with the driver.
    GUID InterfaceClassGuid = {0x4d1e55b2, 0xf16f, 0x11cf, 
        {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30} };
    
    DEV_BROADCAST_DEVICEINTERFACE NotificationFilter;
    
    if(NULL == hDeviceNotify){
        return FALSE;
    }
    
    ZeroMemory( &NotificationFilter, sizeof(NotificationFilter) );
    NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    NotificationFilter.dbcc_classguid = InterfaceClassGuid;
    
    *hDeviceNotify = RegisterDeviceNotificationA( 
                                                 hDeviceWindow,              // events recipient
                                                 &NotificationFilter,        // type of device
                                                 DEVICE_NOTIFY_WINDOW_HANDLE // type of recipient handle
                                                 );
    
    if ( NULL == *hDeviceNotify ) 
    {
        return FALSE;
    }
#if defined(HID_DEVICE_NOTIFY) && defined(HID_DEVICE_SUPPORT_CONNECT)
    hid_enum_notify_devices(hDeviceWindow);
#endif
    return TRUE;
}

void unregister_device_interface_to_hwnd(HDEVNOTIFY hDeviceNotify )
{  
    if(NULL != hDeviceNotify){
        UnregisterDeviceNotification(hDeviceNotify);
        hDeviceNotify = NULL;
    }
#if defined(HID_DEVICE_NOTIFY) && defined(HID_DEVICE_SUPPORT_CONNECT)
    hid_free_notify_devices();
#endif
}

INT_PTR WINAPI WinProcCallback(
                               HWND hWnd,
                               UINT message,
                               WPARAM wParam,
                               LPARAM lParam
                               )
{
    LRESULT ret = 1;
    switch (message)
    {
        case WM_CREATE:
            hDeviceWindow = hWnd;
            if(!register_device_interface_to_hwnd(&hDeviceNotify)){
                //printf("Error register");
            }
            break;
        case WM_DEVICECHANGE:
			{
              hid_device_change((UINT)wParam,(DWORD_PTR)lParam);
            }
            break;
        case WM_CLOSE:
        {
            unregister_device_interface_to_hwnd(hDeviceNotify);
            DestroyWindow(hWnd);
            hDeviceWindow = NULL;
        }
            break;
        default:
            ret = DefWindowProc(hWnd, message, wParam, lParam);
            break;
    }
    return ret;
}


DWORD WINAPI thread_proc(LPVOID data)
{
    // thread running
    BOOL  bRet = 0;
    HWND hwnd = (HWND)data;
   
   if(!init_device_notification())
        return bRet;

    while(!quit)
    {
        MSG msg;
        
        //remove any message that may be in queue
        while (bRet = PeekMessage(&msg, hwnd, 0, 0, PM_REMOVE)) 
        { 
            // processing message
            switch(msg.message){
                case WM_QUIT:
                    quit = TRUE;
                    break;
            }
            if (bRet == -1)  {
                quit = TRUE;
                break;
            } 
            TranslateMessage(&msg); 
            DispatchMessage(&msg); 
        }
#ifdef HID_DEBUG
        Sleep(1000);
        printf("HID thread\n");
#else
        Sleep(10);
#endif
    }
    
    deinit_device_notification();
    
    if( hDeviceWindow )
        DestroyWindow(hDeviceWindow);
    
    hDeviceWindow = NULL;
#ifdef HID_DEBUG
    printf("HID thread close\n");
#endif
    return 0;
}  

BOOL init_device_thread_window(HWND hWindow)
{  
    BOOL bInit = TRUE;
    pthread_mutex_lock(hid_lib_init_mutex);
    if(NULL == hThreadDeviceWnd){
        quit = FALSE;
        hThreadDeviceWnd = CreateThread(NULL, 0, thread_proc, hWindow, 0, &dwThreadID);
        bInit = (NULL!=hThreadDeviceWnd);
    }
    pthread_mutex_unlock(hid_lib_init_mutex);
    return bInit;
}

void deinit_device_thread_window()
{
    pthread_mutex_lock(hid_lib_init_mutex);
    if(hThreadDeviceWnd){
        quit = TRUE;
        PostThreadMessage(dwThreadID,WM_QUIT,0,0);
        WaitForSingleObject(hThreadDeviceWnd, INFINITE);
        CloseHandle(hThreadDeviceWnd);
        hThreadDeviceWnd= NULL;
    }
    pthread_mutex_unlock(hid_lib_init_mutex);
}


BOOL init_device_notification()
{
    WNDCLASSEXA wndClass;
    HINSTANCE hInstanceDLL;
    HWND hWnd;
    
    if(hDeviceWindow)
        return FALSE;
    
    hInstanceDLL = (HINSTANCE)(GetModuleHandle(0));
    
    wndClass.cbSize = sizeof(WNDCLASSEX);
    wndClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wndClass.hInstance = hInstanceDLL;
    wndClass.lpfnWndProc = (WNDPROC)(WinProcCallback);
    wndClass.cbClsExtra = 0;
    wndClass.cbWndExtra = 0;
    wndClass.hIcon = NULL;
    wndClass.hbrBackground = NULL;
    wndClass.hCursor = NULL;
    wndClass.lpszClassName = DEVICE_WND_CLASS_NAME;
    wndClass.lpszMenuName = NULL;
    wndClass.hIconSm = wndClass.hIcon;
    if ( !RegisterClassExA(&wndClass) )
    {
        //printf("Error RegisterClassEx\n");
        return FALSE;
    }
    hWnd = CreateWindowExA( WS_EX_LAYERED | WS_EX_TRANSPARENT,
                           DEVICE_WND_CLASS_NAME,
                           NULL,
                           WS_VISIBLE, // style
                           0, 0, 0, 0,
                           NULL, NULL, 
                           hInstanceDLL, 
                           NULL);
    
    if ( hWnd == NULL )
    {
        //printf("Error: CreateWindow");
        return FALSE;
    }
    // Actually hide the window.
    ShowWindow(hWnd, SW_HIDE);
    hDeviceWindow = hWnd;
    
    return TRUE;
}

void deinit_device_notification(void)
{
    if(hDeviceWindow){
        // for compatible with 64 bit
        HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrA(hDeviceWindow, GWLP_HINSTANCE); 
        SendMessage(hDeviceWindow,WM_CLOSE,0,0);
        UnregisterClassA(DEVICE_WND_CLASS_NAME, hInstance);
    }
}

#else
int HID_API_EXPORT HID_API_CALL hid_add_notification_callback(hid_device_callback callBack, void *context)
{
   return  -1;
}
void HID_API_EXPORT HID_API_CALL hid_remove_notification_callback(hid_device_callback  callBack, void *context)
{
}
void HID_API_EXPORT HID_API_CALL hid_remove_all_notification_callbacks(void)
{
}
#endif // NO SUPPORT

BOOL APIENTRY DllMain( HMODULE hModule,
        unsigned long  ul_reason_for_call,
        LPVOID lpReserved
        )
{
    switch(ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
        case DLL_THREAD_ATTACH:
            if(NULL == hid_lib_init_mutex)
            { 
                 hid_lib_init_mutex = CreateMutexA(NULL, FALSE, DEVICE_WND_THREAD_MUTEX);
            }
            break;
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            if(hid_lib_init_mutex)
            {
                CloseHandle(hid_lib_init_mutex);
                hid_lib_init_mutex = NULL;
            }
            break;
    }
    return TRUE;
}
//#define PICPGM
//#define S11
#define P32
#ifdef S11 
unsigned short VendorID = 0xa0a0;
unsigned short ProductID = 0x0001;
#endif

#ifdef P32
unsigned short VendorID = 0x04d8;
unsigned short ProductID = 0x3f;
#endif


#ifdef PICPGM
unsigned short VendorID = 0x04d8;
unsigned short ProductID = 0x0033;
#endif


#if 0
int __cdecl main(int argc, char* argv[])
{
    int res;
    unsigned char buf[65];
    
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);
    
    // Set up the command buffer.
    memset(buf,0x00,sizeof(buf));
    buf[0] = 0;
    buf[1] = 0x81;
    
    
    // Open the device.
    int handle = open(VendorID, ProductID, L"12345");
    if (handle < 0)
        printf("unable to open device\n");
    
    
    // Toggle LED (cmd 0x80)
    buf[1] = 0x80;
    res = write(handle, buf, 65);
    if (res < 0)
        printf("Unable to write()\n");
    
    // Request state (cmd 0x81)
    buf[1] = 0x81;
    write(handle, buf, 65);
    if (res < 0)
        printf("Unable to write() (2)\n");
    
    // Read requested state
    read(handle, buf, 65);
    if (res < 0)
        printf("Unable to read()\n");
    
    // Print out the returned buffer.
    for (int i = 0; i < 4; i++)
        printf("buf[%d]: %d\n", i, buf[i]);
    
    return 0;
}
#endif
#ifdef __cplusplus
} // extern "C"
#endif
