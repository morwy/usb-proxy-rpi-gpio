LDFLAG=-lusb-1.0 -pthread -ljsoncpp -Lthird-party/CppGPIO -lcppgpio

ifndef CFLAGS
	ifeq ($(TARGET),Debug)
		CFLAGS=-Wall -Wextra -g
	else
		CFLAGS=-Wall -Wextra -O2
	endif
endif

.PHONY: all clean

all:
	(cd third-party/CppGPIO; $(MAKE))
	($(MAKE) usb-proxy)

usb-proxy: usb-proxy.o host-raw-gadget.o device-libusb.o proxy.o misc.o
	g++ usb-proxy.o host-raw-gadget.o device-libusb.o proxy.o misc.o $(LDFLAG) -o usb-proxy

%.o: %.cpp %.h
	g++ $(CFLAGS) -c $<

%.o: %.cpp
	g++ $(CFLAGS) -c $<


clean:
	(cd third-party/CppGPIO; $(MAKE) clean; rm *.o; rm *.a; rm *.so*)
	(rm *.o; rm usb-proxy)
