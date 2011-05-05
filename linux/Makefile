###########################################
# Simple Makefile for HIDAPI test program
#
# Alan Ott
# Signal 11 Software
# 2010-06-01
###########################################


JNIOBJS=HIDManager.o HIDDeviceInfo.o HIDDevice.o hid-java.o
JNIINCLUDES=-I.. -I../jni-impl -I/System/Library/Frameworks/JavaVM.framework/Headers 
JNILIBS=-l iconv
JNISHAREDLIB=libhidapi-jni.jnilib
JNISHAREDLIBVER=1.0

CC=gcc
CXX=g++
COBJS=hid-libusb.o
CPPOBJS=../hidtest/hidtest.o
OBJS=$(COBJS) $(CPPOBJS) $(JNIOBJS)
CFLAGS+=-I../hidapi -Wall -g -c `pkg-config libusb-1.0 --cflags` $(JNIINCLUDES)
LIBS=`pkg-config libusb-1.0 libudev --libs` $(JNILIBS)

all: hidtest $(JNISHAREDLIB)

$(JNISHAREDLIB): $(OBJS)
	$(CC) -dynamiclib -current_version $(JNISHAREDLIBVER) $(COBJS) $(JNIOBJS) $(LIBS) -o $(JNISHAREDLIB)

hidtest: $(OBJS)
	g++ -Wall -g $^ $(LIBS) -o hidtest

%.o: ../jni-impl/%.c
	$(CC) -c $(CFLAGS) $< -o $@

$(COBJS): %.o: %.c
	$(CC) $(CFLAGS) $< -o $@

$(CPPOBJS): %.o: %.cpp
	$(CXX) $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJS) hidtest $(JNISHAREDLIB)

.PHONY: clean
