# The compiler to use
CC = clang

# Compiler flags:
# -std=c17: Use the C17 standard
# -Wall: Enable all warnings
# -Isrc: Tell the compiler to look for headers in the 'src' directory
CFLAGS = -std=c17 -Wall -Isrc

# The name of the final executable
TARGET = ore

# Automatically find all .c files in the 'src' directory and its subdirectories
SRCS = $(shell find src -name '*.c')

# The default rule, which is run when you just type "make"
# It says that the "all" target depends on the "your_program" target.
all: $(TARGET)

# Rule to build the executable:
# It says that to create the TARGET, it depends on all the source files (SRCS).
# The command to run is the compilation command we used before.
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

# A "clean" rule to remove the built executable.
# You can run this with "make clean"
clean:
	rm -f $(TARGET)
