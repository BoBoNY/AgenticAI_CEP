CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -pedantic -O2
TARGET  = departures
SRC     = departures.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET) routes.txt
