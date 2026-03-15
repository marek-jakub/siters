CC=gcc
CFLAGS=`pkg-config --cflags gtk+-3.0 poppler-glib` -Iinclude
LIBS=`pkg-config --libs gtk+-3.0 poppler-glib` -lm
TARGET=siters
SRC=src/main.c src/siters.c src/sessions_model.c src/session_model.c src/document_model.c

all:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)