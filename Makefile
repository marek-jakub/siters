CC=gcc
CFLAGS=`pkg-config --cflags gtk+-3.0 poppler-glib`
LIBS=`pkg-config --libs gtk+-3.0 poppler-glib` -lm
TARGET=siters
SRC=app/main.c app/siters.c

all:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)