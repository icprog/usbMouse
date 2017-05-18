TOP=..
include $(TOP)/configure/CONFIG
#=======================================

# Build usbMouse as a library for an IOC:
LIBRARY_IOC += usbMouse

# Library Source files
usbMouse_SRCS += usbMouse.c

# Link with the asyn and base libraries
usbMouse_LIBS += asyn
usbMouse_LIBS += $(EPICS_BASE_IOC_LIBS)

# Install .dbd and .db files
DBD += usbMouse.dbd
DB_INSTALLS += usbMouse.db

#=======================================
include $(TOP)/configure/RULES
