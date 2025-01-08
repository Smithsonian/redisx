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

# Whether to use OpenMP. If not defined, we'll enable it automatically if 
# libgomp is available
#WITH_OPENMP = 1

# Whether to build with TLS support (via OpenSSL). If not defined, we'll
# enable it automatically if libssl is available
#WITH_TLS = 1

# Add include/ directory
CPPFLAGS += -I$(INC)

# Base compiler options (if not defined externally...)
CFLAGS ?= -g -Os -Wall

# Compile for specific C standard
ifdef CSTANDARD
  CFLAGS += -std=$(CSTANDARD)
endif

# Extra linker flags (if any)
#LDFLAGS=

# cppcheck options for 'check' target
CHECKOPTS ?= --enable=performance,warning,portability,style --language=c \
            --error-exitcode=1 --std=c99 

# Add-on ccpcheck options
CHECKOPTS += --inline-suppr $(CHECKEXTRA)

# Exhaustive checking for newer cppcheck
#CHECKOPTS += --check-level=exhaustive

# Specific Doxygen to use if not the default one
#DOXYGEN ?= /opt/bin/doxygen

# ============================================================================
# END of user config section. 
#
# Below are some generated constants based on the one that were set above
# ============================================================================

ifneq ($(shell which ldconfig), )
  # Detect OpenSSL automatically, and enable TLS support if present
  ifndef WITH_TLS 
    ifneq ($(shell ldconfig -p | grep libssl), )
      $(info INFO: TLS support is enabled automatically.)
      WITH_TLS = 1
    else
      $(info INFO: optional TLS support is not enabled.)
      WITH_TLS = 0
    endif
  endif

  # Detect OpenMP automatically, and enable WITH_OPENMP support if present
  ifndef WITH_OPENMP 
    ifneq ($(shell ldconfig -p | grep libgomp), )
      $(info INFO: OpenMP optimizations are enabled automatically.)
      WITH_OPENMP = 1
    else
      $(info INFO: optional OpenMP optimizations are not enabled.)
      WITH_OPENMP = 0
    endif
  endif
endif

# Link against math libs (for e.g. isnan()), and xchange dependency
LDFLAGS += -lm -lxchange

ifeq ($(WEXTRA),1)
  CFLAGS += -Wextra
endif

ifeq ($(WITH_OPENMP),1)
  CFLAGS += -fopenmp
  LDFLAGS += -fopenmp
endif

ifeq ($(WITH_TLS),1)
  CPPFLAGS += -DWITH_TLS=1
  LDFLAGS += -lssl
endif

# Search for libraries under LIB
ifneq ($(findstring $(LIB),$(LD_LIBRARY_PATH)),$LIB)
  LDFLAGS += -L$(LIB)
  LD_LIBRARY_PATH := $(LIB):$(LD_LIBRARY_PATH)
endif

# Compile and link against a specific xchange library (if defined)
ifdef XCHANGE
  CPPFLAGS += -I$(XCHANGE)/include
  LDFLAGS += -L$(XCHANGE)/lib
  LD_LIBRARY_PATH := $(XCHANGE)/lib:$(LD_LIBRARY_PATH)
endif

# Search for files in the designated locations
vpath %.h $(INC)
vpath %.c $(SRC)
vpath %.o $(OBJ)
vpath %.d dep 

