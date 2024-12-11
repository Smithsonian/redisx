# Use the parent directories for libraries and headers.
LIB = ../lib
INC = ../include
BUILD_MODE = debug

ifdef XCHANGE
 XCHANGE := ../$(XCHANGE)
endif

# Load the common Makefile definitions...
include ../config.mk

.PHONY: all
all: tests run

.PHONY: tests
tests: $(BIN)/test-ping $(BIN)/test-hello $(BIN)/test-tab

.PHONY: run
run: tests
	$(BIN)/test-ping
	$(BIN)/test-hello
	$(BIN)/test-tab

$(BIN)/test-%: $(OBJ)/test-%.o $(LIB)/libredisx.a
	make $(BIN)
	$(CC) -o $@ $^ $(LDFLAGS) -lredisx

.PHONY: clean-test
clean-test:
	rm -rf bin

clean: clean-test

# Finally, the standard generic rules and targets...
include ../build.mk

