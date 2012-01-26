/*******************************************************
 Windows HID simplification

 Alan Ott
 Signal 11 Software

 8/22/2009

 Copyright 2009, All Rights Reserved.
 
 This contents of this file may be used by anyone
 for any reason without any conditions and may be
 used as a starting point for your own applications
 which use HIDAPI.
********************************************************/

#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>

#include "hidapi.h"

#if defined(__APPLE__)
 #define MAC_OS_X
#endif

#ifdef MAC_OS_X
#import <IOKit/usb/IOUSBLib.h>
#define RUN_LOOP_THEAD 
#endif

#define MAX_STR   255

// Headers needed for sleeping.
#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <pthread.h>
#endif

static void list_devices()
{
    struct hid_device_info *devs, *cur_dev;
    devs = hid_enumerate(0x0, 0x0);
    cur_dev = devs;    
    while (cur_dev) {
    printf("Device Found\n  type: %04hx %04hx\n  path: %s\n  serial_number: %ls", 
           cur_dev->vendor_id, cur_dev->product_id, cur_dev->path, cur_dev->serial_number);
    printf("\n");
    printf("  Manufacturer: %ls\n", cur_dev->manufacturer_string);
    printf("  Product:      %ls\n", cur_dev->product_string);
    printf("  Release:      %hx\n", cur_dev->release_number);
    printf("  Interface:    %d\n",  cur_dev->interface_number);
    printf("\n");
    cur_dev = cur_dev->next;
    }
    hid_free_enumeration(devs);
}

void hid_device_change(const struct hid_device_info *deviceInfo, hid_device_event  eventType, void *context);

int hid_check_add_remove_notify();
int hid_multi_devices_open();

void hid_device_change(const struct hid_device_info *deviceInfo, hid_device_event  eventType, void *context)
{
    printf("Event Type: %s\n", (eventType == device_arrival)?"Arrival":"Removal");
    if(deviceInfo)
    {
        printf("  %04hx %04hx\n", deviceInfo->vendor_id, deviceInfo->product_id);
        printf("  Serial: %ls\n", deviceInfo->serial_number);
        printf("  Manufacturer: %ls\n", deviceInfo->manufacturer_string);
        printf("  Path: %s\n", deviceInfo->path);
        printf("  Product: %ls\n",deviceInfo->product_string);
        printf("  Release version: %d\n",deviceInfo->release_number);
    }
    struct hid_device_info *devs, *cur_dev;
    int k = 0;
    
    devs = hid_enumerate(0x0, 0x0);
    cur_dev = devs;    
    while (cur_dev) {
        cur_dev = cur_dev->next;
        k++;
    }
    
    hid_free_enumeration(devs);
    printf("Count devices = %d\n\n",k);
    
}
#ifdef RUN_LOOP_THEAD
static volatile int squit = 0;
static int hid_mgr_init = 0;
static int cond = FALSE;
static pthread_cond_t condition;
static pthread_t runloop_thread = NULL;
static CFRunLoopRef run_loop = NULL;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void *hid_runloop_thread(void *param)
{
    SInt32 code = 0;
        
    run_loop = CFRunLoopGetCurrent();
    
    hid_init();
    
    pthread_mutex_lock(&mutex);
    cond = true;
    pthread_cond_signal(&condition);
    pthread_mutex_unlock(&mutex);
    
    while(!squit)
    {
        code = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
        if( code == kCFRunLoopRunFinished ||  code == kCFRunLoopRunStopped )
        {
           break;
        }
        sleep(1);
    }

    return NULL;
}

static int hid_runloop_startup()
{  
    if(hid_mgr_init)
        return 0;
  	hid_mgr_init = 1;
    
    if(squit)
    { 
        pthread_cond_destroy(&condition);
        pthread_join(runloop_thread, NULL);
        squit = 0;
    }
    else 
  	{
        pthread_attr_t attr;
        pthread_attr_init( &attr );
        pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
        pthread_cond_init(&condition, NULL);
        pthread_create(&runloop_thread, &attr, hid_runloop_thread, NULL);
    }
	hid_mgr_init = 1;
    return 0;
}

static void hid_runloop_exit()
{
    squit = 1;
    pthread_cond_destroy(&condition);
    pthread_join(runloop_thread, NULL);
}
#endif

static int hid_init_1()
{
#ifdef RUN_LOOP_THEAD
    return hid_runloop_startup();
#else
	return hid_init();
#endif
}

static int hid_exit_1()
{
#ifdef RUN_LOOP_THEAD
    if(hid_mgr_init)
	{
        hid_runloop_exit();
		hid_mgr_init = 0;
    }
#else
	hid_exit();
#endif
    return 0;
}


static int init_hid_mgr()
{
#ifdef RUN_LOOP_THEAD
    if(hid_mgr_init)
    {
        pthread_mutex_lock(&mutex);
        while(cond == FALSE){
            pthread_cond_wait(&condition, &mutex);
        }
        pthread_mutex_unlock(&mutex);
        return 1;
    }
    return 0;
#else
    return (hid_init()!=-1);
#endif
}

static void hid_test_multiplies()
{
	const int numTimes = 10;
	for(int i = 0; i < numTimes; i++)
	{
      hid_init_1();
	
#ifdef RUN_LOOP_THEAD	
	  pthread_mutex_lock(&mutex);
	  while(cond == FALSE){
		  pthread_cond_wait(&condition, &mutex);
	  }
	  pthread_mutex_unlock(&mutex);
#endif
	  hid_add_notification_callback(hid_device_change, NULL);
	  hid_remove_notification_callback(hid_device_change,NULL);
	
	  hid_exit_1();
	}
}
int hid_check_add_remove_notify()
{
  int quit = 0;
  printf("Press key 'q' to quit.\n");
  hid_init_1();
#ifdef RUN_LOOP_THEAD
	
    pthread_mutex_lock(&mutex);
    
    while(cond == FALSE){
        pthread_cond_wait(&condition, &mutex);
    }
    
    pthread_mutex_unlock(&mutex);
    
    hid_add_notification_callback(hid_device_change, NULL);
    
    list_devices();
    
    hid_device *handle = 0;//hid_open(0x4f3, 0x216, NULL);
    if(handle){
        printf("Open device\n");    
        hid_close(handle);
    }
    else {
        printf("Error Open device\n");    
    }
    while(!quit){
        printf("waiting connect/disconnect...\n");
        if(getchar() == 'q'){
            quit = 1;
        }
        sleep(1);
    }
#else
  hid_add_notification_callback(hid_device_change, NULL);
  list_devices();
  while ( !quit ) 
  {
     printf("waiting connect/disconnect...\n");
     #ifdef _WIN32
      ::Sleep(1);
     #else
        usleep(50*1000);
     #endif 
     if(getchar() == 'q')
     {
        quit = 1;
     }
  } 
#endif
  hid_remove_notification_callback(hid_device_change,NULL);
  hid_remove_all_notification_callbacks();
	
  hid_exit_1();
	
  return 0;
}
    
int hid_multi_devices_open()
{
    wchar_t wstr[MAX_STR];
    int res = 0;
    unsigned short vid = 0x4f3;
    unsigned short pid = 0x216;
    
    hid_device *handle1;
    hid_device *handle2;
    
    printf("Device = %04hx %04hx\n", vid, pid);
    
    handle1 = hid_open(vid, pid, NULL);
    if (!handle1) {
        printf("unable to open device\n");
         return 1;
    }
    handle2 = hid_open(vid, pid, NULL);
    if (!handle2) {
        printf("unable to open device\n");
        hid_close(handle1);        
         return 1;
    }
    // Read the Manufacturer String
    wstr[0] = 0x0000;
    res = hid_get_manufacturer_string(handle1, wstr, MAX_STR);
    if (res < 0)
        printf("Unable to read manufacturer string\n");
    printf("Manufacturer String: %ls\n", wstr);
    
    // Read the Product String
    wstr[0] = 0x0000;
    res = hid_get_product_string(handle1, wstr, MAX_STR);
    if (res < 0)
        printf("Unable to read product string\n");
    printf("Product String: %ls\n", wstr);
    
    wstr[0] = 0x0000;
    res = hid_get_manufacturer_string(handle2, wstr, MAX_STR);
    if (res < 0)
        printf("Unable to read manufacturer string\n");
    printf("Manufacturer String: %ls\n", wstr);
    
    // Read the Product String
    wstr[0] = 0x0000;
    res = hid_get_product_string(handle2, wstr, MAX_STR);
    if (res < 0)
        printf("Unable to read product string\n");
    printf("Product String: %ls\n", wstr);
    
    if(handle1 == handle2){
        printf("Open device pointer1 = pointer2 = %p\n",handle1); 
    }
    hid_close(handle1);
    hid_close(handle2);
    
    return 0;
}


static void usage()
{
    const char *progname = "hidtest";
    fprintf(stderr,
            "Usage:\n"
            "%s [test option...] \n"
            "Options:\n"
            "-c  connect/disconnect events\n"
            "-o  multiply open devices\n"
            "-e  enumerate devices\n",
            progname);
    exit(1);            
}

int main(int argc, char* argv[])
{
    int res;
    unsigned char buf[256];
    wchar_t wstr[MAX_STR];
    hid_device *handle;
    int i;
    int itest = -1;

#ifdef _WIN32
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);
#endif
    if( argc <= 1){
        usage();
    }
    for( i = 1; i< argc; i++ ){
        const char *arg = argv[i];
        if(arg[0]!='-') {
            usage();
        }
        else{
            arg+=1;
            if(arg[0] == 'c') //connect
                 itest = 0;
            else if(arg[0] == 'o') // open
                 itest = 1;
            else if(arg[0] == 'e') // enumerate
                 itest = 2;
            else {
                 usage();    
            }
        }
    }
    switch(itest){
        case 0:
			hid_test_multiplies();
            hid_check_add_remove_notify();
            break;
        case 1:
            hid_multi_devices_open();
            break;
    }
    
    if( itest != 2 )
        goto quit;
    
    if(!init_hid_mgr())
        goto quit;
    
    list_devices();
    
    // Set up the command buffer.
    memset(buf,0x00,sizeof(buf));
    buf[0] = 0x01;
    buf[1] = 0x81;
    
    // Open the device using the VID, PID,
    // and optionally the Serial number.
    handle = hid_open(0x5ac, 0x8242, NULL);
   // handle = hid_open(0x4f3, 0x216, NULL);
    if (!handle) {
        printf("unable to open device\n");
         goto quit;
    }

    // Read the Manufacturer String
    wstr[0] = 0x0000;
    res = hid_get_manufacturer_string(handle, wstr, MAX_STR);
    if (res < 0)
        printf("Unable to read manufacturer string\n");
    printf("Manufacturer String: %ls\n", wstr);

    // Read the Product String
    wstr[0] = 0x0000;
    res = hid_get_product_string(handle, wstr, MAX_STR);
    if (res < 0)
        printf("Unable to read product string\n");
    printf("Product String: %ls\n", wstr);

    // Read the Serial Number String
    wstr[0] = 0x0000;
    res = hid_get_serial_number_string(handle, wstr, MAX_STR);
    if (res < 0)
        printf("Unable to read serial number string\n");
    printf("Serial Number String: (%d) %ls", (int)wstr[0], wstr);
    printf("\n");

    // Read Indexed String 1
    wstr[0] = 0x0000;
    res = hid_get_indexed_string(handle, 1, wstr, MAX_STR);
    if (res < 0)
        printf("Unable to read indexed string 1\n");
    printf("Indexed String 1: %ls\n", wstr);

    // Set the hid_read() function to be non-blocking.
    hid_set_nonblocking(handle, 1);
    
    // Try to read from the device. There shoud be no
    // data here, but execution should not block.
    res = hid_read(handle, buf, 17);

    // Send a Feature Report to the device
    buf[0] = 0x2;
    buf[1] = 0xa0;
    buf[2] = 0x0a;
    buf[3] = 0x00;
    buf[4] = 0x00;
    res = hid_send_feature_report(handle, buf, 17);
    if (res < 0) {
        printf("Unable to send a feature report.\n");
    }

    memset(buf,0,sizeof(buf));

    // Read a Feature Report from the device
    buf[0] = 0x2;
    res = hid_get_feature_report(handle, buf, sizeof(buf));
    if (res < 0) {
        printf("Unable to get a feature report.\n");
        printf("%ls", hid_error(handle));
    }
    else {
        // Print out the returned buffer.
        printf("Feature Report\n   ");
        for (i = 0; i < res; i++)
            printf("%02hhx ", buf[i]);
        printf("\n");
    }

    memset(buf,0,sizeof(buf));

    // Toggle LED (cmd 0x80). The first byte is the report number (0x1).
    buf[0] = 0x00;
    buf[1] = 0x80;
	res = hid_write(handle, buf, 17);
    if (res < 0) {
        printf("Unable to write()\n");
        printf("Error: %ls\n", hid_error(handle));
    }

    // Request state (cmd 0x81). The first byte is the report number (0x1).
    buf[0] = 0x1;
    buf[1] = 0x81;
    hid_write(handle, buf, 17);
    if (res < 0)
        printf("Unable to write() (2)\n");

    // Read requested state. hid_read() has been set to be
    // non-blocking by the call to hid_set_nonblocking() above.
    // This loop demonstrates the non-blocking nature of hid_read().
    res = 0;
    while (res == 0) {
        res = hid_read(handle, buf, sizeof(buf));
        if (res == 0)
            printf("waiting...\n");
        if (res < 0)
            printf("Unable to read()\n");
        #ifdef _WIN32
        Sleep(500);
        #else
        usleep(500*1000);
        #endif
    }

    printf("Data read:\n   ");
    // Print out the returned buffer.
    for (i = 0; i < res; i++)
        printf("%02hhx ", buf[i]);
    printf("\n");

quit:
#ifdef _WIN32
    system("pause");
#endif
    return 0;
}
