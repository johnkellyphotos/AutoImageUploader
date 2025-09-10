CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -Wno-pedantic `sdl2-config --cflags` $(shell pkg-config --cflags libnm glib-2.0)
LDFLAGS = `sdl2-config --libs` -lSDL2_ttf -lpthread -lcurl -ljson-c -lusb-1.0 -lgphoto2

all: uploader_gui

uploader_gui: uploader_gui.c uploader.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f uploader_gui
