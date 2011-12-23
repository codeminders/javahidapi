
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "jni-stubs/com_codeminders_hidapi_HIDManager.h"
#include "hidapi/hidapi.h"
#include "hid-java.h"

#ifdef MAC_OS_X
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#include <pthread.h>
#endif

#ifdef MAC_OS_X
#define HID_RUN_LOOP
#endif

#define DEBUG 0


static JNIEnv *m_env = NULL;
static JavaVM *m_vm = NULL;

/* JNI reference count */
static int jni_ref_count = 0;

#ifdef  HID_RUN_LOOP 
#define SLEEP_TIME    100 * 1000
static volatile int squit = 0;
static int hid_mgr_init = 0;
static int cond = FALSE;
static pthread_cond_t condition;
static pthread_t runloop_thread = NULL;
static CFRunLoopRef run_loop = NULL;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

#endif

static int init_hid_mgr()
{
#ifdef HID_RUN_LOOP 
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
    return 1;
#endif
}

#ifdef  HID_RUN_LOOP 

static void *hid_runloop_thread(void *param)
{
    SInt32 code = 0;
    
    if( NULL == m_env )
        return NULL;
    
    int res = m_vm->AttachCurrentThread( (void**) &m_env, NULL );
    if(res < 0){
    #if DEBUG        
        printf("Attached failed\n");
    #endif
        return NULL;
    }  
    
    run_loop = CFRunLoopGetCurrent();
   
    pthread_mutex_lock(&mutex);
    
    if(hid_init() == -1){
        pthread_cond_destroy(&condition);
        hid_mgr_init = 0;
        return NULL;
    }
  
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
#if DEBUG        
        printf("HID run loop thread\n");
#endif
        usleep(SLEEP_TIME);
    }
    if(m_vm){
       m_vm->DetachCurrentThread();
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
        squit = 0;
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
    m_env = NULL;
    m_vm = NULL;
}

static int hid_init_loop()
{
    return hid_runloop_startup();
}

static int hid_exit_loop()
{
    if(init_hid_mgr()){
       hid_runloop_exit();
       hid_mgr_init = 0;
    }
    return 0;
}

#endif

static jobject getPeer(JNIEnv *env, jobject self)
{
    jclass cls = env->FindClass(HID_MANAGER_CLASS);
    assert(cls!=NULL);
    if (cls == NULL) 
        return NULL;
    jfieldID fid = env->GetFieldID(cls, "peer", "J");
    return (jobject)(env->GetLongField(self, fid));
}

static void setPeer(JNIEnv *env, jobject self, jobject peer)
{
    jclass cls = env->FindClass(HID_MANAGER_CLASS);
    assert(cls!=NULL);
    if (cls == NULL) 
        return; 
    jfieldID fid = env->GetFieldID(cls, "peer", "J");
    jlong peerj = (jlong)peer;
    env->SetLongField(self, fid, peerj);     
}

static void setIntField(JNIEnv *env,
                        jclass cls,
                        jobject obj,
                        const char *name,
                        int val)
{
    jfieldID fid = env->GetFieldID(cls, name, "I");
    env->SetIntField(obj, fid, val);
}

static void setStringField(JNIEnv *env,
                           jclass cls,
                           jobject obj,
                           const char *name,
                           const char *val)
{
    jfieldID fid = env->GetFieldID(cls, name, "Ljava/lang/String;");
    env->SetObjectField(obj, fid,  val ? env->NewStringUTF(val) : NULL);
}

static void setUStringField(JNIEnv *env,
                           jclass cls,
                           jobject obj,
                           const char *name,
                           const wchar_t *val)
{
    jfieldID fid = env->GetFieldID(cls, name, "Ljava/lang/String;");

    if(val)
    {
        char *u8 = convertToUTF8(env, val);
        env->SetObjectField(obj, fid, env->NewStringUTF(u8));
        free(u8);
    }
    else
        env->SetObjectField(obj, fid, NULL);
}


static jobject createHIDDeviceInfo(JNIEnv *env, jclass cls, struct hid_device_info *dev)
{
    jmethodID cid = env->GetMethodID(cls, "<init>", "()V");
    if (cid == NULL) 
        return NULL; /* exception thrown. */ 
    
    if (dev == NULL)
        return NULL;

    jobject result = env->NewObject(cls, cid);

    setIntField(env, cls, result, "vendor_id", dev->vendor_id);
    setIntField(env, cls, result, "product_id", dev->product_id);
    setIntField(env, cls, result, "release_number", dev->release_number);
    setIntField(env, cls, result, "usage_page", dev->usage_page);
    setIntField(env, cls, result, "usage", dev->usage);
    setIntField(env, cls, result, "interface_number", dev->interface_number);
    
    setStringField(env, cls, result, "path", dev->path);
    setUStringField(env, cls, result, "serial_number", dev->serial_number);
    setUStringField(env, cls, result, "manufacturer_string", dev->manufacturer_string);
    setUStringField(env, cls, result, "product_string", dev->product_string);

    return result;
}

// This is C callback call from C code and then call java methods
void hid_device_jni_callback(const struct hid_device_info *dev,  hid_device_event typeEvent, void *context)
{
    if( m_env == NULL )
        return;
    
    if( context == NULL)
        return;
    
    int res = m_vm->AttachCurrentThread( (void**) &m_env, NULL );
    
    if(res < 0){
#if DEBUG        
       printf("Attached failed\n");
#endif
       return;
    }
    
    if(!init_hid_mgr())
    {
       m_vm->DetachCurrentThread();
       return;
    }

    jobject clsObj = (jobject)context;
    jclass cls = NULL;
    
    if(clsObj){
        cls = m_env->GetObjectClass(clsObj);
    }
     
    if(cls == NULL){
       cls = m_env->FindClass( HID_MANAGER_CLASS );
    }
    
    if(cls == NULL)
    {
       m_vm->DetachCurrentThread();
       return;
    }
     
    jclass infoCls = m_env->FindClass(DEVINFO_CLASS);
    if (infoCls == NULL) {
        m_vm->DetachCurrentThread();
        return;
    } 
    if(typeEvent == device_arrival)
    {
        struct hid_device_info *devInfo = (struct hid_device_info *)dev;
        jmethodID mid = m_env->GetMethodID(cls, "deviceAdded", "(Lcom/codeminders/hidapi/HIDDeviceInfo;)V");
        if(mid){
           jobject idevInfo = createHIDDeviceInfo(m_env, infoCls, devInfo);
            m_env->CallVoidMethod(clsObj, mid, idevInfo );
        }
        else{
#if DEBUG        
         printf("No such method\n");
#endif
        }
    }
    else if(typeEvent == device_removal)
    {
        struct hid_device_info *devInfo = (struct hid_device_info *)dev;
        jmethodID mid = m_env->GetMethodID(cls, "deviceRemoved", "(Lcom/codeminders/hidapi/HIDDeviceInfo;)V");
        if(mid){
           jobject idevInfo = createHIDDeviceInfo(m_env, infoCls, devInfo);
           m_env->CallVoidMethod(clsObj, mid, idevInfo );
        }
        else{
#if DEBUG        
         printf("No such method\n");
#endif
        }
    }
}

static int registerHIDDeviceCallback(JNIEnv *env, jobject obj)
{
    int res = 0;
#ifdef HID_RUN_LOOP
    if(!init_hid_mgr())
    {
        throwIOException(env, NULL);
        return res;
    }
    if(hid_mgr_init){
        res = hid_add_notification_callback(hid_device_jni_callback, (void*)obj);
    }
#else
    res = hid_add_notification_callback(hid_device_jni_callback, (void*)obj);
#endif
    return res;
}


JNIEXPORT jobjectArray JNICALL
Java_com_codeminders_hidapi_HIDManager_listDevices(JNIEnv *env, jclass cls)
{
    struct hid_device_info *devs, *cur_dev;
    int res = 0;
    
#ifdef HID_RUN_LOOP    
    res = hid_init_loop(); 
#else
    res = hid_init();
#endif    
    if(res != 0){
        throwIOException(env, NULL);
        return NULL;
    }
    if(!init_hid_mgr())
    {
        throwIOException(env, NULL);
        return NULL;
    }
    
    devs = hid_enumerate(0x0, 0x0);
    if(devs == NULL)
    {
       throwIOException(env, NULL);
       return NULL;
    }
    
    cur_dev = devs;
    int size=0;
    while(cur_dev)
    {
       size++;
       cur_dev = cur_dev->next;
    }

    jclass infoCls = env->FindClass(DEVINFO_CLASS);
    if (infoCls == NULL) {
        return NULL; /* exception thrown */
    }
    jobjectArray result= env->NewObjectArray(size, infoCls, NULL);
    cur_dev = devs;
    int i=0;
    while(cur_dev)
    {
        jobject x = createHIDDeviceInfo(env, infoCls, cur_dev);
        if(x == NULL)
           return NULL; /* exception thrown */ 

        env->SetObjectArrayElement(result, i, x);
        env->DeleteLocalRef(x);
        i++;
        cur_dev = cur_dev->next;
    }
    hid_free_enumeration(devs);
    
    /* Free local references */
    env->DeleteLocalRef(cls);
    
    return result;
}

JNIEXPORT void JNICALL Java_com_codeminders_hidapi_HIDManager_init(JNIEnv *env, jobject obj)
{
    int res = 0;
    jobject jobjRef = 0;
    if(NULL == m_env)
    {
      m_env = env;
      m_env->GetJavaVM( &m_vm );
    }
#ifdef HID_RUN_LOOP    
    res = hid_init_loop(); 
#else
    res = hid_init();
#endif    
     
    if(res !=0 )
    {
       throwIOException(env, NULL);
       return;
    }
    
    jobjRef = env->NewGlobalRef(obj);
    setPeer(env, obj, jobjRef);
#if DEBUG        
    printf("JNI - init peer(objRef) =  %p \n", jobjRef);
#endif
        
    if(jobjRef){
       registerHIDDeviceCallback(env, jobjRef);
    }
    jni_ref_count++;
}
    
JNIEXPORT void JNICALL Java_com_codeminders_hidapi_HIDManager_release(JNIEnv *env, jobject obj )
{
    int res = 0;
    jobject jobjRef = (jobject)getPeer(env, obj);
#if DEBUG        
    printf("JNI - release peer(jobjRef) =  %p \n", jobjRef);
#endif
    if(jobjRef){
        hid_remove_notification_callback(hid_device_jni_callback, jobjRef);
        m_env->DeleteGlobalRef(jobjRef);
        setPeer(env,obj,0);
        jni_ref_count--;
    }
#if DEBUG        
    printf("jni_ref_count = %d\n", jni_ref_count);
#endif
    if(jni_ref_count>0){ 
       return;     
    }
#ifdef HID_RUN_LOOP    
    res = hid_exit_loop(); 
#else
    res = hid_exit();
#endif
    if(res !=0 )
    {
       throwIOException(env, NULL);
    }
}
