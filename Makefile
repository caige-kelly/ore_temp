# The compiler to use
CC = zig cc

# Compiler flags:
# -std=c23: Use the C23 standard
# -Wall: Enable all warnings
# -Isrc: Tell the compiler to look for headers in the 'src' directory
CFLAGS = -std=c23 -Wall -Isrc -g

# The name of the final executable
TARGET = ore

# Automatically find all .c files in the 'src' directory and its subdirectories
SRCS = $(shell find src -name '*.c')

# The default rule, which is run when you just type "make"
# It says that the "all" target depends on the "your_program" target.
.PHONY: all clean test

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

# Run the regression test harness.
test:
	@sh tools/test.sh
