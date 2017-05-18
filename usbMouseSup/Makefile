TOP=..
include $(TOP)/configure/CONFIG
#=======================================


#---------------------
# Install DBD files
#
DBD += usbMouse.dbd

# Build usbMouse as a library for an IOC:
LIBRARY_IOC += usbMouse
# Library Source files
usbMouse_SRCS += usbMouse.c

usbMouse_LIBS += $(EPICS_BASE_IOC_LIBS)


USR_CFLAGS  += `pkg-config --cflags libusb-1.0`
USR_LDFLAGS += `pkg-config --libs libusb-1.0`
PROD_LDLIBS += `pkg-config --libs libusb-1.0`

#=======================================
include $(TOP)/configure/RULES
