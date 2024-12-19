SRC := test

include config.mk

LDFLAGS += -L$(LIB) -lredisx
LD_LIBRARY_PATH := $(LIB):$(LD_LIBRARY_PATH)

.PHONY: all
all: tests run

.PHONY: tests
tests: $(BIN)/test-ping $(BIN)/test-info $(BIN)/test-hello $(BIN)/test-tab

.PHONY: run
run: tests
	$(BIN)/test-info
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

include build.mk
