Java HID API for Windows, Linux, and Mac OS X

About
------

Java HID API is JNI wrapper allowing using HIDAPI library from Java code. HIDAPI is a multi-platform library which allows an application to interface with USB and Bluetooth HID-Class devices on Windows, Linux, and Mac OS X. 

License
--------
See LICENSE.txt

Download
---------
http://code.google.com/p/javahidapi/

Build Instructions
-------------------

Windows:
  Build the .sln file in the windows/ directory.
  Buildig JNI DDL is currently not yet implemented on Windows.

Linux:
        make -C linux
        ant

Mac OS X:
    make -C mac
    ant

Testing Instructions
-------------------

Running command 'ant test' will print you list of devices.  Then, it
would try to open PS3 Afterglow controller (
http://www.gamestop.com/ps3/accessories/ps3-afterglow-controller/69393
) and read and print data from it. If it is not present,
HIDDeviceNotFoundException will be thrown.

        

