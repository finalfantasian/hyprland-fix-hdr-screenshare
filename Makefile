CXX ?= g++

EXTRA_FLAGS =

ifeq ($(CXX),g++)
    EXTRA_FLAGS += -fno-gnu-unique
endif

all:
	$(CXX) -shared -fPIC $(EXTRA_FLAGS) main.cpp -o fix-hdr-screenshare.so -g `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon` -std=c++2b -Wno-narrowing

clean:
	rm -f ./fix-hdr-screenshare.so
