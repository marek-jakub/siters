CC = gcc

DATADIR ?= $(CURDIR)

PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
PKG_LIBS   := $(shell pkg-config --libs gtk+-3.0)
MUPDF_CFLAGS := $(shell pkg-config --cflags mupdf)
MUPDF_LIBS   := $(shell pkg-config --libs --static mupdf)

# MuPDF static lib may reference symbols in GTK libs (harfbuzz)
# those must appear after -lmupdf so GNU ld can resolve them.
ORDERED_LIBS = $(MUPDF_LIBS) $(PKG_LIBS)

CFLAGS = $(PKG_CFLAGS) $(MUPDF_CFLAGS) \
         -DDATADIR=\"$(DATADIR)\" \
         -Iinclude -Iinclude/json-glib \
         -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include
LIBS   = $(ORDERED_LIBS) -L/usr/lib/x86_64-linux-gnu -l:libjson-glib-1.0.so.0

TARGET = siters
SRC = src/main.c src/siters.c src/sessions_model.c src/session_model.c \
      src/document_model.c src/pdf_mupdf.c

all:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

memdebug: CFLAGS += -DMEM_DEBUG
memdebug: all

clean:
	rm -f $(TARGET)
