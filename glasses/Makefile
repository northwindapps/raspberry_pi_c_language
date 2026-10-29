# Build on the Pi:  make
CC       = gcc
CXX      = g++
CFLAGS   = -O2 -Wall -Wextra -std=gnu11 -pthread
CXXFLAGS = -O2 -Wall -Wextra -std=c++20 -pthread $(shell pkg-config --cflags libcamera)
LDLIBS   = $(shell pkg-config --libs libcamera) -ljpeg -lasound -lm -pthread

OBJS = main.o http.o audio.o jpeg_enc.o camera.o

glasses: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDLIBS)

camera.o: camera.cpp camera.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c *.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f glasses $(OBJS)

.PHONY: clean
