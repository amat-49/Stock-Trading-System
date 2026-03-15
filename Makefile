# Compiler and flags
CC = gcc
CFLAGS = -g

# Libraries required by sqlite3.c
# -lsqlite3 for thread support and -ldl for dynamic linking loader
LIBS = -lpthread -ldl

# Executable names
CLIENT = client
SERVER = server

# Default target
all: $(CLIENT) $(SERVER)

# Build the server
$(SERVER): server.c sqlite3.c
	$(CC) $(CFLAGS) server.c sqlite3.c -o $(SERVER) $(LIBS)

# Build the client
$(CLIENT): client.c
	$(CC) $(CFLAGS) client.c -o $(CLIENT) -pthread

# Clean up
clean:
	rm -f $(CLIENT) $(SERVER)
	rm -f stocks.db

# Help target
help:
	@echo "Commands: "
	@echo " make           - Compile everything"
	@echo " make clean     - Remove binaries and database"