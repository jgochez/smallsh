# Define the compiler
CC=gcc

# Define any compile-time flags
CFLAGS=-std=c99 -Wall -g

# Define the target executable name
TARGET=smallsh

# Default target: build the program
all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).c

