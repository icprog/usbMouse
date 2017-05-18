/****************************************************************************
 * Copyright (c) 2011 Lawrence Berkeley National Laboratory,                *
 * Accelerator Technology Group, Engineering Division                       *
 * This code is distributed subject to a Software License Agreement found   *
 * in file LICENSE.txt that is included with this distribution.             *
 ****************************************************************************/

/*
 * Example showing how to read from a USB mouse
 *
 * Author: W. Eric Norum
 */

#include <string.h>
#include <epicsStdio.h>
#include <epicsString.h>
#include <epicsThread.h>
#include <epicsExport.h>
#include <errlog.h>
#include <cantProceed.h>
#include <iocsh.h>
#include <asynDriver.h>
#include <asynInt32.h>
#include <libusb-1.0/libusb.h>


/*
 * Define this to non-zero to get lengthy ASYN reports
 */
#define ASYN_LONG_REPORTS 1

/* Conventional codes for class-specific descriptors.  The convention is
 * defined in the USB "Common Class" Spec (3.11).  Individual class specs
 * are authoritative for their usage, not the "common class" writeup.
 */
#define USB_DT_CS_DEVICE       (LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_DT_DEVICE)
#define USB_DT_CS_INTERFACE    (LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_DT_INTERFACE)

/*
 * USB Setup Packet values
 * These are gleaned from HID1_11.pdf section 7.2.1 "Get_Report Request":
 *      bmRequestType       10100001
 *      bRequest            00000001 (GET_REPORT)
 *      wValue              Report type in high byte, report ID in low byte
 *      wIndex              Interface
 *      wLength             Report length
 *
 * The GET_REPORT request allows the host to receive a report via the
 * CONTROL pipe.  A report type of 1 is 'INPUT'.
 */

/*
 * bmRequestType bits
 */
#define USB_TYPE_CLASS          (0x01 << 5) /* Class device request */
#define USB_RECIP_INTERFACE     0x01        /* Recepient is interface */

/*
 * bRequest values for HID class
 */
#define HID_REPORT_GET          0x01

/*
 * wValue bits (report type is high byte)
 */
#define HID_RT_INPUT            0x01

/*
 * How long to wait for response (milliseconds)
 */
#define USB_TIMEOUT             10000

/*
 * Mouse values
 */
typedef struct mouseValues {
    int buttons;
    int xPosition;
    int yPosition;
    int wheel;
} mouseValues;

/*
 * Driver private storage
 */
typedef struct drvPvt {
    char                           *portName;

    /*
     * Asyn interfaces
     */
    asynInterface                   asynCommon;
    asynInterface                   asynInt32;
    void                           *asynInt32InterruptPvt;

    /*
     * Control diagnostic messages
     */
    asynUser                       *pasynUserForMessages;

    /*
     * Device information
     */
    int                             idVendor;
    int                             idProduct;
    int                             idNumber;

    /*
     * libusb-1.0
     */
    libusb_context                 *usbContext;
    libusb_device_handle           *usbHandle;
    struct libusb_device_descriptor usbDeviceDescriptor;
    struct libusb_config_descriptor *usbConfigp;
    int                             isConnected;

    /*
     * Data from mouse
     */
    unsigned char                   cbuf[80];
    int                             nRead;
    mouseValues                     oldMouse;
    mouseValues                     newMouse;
    char                           *manufacturerString;
    char                           *productString;
    char                           *serialNumberString;
    int                             HIDreportLength;
    unsigned char                  *HIDreport;

    /*
     * Reader thread info
     */
    double                          pollInterval;
    int                             useDevicePollInterval;
    unsigned long                   packetCount;
    int                             transferDone;
} drvPvt;

/*
 * Sign-extend
 */
static int
signExtend(int size, int value)
{
    switch(size) {
    default:                           break;
    case 1: value = (epicsInt8)value;  break;
    case 2: value = (epicsInt16)value; break;
    case 4: value = (epicsInt32)value; break;
    }
    return value;
}

#if ASYN_LONG_REPORTS
/*
 *****************************************************
 * These routines are present only to provide device *
 * information for the ASYN report method.           *
 *****************************************************
 */
/*
 * Get a report of the HID values
 */
static void
getHIDreport(drvPvt *pdpvt,
             const struct libusb_interface_descriptor *interface,
             const unsigned char *buf)
{
    int s;

    if (pdpvt->HIDreport)
        free(pdpvt->HIDreport);
    pdpvt->HIDreportLength = (buf[8] << 8) | buf[7];
    pdpvt->HIDreport = callocMustSucceed(pdpvt->HIDreportLength, 1, "getHIDreport");
    s = libusb_control_transfer(pdpvt->usbHandle,
                        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD |
                                             LIBUSB_RECIPIENT_INTERFACE,
                        LIBUSB_REQUEST_GET_DESCRIPTOR,
                        (LIBUSB_DT_REPORT << 8) | 0x00,
                        interface->bInterfaceNumber,
                        pdpvt->HIDreport, pdpvt->HIDreportLength, USB_TIMEOUT);
    if (s != pdpvt->HIDreportLength) {
        asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
                                            "Get HID report failed: %d\n", s);
        free(pdpvt->HIDreport);
        pdpvt->HIDreport = NULL;
        pdpvt->HIDreportLength = 0;
    }
}

/*
 * Show HID report
 */
static void
showHIDreport(FILE *fp, drvPvt *pdpvt)
{
    int i, j, indent = 0; 
    int dSize, bSize, bTag, bType, data, hidUsagePage = 0;
    static const char *const types[4] = { "Main", "Global", "Local", "Reserved" };

    for (i = 0 ; i < pdpvt->HIDreportLength ; i += dSize + bSize) {
        bTag = pdpvt->HIDreport[i];
        bSize = bTag & 0x3;
        if (bSize == 3) bSize = 4;
        bType = (bTag >> 2) & 0x3;
        bTag = bTag & ~0x3;
        if (bTag == 0xF) {
            dSize = 3;
            bSize = pdpvt->HIDreport[i+1];
        }
        else {
            dSize = 1;
            data = 0;
            for (j = 0 ; j < bSize ; j++)
                data += pdpvt->HIDreport[i+dSize+j] << (j * 8);
            if ((bTag == 0xC0) && (indent != 0))
                indent--;
            fprintf(fp, "           %8s  %*s", types[bType], indent * 3, "");
            switch (bTag) {
            /*
             * Main Items
             */
            case 0x80:
                fprintf(fp, "Input: %s, %s, %s, %s, %s, %s, %s, %s",
                        data & 0x001 ? "Constant"  : "Data",
                        data & 0x002 ? "Variable"  : "Array",
                        data & 0x004 ? "Relative"  : "Absolute",
                        data & 0x008 ? "Wrap"      : "No wrap",
                        data & 0x010 ? "Nonlinear" : "Linear",
                        data & 0x020 ? "No preferred state" : "Preferred state",
                        data & 0x040 ? "Null state" : "No null position",
                        data & 0x100 ? "Buffered bytes"  : "Bitfield");
                break;

            case 0xA0:
                fprintf(fp, "Collection: ");
                switch (data) {
                case 0x00:  fprintf(fp, "Physical (group of axes)");     break;
                case 0x01:  fprintf(fp, "Applcation (mouse, keyboard)"); break;
                case 0x02:  fprintf(fp, "Logical (interrelated data)");  break;
                case 0x03:  fprintf(fp, "Report");                       break;
                case 0x04:  fprintf(fp, "Named array");                  break;
                case 0x05:  fprintf(fp, "Usage switch");                 break;
                case 0x06:  fprintf(fp, "Usage modifier");               break;
                default:
                    if (data <= 0x7F)
                        fprintf(fp, "Reserved %#X", data);
                    else
                        fprintf(fp, "Vendor-defined %#X", data);
                    break;
                }
                indent++;
                break;

            case 0xB0:
                fprintf(fp, "Feature: %s, %s, %s, %s, %s, %s, %s, %s, %s",
                        data & 0x001 ? "Constant"  : "Data",
                        data & 0x002 ? "Variable"  : "Array",
                        data & 0x004 ? "Relative"  : "Absolute",
                        data & 0x008 ? "Wrap"      : "No wrap",
                        data & 0x010 ? "Nonlinear" : "Linear",
                        data & 0x020 ? "No preferred state" : "Preferred state",
                        data & 0x040 ? "Null state" : "No null position",
                        data & 0x080 ? "Volatile"  : "Non-volatile",
                        data & 0x100 ? "Buffered bytes"  : "Bitfield");
                break;

            case 0xC0:
                fprintf(fp, "End of collection");
                break;

            /*
             * Global Items
             */
            case 0x04:
                fprintf(fp, "Usage page %4.4X", data);
                hidUsagePage = data;
                break;

            case 0x14:
                fprintf(fp, "Logical minimum %d", signExtend(bSize, data));
                break;

            case 0x24:
                fprintf(fp, "Logical maximum %d", signExtend(bSize, data));
                break;

            case 0x34:
                fprintf(fp, "Physical minimum %d", signExtend(bSize, data));
                break;

            case 0x44:
                fprintf(fp, "Physical maximum %d", signExtend(bSize, data));
                break;

            case 0x54:
                fprintf(fp, "Unit exponent %d", data);
                break;

            case 0x64:
                fprintf(fp, "Unit %d", data);
                break;

            case 0x74:
                fprintf(fp, "Report size %d", data);
                break;

            case 0x84:
                fprintf(fp, "Report ID %d", data);
                break;

            case 0x94:
                fprintf(fp, "Report count %d", data);
                break;

            case 0xA4:
                fprintf(fp, "PUSH");
                break;

            case 0xB4:
                fprintf(fp, "POP");
                break;

            /*
             * Local Items
             */
            case 0x08:
                fprintf(fp, "Usage index %d", data);
                break;

            case 0x18:
                fprintf(fp, "Usage minimum %d", data);
                break;

            case 0x28:
                fprintf(fp, "Usage maximum %d", data);
                break;

            /*
             * Catch-all
             */
            default:
                fprintf(fp, "Tag %x data:%*.*X", bTag, bSize*2, bSize*2, data);
                break;
            }
            fprintf(fp, "\n");
                
        }
    }
}
#endif /* ASYN_LONG_REPORTS */

/*
 * Get a string descriptor from the device
 * This routine isn't strictly necessary either, but it does provide
 * useful information so I made it non-conditional.
 */
static asynStatus
getStringDescriptor(drvPvt *pdpvt, unsigned int descriptor, char **cpp)
{
    unsigned char cbuf[255];
    char value[128];
    int i, j, s;
    int languageCode;

    if (descriptor == 0) {
        *cpp = epicsStrDup("???");
        return asynSuccess;
    }
    if (descriptor > 255) {
        epicsSnprintf(value, sizeof value, "Invalid desriptor (%d)", descriptor);
        *cpp = epicsStrDup(value);
        return asynError;
    }

    /*
     * Get the first supported language
     */
    s = libusb_control_transfer(pdpvt->usbHandle,
          LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
          LIBUSB_REQUEST_GET_DESCRIPTOR,
          (LIBUSB_DT_STRING << 8) | 0x00,  /* Index 0 (language identifiers) */
          0x0000,  /* Interface number */
          cbuf, sizeof cbuf, USB_TIMEOUT);
    if (s <= 0) {
        epicsSnprintf(value, sizeof value, "Can't get language descriptor");
        *cpp = epicsStrDup(value);
        return asynError;
    }
    languageCode = (cbuf[3] << 8) | cbuf[2];

    /*
     * Get the string in that language
     */
    s = libusb_control_transfer(pdpvt->usbHandle,
         LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
          LIBUSB_REQUEST_GET_DESCRIPTOR,
         (LIBUSB_DT_STRING << 8) | descriptor,
         languageCode,
         cbuf, sizeof cbuf, USB_TIMEOUT);
    if (s <= 0) {
        epicsSnprintf(value, sizeof value, "Can't get descriptor %d", descriptor);
        *cpp = epicsStrDup(value);
        return asynError;
    }

    /*
     * Assume string is in ASCII subset of Unicode
     */
    for (i = 2, j = 0 ; (i < cbuf[0]) && (j < sizeof(value) - 1) ; i += 2, j++)
        value[j] = cbuf[i];
    value[j] = '\0';
    *cpp = epicsStrDup(value);
    return asynSuccess;
}

/*
 * Try to connect to the mouse
 */
static asynStatus
connectToMouse(drvPvt *pdpvt)
{
    libusb_device **list;
    libusb_device *found = NULL;
    ssize_t n;
    int i, s;
    const struct libusb_interface_descriptor *interface;
    const struct libusb_endpoint_descriptor *endpoint;

    /*
     * Find the device
     */
    n = libusb_get_device_list(NULL, &list);
    if (n < 0) {
        asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
                                "libusb_get_device_list failed: %d\n", (int)n);
        return asynError;
    }
    for (i = 0 ; i < n ; i++) {
        libusb_device *device = list[i];
        int s = libusb_get_device_descriptor(device, &pdpvt->usbDeviceDescriptor);
        if (s != 0) {
            asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
                                "libusb_get_device_descriptor failed: %d\n", s);
            return asynError;
        }
        if ((pdpvt->usbDeviceDescriptor.idVendor == pdpvt->idVendor)
         && (pdpvt->usbDeviceDescriptor.idProduct == pdpvt->idProduct)) {
            found = device;
            break;
        }
    }
    if (!found) {
        asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
            "Can't find device with vendor ID:%4.4X and product ID:%4.4X.\n",
                                         pdpvt->idVendor,  pdpvt->idProduct);
        return asynError;
    }

    /*
     * Open a connection to the device
     */
    s = libusb_open(found, &pdpvt->usbHandle);
    if (s != 0) {
        asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
                                                "libusb_open failed: %d\n", s);
        return 1;
    }
    libusb_free_device_list(list, 1);
    s = libusb_kernel_driver_active(pdpvt->usbHandle, pdpvt->idNumber);
    if (s == 1) {
        s = libusb_detach_kernel_driver(pdpvt->usbHandle, pdpvt->idNumber);
        if (s != 0) {
            asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
                    "Warning -- libusb_detach_kernel_driver failed: %d\n", s);
        }
    }
    else if (s != 0) {
        asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
                                "libusb_kernel_driver_active failed: %d\n", s);
        return asynError;
    }
    s = libusb_claim_interface(pdpvt->usbHandle, pdpvt->idNumber);
    if (s != 0) {
        asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
                           "Warning -- libusb_claim_interface failed: %d\n", s);
    }

    /*
     * Get device information
     */
    if (pdpvt->usbConfigp != NULL)
        libusb_free_config_descriptor(pdpvt->usbConfigp);
    libusb_get_config_descriptor(found, 0, &pdpvt->usbConfigp);
    interface = pdpvt->usbConfigp->interface->altsetting;
    endpoint = interface->endpoint;
    if (pdpvt->useDevicePollInterval)
        pdpvt->pollInterval = 125.0e-6 * (1 << (endpoint->bInterval - 1));
    if (interface->bInterfaceClass == LIBUSB_CLASS_HID) {
#if ASYN_LONG_REPORTS
        const unsigned char *buf = interface->extra;
        if ((interface->extra_length >= 9)
         && (interface->extra_length >= buf[0])
         && (buf[1] == LIBUSB_DT_HID)
         && (buf[5] >= 1)
         && (buf[6] == LIBUSB_DT_REPORT)) {
            getHIDreport(pdpvt, interface, buf);
        }
#endif
    }
    else {
        asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
                "Interface class (%d) is not LIBUSB_CLASS_HID (%d)\n",
                         interface->bInterfaceClass, LIBUSB_CLASS_HID);
    }
    getStringDescriptor(pdpvt, pdpvt->usbDeviceDescriptor.iManufacturer, &pdpvt->manufacturerString);
    getStringDescriptor(pdpvt, pdpvt->usbDeviceDescriptor.iProduct, &pdpvt->productString);
    getStringDescriptor(pdpvt, pdpvt->usbDeviceDescriptor.iSerialNumber, &pdpvt->serialNumberString);

    /*
     * All connected and ready to go
     */
    pdpvt->transferDone = 0;
    pdpvt->isConnected = 1;
    return asynSuccess;
}

/*
 * Stuff data into records and trigger record processing.
 */
static void
transferStatus(drvPvt *pdpvt)
{
    ELLLIST *pclientList;
    interruptNode *pnode;
    int changedButtons = pdpvt->newMouse.buttons ^ pdpvt->oldMouse.buttons;

    pasynManager->interruptStart(pdpvt->asynInt32InterruptPvt, &pclientList);
    pnode = (interruptNode *)ellFirst(pclientList);
    while (pnode) {
        asynInt32Interrupt *int32Interrupt = pnode->drvPvt;
        if ((int32Interrupt->addr >= 0) && (int32Interrupt->addr <= 7)) {
            int bit = 1 << int32Interrupt->addr;
            if (((changedButtons & bit) != 0)
             || (pdpvt->transferDone == 0))
                int32Interrupt->callback(int32Interrupt->userPvt,
                                         int32Interrupt->pasynUser,
                                         ((pdpvt->newMouse.buttons&bit)!=0));
        }
        else if ((int32Interrupt->addr >= 10) && (int32Interrupt->addr <= 12)) {
            int newValue = 0, oldValue = 0;
            switch (int32Interrupt->addr) {
            case 10: newValue = pdpvt->newMouse.xPosition;
                     oldValue = pdpvt->oldMouse.xPosition;
                     break;
            case 11: newValue = pdpvt->newMouse.yPosition;
                     oldValue = pdpvt->oldMouse.yPosition;
                     break;
            case 12: newValue = pdpvt->newMouse.wheel;
                     oldValue = pdpvt->oldMouse.wheel;
                     break;
            }
            if ((newValue != oldValue)
             || (pdpvt->transferDone == 0))
                int32Interrupt->callback(int32Interrupt->userPvt,
                                         int32Interrupt->pasynUser,
                                         newValue);
        }
        else if (pdpvt->transferDone == 0) {
            errlogPrintf("WARNING -- BAD USB MOUSE ASYN ADDRESSS %d\n",
                                                        int32Interrupt->addr);
        }
        pnode = (interruptNode *)ellNext(&pnode->node);
    }
    pasynManager->interruptEnd(pdpvt->asynInt32InterruptPvt);
    pdpvt->oldMouse = pdpvt->newMouse;
    pdpvt->transferDone = 1;
}

/*
 * This thread soaks up reads from the mouse
 */
static void
readerThread(void *arg)
{
    drvPvt *pdpvt = arg;
    int s;
    extern volatile int interruptAccept;

    for (;;) {
        if (!pdpvt->isConnected) {
            epicsThreadSleep(10.0);
            if (connectToMouse(pdpvt) != asynSuccess)
                continue;
        }
        for (;;) {
            s = libusb_control_transfer(pdpvt->usbHandle,
                    LIBUSB_ENDPOINT_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                    HID_REPORT_GET,
                    (HID_RT_INPUT << 8) | 0x00,
                    pdpvt->idNumber,
                    pdpvt->cbuf, sizeof pdpvt->cbuf, USB_TIMEOUT);
            if (s <= 0) {
                asynPrint(pdpvt->pasynUserForMessages, ASYN_TRACE_ERROR, 
                                "libusb_control_transfer failed: %d\n", s);
                libusb_close(pdpvt->usbHandle);
                pdpvt->isConnected = 0;
                break;
            }
            pdpvt->nRead = s;
            if (s > 0) pdpvt->newMouse.buttons = pdpvt->cbuf[0];
            if (s > 1) pdpvt->newMouse.xPosition += signExtend(1, pdpvt->cbuf[1]);
            if (s > 2) pdpvt->newMouse.yPosition += signExtend(1, pdpvt->cbuf[2]);
            if (s > 3) pdpvt->newMouse.wheel += signExtend(1, pdpvt->cbuf[3]);
            asynPrintIO(pdpvt->pasynUserForMessages, ASYN_TRACEIO_DRIVER, 
                    (char *)pdpvt->cbuf, pdpvt->nRead, "Read %d", pdpvt->nRead);
            if (interruptAccept)
                transferStatus(pdpvt);
            pdpvt->packetCount++;
            epicsThreadSleep(pdpvt->pollInterval);
        }
    }
}


/*
 * asynCommon methods
 */
static void
report(void *pvt, FILE *fp, int details)
{
    drvPvt *pdpvt = (drvPvt *)pvt;
    const struct libusb_interface_descriptor *interface;

    interface = pdpvt->usbConfigp->interface->altsetting;
    if (details >= 1) {
        fprintf(fp, "          Vendor ID: 0x%4.4X\n", pdpvt->idVendor);
        fprintf(fp, "         Product ID: 0x%4.4X\n", pdpvt->idProduct);
        fprintf(fp, "   Interface number: %d\n", pdpvt->idNumber);
        fprintf(fp, "      Poll interval: %.3g ms\n", pdpvt->pollInterval * 1000);
        fprintf(fp, "    Maximum current: %d mA\n", pdpvt->usbConfigp->MaxPower * 2);
    }

#if ASYN_LONG_REPORTS
    if (details >= 1) {
        fprintf(fp, "       Manufacturer: \"%s\"\n", pdpvt->manufacturerString);
        fprintf(fp, "            Product: \"%s\"\n", pdpvt->productString);
        fprintf(fp, "      Serial number: \"%s\"\n", pdpvt->serialNumberString);
    }
    if (details >= 2) {
        int i;
        const struct libusb_endpoint_descriptor *endpoint = interface->endpoint;
        if (interface->bInterfaceClass == LIBUSB_CLASS_HID) {
            const unsigned char *buf;
            buf = interface->extra;
            if (buf[1] != LIBUSB_DT_HID) {
                fprintf(fp, "     Descriptor %#x is not LIBUSB_DT_HID (%#x)\n",
                                                        buf[1], LIBUSB_DT_HID);
            }
            else if ((interface->extra_length < 9)
                  || (interface->extra_length < buf[0])) {
                fprintf(fp, "     Extra length %x is not %d\n",
                                            interface->extra_length, buf[0]);
            }
            else if (details >= 2) {
                fprintf(fp, "           HID Code: %2.2X.%2.2X\n", buf[3], buf[2]);
                fprintf(fp, "   HID Country Code: %d%s\n", buf[4],
                                            buf[4] ? "" : " (Non-localized)");
                fprintf(fp, "  HID # Descriptors: %d\n", buf[5]);
                fprintf(fp, "  HID Report Length: %d\n", pdpvt->HIDreportLength);
                if (pdpvt->HIDreport)
                    showHIDreport(fp, pdpvt);
            }
        }
        for (i = 0 ; i < interface->bNumEndpoints ; i++, endpoint++) {
            fprintf(fp, "   Endpoint descriptor:\n");
            if (endpoint->bLength != 7)  {
                fprintf(fp, "         Endpoint length is %d, expect 7.\n", endpoint->bLength);
            }
            else if (endpoint->bDescriptorType != 5) {
                fprintf(fp, "         Endpoint bDescriptorType is %d, expect 5.\n", endpoint->bDescriptorType);
            }
            else {
                static const char *const transferTypes[] = { "Control",
                                                            "Isochronous",
                                                            "Bulk",
                                                            "Interrupt" };
                static const char *const synchronizationTypes[] = {
                                                            "None",
                                                            "Asynchronous",
                                                            "Adaptive",
                                                            "Sycnhronous" };
                static const char *const usageTypes[] = {
                                                    "Data",
                                                    "Feedback",
                                                    "Data (Implicit feedback)",
                                                    "3 (Reserved)" };
                fprintf(fp, "              Endpoint: %d (%s)\n",
                            endpoint->bEndpointAddress & 0xF,
                            endpoint->bEndpointAddress & 0x80 ? "IN" : "OUT");
                fprintf(fp, "                  Type: %s\n",
                    transferTypes[endpoint->bmAttributes & 0x3]);
                fprintf(fp, "       Synchronization: %s\n",
                    synchronizationTypes[(endpoint->bmAttributes >> 2) & 0x3]);
                fprintf(fp, "                 Usage: %s\n",
                    usageTypes[(endpoint->bmAttributes >> 4) & 0x3]);
                fprintf(fp, "       Max packet size: %d\n", endpoint->wMaxPacketSize);
                fprintf(fp, "             bInterval: %d (%.3g ms)\n",
                                endpoint->bInterval,
                                125.0e-3 * (1 << (endpoint->bInterval - 1)));
            }
        }
    }
#endif /* ASYN_LONG_REPORTS */

    if (details >= 3) {
        fprintf(fp, "       Packet Count: %lu\n", pdpvt->packetCount);
    }
    if (details >= 4) {
        int i;
        fprintf(fp, "    ");
        for (i = 0 ; i < pdpvt->nRead ; i++)
            fprintf(fp, " %2.2X", pdpvt->cbuf[i]);
        fprintf(fp, "\n");
    }
}

static asynStatus
connect(void *pvt, asynUser *pasynUser)
{
    pasynManager->exceptionConnect(pasynUser);
    return asynSuccess;
}

static asynStatus
disconnect(void *pvt, asynUser *pasynUser)
{
    pasynManager->exceptionDisconnect(pasynUser);
    return asynSuccess;
}
static asynCommon commonMethods = { report, connect, disconnect };

/*
 * asynInt32 methods
 * There are none!
 * Everything is handled with interrupt callbacks
 */
static asynInt32 int32Methods;

static void
usbMouseConfigure(const char *portName, int idVendor, int idProduct,
                  int idNumber, int interval, int priority)
{
    drvPvt *pdpvt;
    asynStatus status;
    epicsThreadId tid;
    char *threadName;

    /*
     * Handle defaults
     */
    if (priority <= 0) priority = epicsThreadPriorityMedium;

    /*
     * Set up local storage
     */
    pdpvt = (drvPvt *)callocMustSucceed(1, sizeof(drvPvt), portName);
    pdpvt->portName = epicsStrDup(portName);
    if (interval <= 0)
        pdpvt->useDevicePollInterval = 1;
    else
        pdpvt->pollInterval = interval / 1000.0;

    /*
     * Create our port (autoconnect)
     */
    status = pasynManager->registerPort(pdpvt->portName,
                                        ASYN_CANBLOCK|ASYN_MULTIDEVICE,
                                        1, 0, 0);
    if (status != asynSuccess) {
        printf("registerPort failed\n");
        return;
    }
    pdpvt->asynCommon.interfaceType = asynCommonType;
    pdpvt->asynCommon.pinterface  = &commonMethods;
    pdpvt->asynCommon.drvPvt = pdpvt;
    status = pasynManager->registerInterface(pdpvt->portName, &pdpvt->asynCommon);
    if (status != asynSuccess) {
        printf("registerInterface failed\n");
        return;
    }
    pdpvt->asynInt32.interfaceType = asynInt32Type;
    pdpvt->asynInt32.pinterface  = &int32Methods;
    pdpvt->asynInt32.drvPvt = pdpvt;
    status = pasynInt32Base->initialize(pdpvt->portName, &pdpvt->asynInt32);
    if (status != asynSuccess) {
        printf("pasynInt32Base->initialize failed\n");
        return;
    }
    pasynManager->registerInterruptSource(pdpvt->portName, &pdpvt->asynInt32,
                                                &pdpvt->asynInt32InterruptPvt);

    /*
     * Set up dummy asynUser for controlling diagnostic messages
     */
    pdpvt->pasynUserForMessages = pasynManager->createAsynUser(NULL, NULL);
    status = pasynManager->connectDevice(pdpvt->pasynUserForMessages,
                                         pdpvt->portName ,2000);
    if (status != asynSuccess)
        printf("Warning -- can't set up diagnostic messsage pasynUser!");

    /*
     * Try connecting
     */
    pdpvt->idVendor = idVendor;
    pdpvt->idProduct = idProduct;
    libusb_init(&pdpvt->usbContext);
    connectToMouse(pdpvt);

    /*
     * Start the reader thread.
     */
    threadName = callocMustSucceed(strlen(portName)+20, 1, portName);
    sprintf(threadName, "%s_READER", portName);
    tid = epicsThreadCreate(threadName,
                            priority,
                            epicsThreadGetStackSize(epicsThreadStackMedium),
                            readerThread,
                            pdpvt);
    if (!tid) {
        printf("Can't set up %s thread!\n", threadName);
        return;
    }
    free(threadName);
}

/*
 * IOC shell command registration
 */
static const iocshArg usbMouseConfigureArg0 = { "port",iocshArgString};
static const iocshArg usbMouseConfigureArg1 = { "vendor ID",iocshArgInt};
static const iocshArg usbMouseConfigureArg2 = { "product ID",iocshArgInt};
static const iocshArg usbMouseConfigureArg3 = { "device number",iocshArgInt};
static const iocshArg usbMouseConfigureArg4 = { "poll interval(ms)",iocshArgInt};
static const iocshArg usbMouseConfigureArg5 = { "priority",iocshArgInt};
static const iocshArg *usbMouseConfigureArgs[] = {
                    &usbMouseConfigureArg0, &usbMouseConfigureArg1,
                    &usbMouseConfigureArg2, &usbMouseConfigureArg3,
                    &usbMouseConfigureArg4, &usbMouseConfigureArg5 };
static const iocshFuncDef usbMouseConfigureFuncDef =
      {"usbMouseConfigure",6,usbMouseConfigureArgs};
static void usbMouseConfigureCallFunc(const iocshArgBuf *args)
{
    usbMouseConfigure(args[0].sval, args[1].ival, args[2].ival,
                      args[3].ival, args[4].ival, args[5].ival);
}

static void
usbMouseSup_RegisterCommands(void)
{
    iocshRegister(&usbMouseConfigureFuncDef,usbMouseConfigureCallFunc);
}
epicsExportRegistrar(usbMouseSup_RegisterCommands);
