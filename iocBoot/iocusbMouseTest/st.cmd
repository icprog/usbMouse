#!../../bin/linux-x86_64/usbMouseTest


#############################################################################
# Set up environment
< envPaths
epicsEnvSet(P, "$(P=usbMouse:)")
epicsEnvSet(R, "$(R=1:)")
epicsEnvSet(PORT, "M0")

# Allow environment to override default vendor/product codes
# The default values of 046D:C019 are for a Logitech optical tilt-wheel mouse
# Values of 045E:0039 are, for example, a Microsoft 5-button optical mouse

#jhlee@kaffee: iocusbMouseTest (master)$ lsusb
# Bus 001 Device 018: ID 03f0:1198 Hewlett-Packard




epicsEnvSet(VENDOR, "$(VENDOR=0x03F0)")
epicsEnvSet(PRODUCT, "$(PRODUCT=0x1198)")



cd "$(TOP)"

#############################################################################
# Register support components
dbLoadDatabase "dbd/usbMouseTest.dbd"
usbMouseTest_registerRecordDeviceDriver pdbbase

#############################################################################
# Configure port
#usbMouseConfigure(port, vendor, product, number, interval, priority)
usbMouseConfigure("$(PORT)", $(VENDOR), $(PRODUCT), 0, 0, 0)
asynSetTraceIOMask("$(PORT)", 2000 ,0x4)
# Uncomment the following line to enable readback data display
#asynSetTraceMask("$(PORT)", 2000, 0x9)

#############################################################################
# Load record instances
dbLoadRecords("db/usbMouse.db","P=$(P),R=$(R),PORT=$(PORT)")

#############################################################################
# Start EPICS
cd "$(TOP)/iocBoot/$(IOC)"
iocInit

epicsThreadSleep(2)
asynReport(2,"$(PORT)")
