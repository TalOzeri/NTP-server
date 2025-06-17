# Compiler
CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra -O2

# Targets
all: ntpClient ntpServer

# Build ntpClient from main.c
ntpClient: main.c
	$(CC) $(CFLAGS) -o ntpClient main.c

# Build ntpServer from ntpServer.c
ntpServer: ntpServer.c
	$(CC) $(CFLAGS) -o ntpServer ntpServer.c

# Clean up compiled files
clean:
	rm -f ntpClient ntpServer

