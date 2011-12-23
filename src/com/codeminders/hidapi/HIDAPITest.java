package com.codeminders.hidapi;

import java.io.IOException;


/**
 * This class demonstrates enumeration, reading and getting
 * notifications when a HID device is connected/disconnected.
 */
public class HIDAPITest
{
    private static final long READ_UPDATE_DELAY_MS = 50L;

    static
    {
        System.loadLibrary("hidapi-jni");
    }

    // "Afterglow" controller for PS3
    static final int VENDOR_ID = 3695;
    static final int PRODUCT_ID = 25346;
    private static final int BUFSIZE = 2048;
    private static final int MAXOBJECTS = 100;
        
    /**
     * @param args input strings value.
     */
    public static void main(String[] args) throws IOException
    {
        try
        {
            HIDManager test;
            test  = new HIDManagerTest();
            listDevices();
           //readDevice();
            System.err.println("waiting connect/disconnect...");
            while(true)
            {
                try
                {
                    Thread.sleep(1000L);
                } catch(InterruptedException e)
                {
                    e.printStackTrace();
                }
            }
       }
       catch(IOException e)
       {
          e.printStackTrace();
       }
    }
    
    /**
     * Static function to read an input report to a HID device.
     */
    private static void readDevice()
    {
        HIDDevice dev;
        HIDManager hid_mgr;
        try
        {
            hid_mgr = new HIDManagerTest();
            dev = hid_mgr.openById(VENDOR_ID, PRODUCT_ID, null);
            System.err.print("Manufacturer: " + dev.getManufacturerString() + "\n");
            System.err.print("Product: " + dev.getProductString() + "\n");
            System.err.print("Serial Number: " + dev.getSerialNumberString() + "\n");
            try
            {
                byte[] buf = new byte[BUFSIZE];
                dev.enableBlocking();
                while(true)
                {
                    int n = dev.read(buf);
                    for(int i=0; i<n; i++)
                    {
                        int v = buf[i];
                        if (v<0) v = v+256;
                        String hs = Integer.toHexString(v);
                        if (v<16) 
                            System.err.print("0");
                        System.err.print(hs + " ");
                    }
                    System.err.println("");
                    
                    try
                    {
                        Thread.sleep(READ_UPDATE_DELAY_MS);
                    } catch(InterruptedException e)
                    {
                        //Ignore
                        e.printStackTrace();
                    }
                }
            } finally
            {
                dev.close();
                hid_mgr.release();    
                System.gc();
            }
            
        } 
        catch(IOException e)
        {
            e.printStackTrace();
        }
    }
    
    /**
     * Static function to find the list of all the HID devices
     * attached to the system.
     */
    private static void listDevices()
    {
        String property = System.getProperty("java.library.path");
        System.err.println(property);
        HIDManager hid_mgr;
        try
        {
            hid_mgr = new HIDManagerTest();
            
            HIDDeviceInfo[] devs = hid_mgr.listDevices();
            System.err.println("Devices:\n\n");
            for(int i=0;i<devs.length;i++)
            {
                System.err.println(""+i+".\t"+devs[i]);
                System.err.println("---------------------------------------------\n");
            }
            hid_mgr.release();
            System.gc();
        }
        catch(IOException e)
        {
            System.err.println(e.getMessage());
            e.printStackTrace();
        }
    }

}
