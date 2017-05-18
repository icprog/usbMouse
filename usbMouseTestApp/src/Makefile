TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#=============================
# Build the IOC application

PROD_IOC = usbMouseTest
# usbMouseTest.dbd will be created and installed
DBD += usbMouseTest.dbd

# usbMouseTest.dbd will be made up from these files:
usbMouseTest_DBD += base.dbd

# Include dbd files from all support applications:
usbMouseTest_DBD += usbMouse.dbd

# Add all the support libraries needed by this IOC
usbMouseTest_LIBS += usbMouse asyn
USR_SYS_LIBS += usb-1.0

# usbMouseTest_registerRecordDeviceDriver.cpp derives from usbMouseTest.dbd
usbMouseTest_SRCS += usbMouseTest_registerRecordDeviceDriver.cpp

# Build the main IOC entry point on workstation OSs.
usbMouseTest_SRCS_DEFAULT += usbMouseTestMain.cpp
usbMouseTest_SRCS_vxWorks += -nil-

# Add support from base/src/vxWorks if needed
#usbMouseTest_OBJS_vxWorks += $(EPICS_BASE_BIN)/vxComLibrary

# Finally link to the EPICS Base libraries
usbMouseTest_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

