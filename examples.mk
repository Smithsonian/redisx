SRC := examples

include config.mk

LDFLAGS += -L$(LIB) -lredisx
LD_LIBRARY_PATH := $(LIB):$(LD_LIBRARY_PATH)

.PHONY: all
all: $(BIN)/hello

include build.mk
