CC = gcc

PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 poppler-glib)
PKG_LIBS   := $(shell pkg-config --libs gtk+-3.0 poppler-glib)

CFLAGS = $(PKG_CFLAGS) -Iinclude
LIBS   = $(PKG_LIBS) -lm

TARGET = siters
SRC = src/main.c src/siters.c src/sessions_model.c src/session_model.c src/document_model.c

all:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)
