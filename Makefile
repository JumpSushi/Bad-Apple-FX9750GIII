# Gint Add-in Makefile
#
# This Makefile is used to build the project.
#
# Before using, you must have a working gint toolchain and you must define
# the GINT_SDK environment variable to point to your SDK.
# For example: export GINT_SDK=/path/to/gint-sdk

# Project name
PROJECT = badapple

# Source files
SOURCES = badapple.c

# SDK path
# You can set the GINT_SDK environment variable, or define it here.
# GINT_SDK ?= /path/to/your/sdk
ifeq ($(GINT_SDK),)
$(error GINT_SDK is not set. Please point it to your gint SDK installation.)
endif

# Toolchain programs
PREFIX   = sh3eb-elf-
CC       = $(PREFIX)gcc
LD       = $(PREFIX)ld
OBJCOPY  = $(PREFIX)objcopy
G1A      = $(GINT_SDK)/bin/g1a-from-elf
MKADDIN  = $(GINT_SDK)/bin/mkaddin

# Build flags
# Add -I flags for your own header files if you need to
INCLUDES = -I$(GINT_SDK)/include
# Add -D flags for compile-time definitions
DEFINES  =
# Optimization and warning flags
CFLAGS   = -O2 -Wall -Wextra $(DEFINES) $(INCLUDES)
LDFLAGS  = -T$(GINT_SDK)/ld/fx9860g.ld -nostdlib
LIBS     = -L$(GINT_SDK)/lib -lgint

# Build targets
all: $(PROJECT).g1a

# Rule to build the final add-in
$(PROJECT).g1a: $(PROJECT).elf
	$(G1A) $< $@ --name "$(PROJECT)"

# Rule to link the ELF executable
$(PROJECT).elf: $(SOURCES:.c=.o)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# Rule to compile C source files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# --- Video processing ---
# This part of the Makefile handles the video conversion.
# It requires ffmpeg to be installed.

RAW_VIDEO = badapple.raw
COMPRESSED_VIDEO = badapple.bin
CONVERTER = converter
CONVERTER_SRC = converter.c

# Rule to build the converter tool
$(CONVERTER): $(CONVERTER_SRC)
	gcc -o $@ $< -O2

# Rule to create the compressed video file
$(COMPRESSED_VIDEO): $(RAW_VIDEO) $(CONVERTER)
	./$(CONVERTER) $(RAW_VIDEO) $(COMPRESSED_VIDEO)

# Rule to extract and dither the video frames using ffmpeg
$(RAW_VIDEO): badapple.mp4
	ffmpeg -i badapple.mp4 -vf "fps=15,scale=80:64,format=monob" -f rawvideo $@

# Add the compressed video to the add-in resources
# This is a placeholder. You might need a more sophisticated way to bundle data.
# For now, we assume the .bin file will be manually copied to the calculator.

# Phony targets
.PHONY: all clean video

# Target to build everything including the video
video: $(COMPRESSED_VIDEO) all

# Clean up build files
clean:
	rm -f *.o *.elf *.g1a $(CONVERTER) $(RAW_VIDEO) $(COMPRESSED_VIDEO)
