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

usbMouse_LIBS += asyn
usbMouse_LIBS += $(EPICS_BASE_IOC_LIBS)

#=======================================
include $(TOP)/configure/RULES
