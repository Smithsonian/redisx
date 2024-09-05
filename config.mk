# ===========================================================================
# Generic configuration options for building the RedisX library (both static 
# and shared).
#
# You can include this snipplet in your Makefile also.
# ============================================================================

# Location under which the Smithsonian/xchange library is installed.
# (I.e., the root directory under which there is an include/ directory
# that contains xchange.h, and a lib/ or lib64/ directory that contains
# libxchange.so
XCHANGE ?= /usr

# Folders in which sources and header files are located, respectively
SRC ?= src
INC ?= include

# Folders for compiled objects, libraries, and binaries, respectively 
OBJ ?= obj
LIB ?= lib
BIN ?= bin

# Compiler: use gcc by default
CC ?= gcc

# Base compiler options (if not defined externally...)
CFLAGS ?= -Os -Wall 

# Add include/ directory
CFLAGS += -I$(INC)

# Extra warnings (not supported on all compilers)
#CFLAGS += -Wextra

# Link against math libs (for e.g. isnan())
LDFLAGS ?= -lm

# Compile and link against a specific xchange library (if defined)
ifdef XCHANGE
  CFLAGS += -I$(XCHANGE)/include
  LDFLAGS = -L$(XCHANGE)/lib
  LD_LIBRARY_PATH = $(XCHANGE)/lib:$(LD_LIBRARY_PATH)
endif

# Always link against the xchange lib.
LDFLAGS += -lxchange

# cppcheck options for 'check' target
CHECKOPTS ?= --enable=performance,warning,portability,style --language=c \
            --error-exitcode=1 $(CHECKEXTRA)

CHECKOPTS += --template='{file}({line}): {severity} ({id}): {message}' --inline-suppr

# Exhaustive checking for newer cppcheck
#CHECKOPTS += --check-level=exhaustive

# Specific Doxygen to use if not the default one
#DOXYGEN ?= /opt/bin/doxygen

# ============================================================================
# END of user config section. 
#
# Below are some generated constants based on the one that were set above
# ============================================================================

# Compiler and linker options etc.
ifeq ($(BUILD_MODE),debug)
	CFLAGS += -g -DDEBUG
endif

# Search for files in the designated locations
vpath %.h $(INC)
vpath %.c $(SRC)
vpath %.o $(OBJ)
vpath %.d dep 
