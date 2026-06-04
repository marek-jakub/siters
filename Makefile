CC = gcc

PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 poppler-glib)
PKG_LIBS   := $(shell pkg-config --libs gtk+-3.0 poppler-glib)

DATADIR ?= $(CURDIR)
CFLAGS = $(PKG_CFLAGS) -DDATADIR=\"$(DATADIR)\" -Iinclude -Iinclude/json-glib -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include
LIBS   = $(PKG_LIBS) -lm -L/usr/lib/x86_64-linux-gnu -l:libjson-glib-1.0.so.0

TARGET = siters
SRC = src/main.c src/siters.c src/sessions_model.c src/session_model.c src/document_model.c

all:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)
