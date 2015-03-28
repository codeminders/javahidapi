# Java HID API for Windows, Linux, and Mac OS X #

## About ##

Java HID API is JNI wrapper allowing using HIDAPI library from Java code. HIDAPI is a multi-platform<br> library which allows an application to interface with USB and Bluetooth HID-Class devices on Windows,<br> Linux, and Mac OS X.<br>
<br>
<h2>License</h2>

This code is base on open-source implementation and is governed by variety by licenses as described in<br> LICENSE.txt file. In particular<br>
<br>
HIDAPI can be used under one of three licenses.<br>
<br>
<ol><li>The GNU Public License, version 3.0, in LICENSE-gpl3.txt<br>
</li><li>A BSD-Style License, in LICENSE-bsd.txt.<br>
</li><li>The more liberal original HIDAPI license. LICENSE-orig.txt.</li></ol>

The license chosen is at the discretion of the user of JavaHIDAPI. For example:<br>
<br>
<ol><li>An author of GPL software would likely use JavaHIDAPI under the terms of the GPL.<br>
</li><li>An author of commercial closed-source software would likely use JavaHIDAPI under the terms<br> of the BSD-style license or the original JavaHIDAPI license.</li></ol>

<h2>Download</h2>

The project is hosted at and could be download from <a href='http://code.google.com/p/javahidapi/'>http://code.google.com/p/javahidapi/ </a><br>
The project site includes issue tracking system where you can submit your bug reports.<br>
<br>
<h2>Build Instructions</h2>

<h3>Windows</h3>

To build you need to have Microsolt Visual Studio 2008 (We used version 9.0.21022.8 RTM).<br>
<br>
Build the .sln file in the windows/ directory. This will build <i>hidapi-jni.dll</i> in <i>windows/release</i> and<br> <i>windows/debug</i> folders.<br>
<br>
<h3>Linux</h3>
<h4>Red Hat Enterprise Linux 6</h4>
You need to have following libraries/packages installed:<br>
<ol><li>libudev<br>
</li><li>libudev-devel<br>
</li><li>libusb1<br>
</li><li>libusb1-devel-1.0.3-1.el6.i686<br>
<h4>Ubuntu 11.10</h4>
You need to have following libraries/packages installed:<br>
</li><li>libudev<br>
</li><li>libudev-devel<br>
</li><li>libusb-1.0-0-dev</li></ol>

To compile source code use following commands:<br>
<pre><code>      make C linux<br>
      ant<br>
</code></pre>
<h4>Mac OS X</h4>
To build under MacOS you need to have following libraries/packages installed:<br>
<ol><li>iconv<br>
</li><li>apache-ant<br>
The recommended way to install is using <a href='http://www.macports.org'>MacPorts</a>.<br>
To compile source code use following commands:<br>
<pre><code>       make -C mac<br>
       ant<br>
</code></pre></li></ol>

<h2>Running Example</h2>
Running commando '<i>ant run</i>' will print you list of currently connected HID devices.<br>After this it would run and print notifications about newly plugged or unplugged HID devices.<br>To stop the program press <i>Ctrl^C</i>.<br>
<br>
<h2>Java API Overview</h2>
JavaHID API provides simple cross-platform Java API for working with HID devices. For full API reference please see <i>doc/api</i> folder in source distribuiton. All API classes are contained in <i>com.codeminders.hidapi</i> package.<br>
<br>
Before using API you must subclass <i>HIDManager</i>. You need to implement two callback methods: <i>deviceAdded</i> and <i>deviceRemoved</i>. These methods are called in separate thread when new HID<br>
devices are added or removed from the system. If you are not interested in receiving device addition/removal notifications then empty implementation will suffice. <i>HIDManagerTest</i> class included in source distribution is a sample implementation which prints notification messages on<br>
standard output.<br>
<br>
After instantiating an instance of <i>HIDManager</i> subclass you can use some of its methods:<br>

The method <i>listDevices</i> returns list of currently found HID devices. Each device is represented by<br>
<i>HIDDeviceInfo</i> class instance which contains information about the devices. Please refer to API<br>
documentation for list of fields. This datat stucture is merely an information about device, to open<br>
it you need to call <i>open()</i> method on it. <br>

<i>HIDManager</i> also provides a couple of convenience methods for quickly finding and opening<br>
devices either by path (<i>openByPath</i>) or by vendor id/product id/serial number (<i>openById</i>).<br>
<br>
Each open device is represented by <i>HIDDevice</i> instance. If device is opened multiple times the returned instances of <i>HIDDevice</i> wil be equals (using Java <i>equals</i> method). However thread-safety is not guaranteed so you need to serialize access to it using standard Java synchronization primitives. <br>Please see API reference for details on methods of this class.