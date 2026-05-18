CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
FC_CFLAGS = $(shell pkg-config --cflags fontconfig 2>/dev/null)
FC_LIBS   = $(shell pkg-config --libs   fontconfig 2>/dev/null)
LDFLAGS = -lX11 -lXfixes -lXft $(FC_LIBS)

SRC = main.c ipc.c clipboard.c popup.c
OBJ = $(SRC:.c=.o)
TARGET = copy_xlqd

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c common.h
	$(CC) $(CFLAGS) $(FC_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
