CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lX11 -lXfixes

SRC = main.c
OBJ = $(SRC:.c=.o)
TARGET = copy_xlqd

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
