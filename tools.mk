include config.mk

LDFLAGS := -lpopt -lreadline -lbsd -L$(LIB) -lredisx $(LDFLAGS)
LD_LIBRARY_PATH := $(LIB):$(LD_LIBRARY_PATH)

# Top level make targets...
all: $(BIN)/redisx-cli

include build.mk