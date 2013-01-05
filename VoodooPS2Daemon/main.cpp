//
//  main.cpp
//  VoodooPS2Daemon
//
//  Created by RehabMan on 1/5/13.
//  Copyright (c) 2013 RehabMan. All rights reserved.
//
//  The purpose of this daemon is to watch for USB mice being connected or disconnected from
//  the system.  This done by monitoring changes to the ioreg.
//
//  When changes in the status are detected, this information is sent to the trackpad
//  driver through a ioreg property. When the trackpad driver sees the chagnes to the property it
//  can decide to enable or disable the trackpad as appropriate.
//
//  This code was loosely based on "Another USB Notification Example" at:
//  http://www.opensource.apple.com/source/IOUSBFamily/IOUSBFamily-540.4.1/Examples/Another%20USB%20Notification%20Example/
//

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach.h>
#include <unistd.h>

// notification data for IOServiceAddInterestNotification
typedef struct NotificationData
{
    io_object_t	notification;
} NotificationData;

static IONotificationPortRef g_NotifyPort;
static io_iterator_t g_AddedIter;
static int g_MouseCount = 0;

// DeviceNotification
//
// This function deals with IOService nodes we previously expressed an interest
// in because they were USB IOHIDPointing nodes
// This is used to keep track of USB mice getting terminated

static void DeviceNotification(void* refCon, io_service_t service, natural_t messageType, void* messageArgument)
{
    NotificationData* pData = (NotificationData*)refCon;
    if (kIOMessageServiceIsTerminated == messageType)
    {
        if (g_MouseCount)
            --g_MouseCount;
        printf("mouse count is now: %d\n", g_MouseCount);
        IOObjectRelease(pData->notification);
        free(pData);
    }
}


// DeviceAdded
//
// This function deals with USB devices as they are connected.  We look for devices
// that have child nodes of type IOHIDPointing devices because those are USB mice.

static void DeviceAdded(void *refCon, io_iterator_t iter1)
{
    io_service_t service;
    while ((service = IOIteratorNext(iter1)))
    {
        io_iterator_t iter2;
        kern_return_t kr = IORegistryEntryCreateIterator(service, kIOServicePlane, kIORegistryIterateRecursively, &iter2);
        if (KERN_SUCCESS != kr)
        {
            printf("IORegistryEntryCreateIterator returned 0x%08x\n", kr);
            continue;
        }
        
        io_service_t temp;
        while ((temp = IOIteratorNext(iter2)))
        {
            io_name_t name;
            kr = IORegistryEntryGetName(temp, name);
            if (KERN_SUCCESS != kr)
                continue;
            if (0 == strcmp("IOHIDPointing", name))
            {
                NotificationData* pData = (NotificationData*)malloc(sizeof(*pData));
                if (pData == NULL)
                    continue;
                kr = IOServiceAddInterestNotification(g_NotifyPort, temp, kIOGeneralInterest, DeviceNotification, pData, &pData->notification);
                if (KERN_SUCCESS != kr)
                {
                    printf("IOServiceAddInterestNotification returned 0x%08x\n", kr);
                    continue;
                }
                ++g_MouseCount;
                printf("mouse count is now: %d\n", g_MouseCount);
            }
            kr = IOObjectRelease(temp);
        }
        kr = IOObjectRelease(service);
    }
}

// SignalHandler
//
// Deal with being terminated.  We might be able to eventually use this to turn off the
// trackpad LED as the system is shutting down.

static void SignalHandler1(int sigraised)
{
    printf("\nInterrupted\n");
    
    // Clean up here
    IONotificationPortDestroy(g_NotifyPort);
    
    if (g_AddedIter)
    {
        IOObjectRelease(g_AddedIter);
        g_AddedIter = 0;
    }
    
    // exit(0) should not be called from a signal handler.  Use _exit(0) instead
    _exit(0);
}

// main
//
// Entry point from command line or (eventually) launchd LaunchDaemon
//

int main (int argc, const char *argv[])
{
    // Set up a signal handler so we can clean up when we're interrupted from the command line
    // or otherwise asked to terminate.
    if (SIG_ERR == signal(SIGINT, SignalHandler1))
        printf("Could not establish new SIGINT handler\n");
    if (SIG_ERR == signal(SIGTERM, SignalHandler1))
        printf("Could not establish new SIGTERM handler\n");
    
    // First create a master_port for my task
    mach_port_t masterPort;
    kern_return_t kr = IOMasterPort(MACH_PORT_NULL, &masterPort);
    if (kr || !masterPort)
    {
        printf("ERR: Couldn't create a master IOKit Port(%08x)\n", kr);
        return -1;
    }
    
    // Create dictionary to match all USB devices
    CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matchingDict)
    {
        printf("Can't create a USB matching dictionary\n");
        mach_port_deallocate(mach_task_self(), masterPort);
        return -1;
    }
    
    // Create a notification port and add its run loop event source to our run loop
    // This is how async notifications get set up.
    g_NotifyPort = IONotificationPortCreate(masterPort);
    CFRunLoopSourceRef runLoopSource = IONotificationPortGetRunLoopSource(g_NotifyPort);
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(runLoop, runLoopSource, kCFRunLoopDefaultMode);
    
    // Now set up a notification to be called when a device is first matched by I/O Kit.
    // Note that this will not catch any devices that were already plugged in so we take
    // care of those later.
    kr = IOServiceAddMatchingNotification(g_NotifyPort, kIOFirstMatchNotification, matchingDict, DeviceAdded, NULL, &g_AddedIter);
    
    // Iterate once to get already-present devices and arm the notification
    DeviceAdded(NULL, g_AddedIter);
    
    // Now done with the master_port
    mach_port_deallocate(mach_task_self(), masterPort);
    masterPort = 0;
    
    // Start the run loop. Now we'll receive notifications.
    CFRunLoopRun();
    
    // We should never get here
    printf("Unexpectedly back from CFRunLoopRun()!\n");
    
    return 0;
}

