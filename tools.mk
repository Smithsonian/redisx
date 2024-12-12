include config.mk

LDFLAGS += -L$(LIB) -lredisx -lpopt -lreadline
LD_LIBRARY_PATH := $(LIB):$(LD_LIBRARY_PATH)

# Top level make targets...
all: $(BIN)/redisx-cli

include build.mk