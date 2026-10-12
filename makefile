# Variables
CC = gcc
CFLAGS = -Wall -g
LDLIBS = -lm
# Object files (the compiled versions of your .c files)
OBJS = main.o trie.o layerOne.o layerTwo.o layerThree.o

# ── Platform detection ───────────────────────────────────────────────────────Path = "file_system.db";

# GNU Make on Windows (MinGW) sets OS=Windows_NT automatically.
ifeq ($(OS),Windows_NT)
    TARGET = a.exe
    CFLAGS += -mthreads
    RM = del /Q
else
    TARGET = a.out
    RM = rm -f
endif

# The 'all' target runs by default
all: $(TARGET)

# Rule to create the final executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDLIBS)

# Rule to compile main.c
main.o: main.c layerOne.h layerTwo.h
	$(CC) $(CFLAGS) -c main.c

# Rule to compile layerOne.c
layerOne.o: layerOne.c layerOne.h trie.h
	$(CC) $(CFLAGS) -c layerOne.c

# Rule to compile trie.c
trie.o: trie.c trie.h
	$(CC) $(CFLAGS) -c trie.c

# Rule to compile layerTwo.c
layerTwo.o: layerTwo.c layerTwo.h
	$(CC) $(CFLAGS) -c layerTwo.c

# Rule to compile layerThree.c
layerThree.o: layerThree.c layerThree.h
	$(CC) $(CFLAGS) -c layerThree.c

clean:
	$(RM) $(OBJS) $(TARGET)
