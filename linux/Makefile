###########################################
# Simple Makefile for HIDAPI test program
#
# Alan Ott
# Signal 11 Software
# 2010-06-01
###########################################


JNIOBJS=HIDManager.o HIDDeviceInfo.o HIDDevice.o hid-java.o
JNIINCLUDES=-I.. -I../jni-impl -I/usr/lib/jvm/java-6-openjdk/include/ -I../jni-impl -I/usr/lib/jvm/jdk1.7.0/include -I../jni-impl -I/usr/lib/jvm/jdk1.7.0/include/linux -I/usr/java/jdk1.7.0_02/include -I/usr/java/jdk1.7.0_02/include/linux
JNILIBS=
JNISHAREDLIB=libhidapi-jni.so
JNISHAREDLIBVER=1.0

CC=gcc
CXX=g++
COBJS=hid.o
CPPOBJS=../hidtest/hidtest.o
OBJS=$(COBJS) $(CPPOBJS) $(JNIOBJS)
CFLAGS+=-fPIC -I../hidapi -g -c `pkg-config libusb-1.0 --cflags` $(JNIINCLUDES)
LIBS=`pkg-config libusb-1.0 libudev --libs` -ludev -lpthread $(JNILIBS)

all: hidtest $(JNISHAREDLIB)

$(JNISHAREDLIB): $(OBJS)
	$(CXX) -shared $(COBJS) $(JNIOBJS) $(LIBS) -o $(JNISHAREDLIB)

hidtest: $(OBJS)
	g++ -Wall -g $^ $(LIBS) -o hidtest

%.o: ../jni-impl/%.cpp
	$(CXX) $(CFLAGS) $< -o $@

$(COBJS): %.o: %.c
	$(CC) $(CFLAGS) $< -o $@

$(CPPOBJS): %.o: %.cpp
	$(CXX) $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJS) hidtest $(JNISHAREDLIB)

.PHONY: clean
