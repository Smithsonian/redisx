SRC := test

include config.mk

LDFLAGS += -L$(LIB) -lredisx
LD_LIBRARY_PATH := $(LIB):$(LD_LIBRARY_PATH)

# Check if a Redis / Valkey server is running for us
# to test on.
ONLINE = 0

REDIS = $(shell pidof redis-server)
ifneq ($(REDIS),)
  $(info INFO: Found running redis-server.)
  ONLINE = 1
endif

VALKEY = $(shell pidof valkey-server)
ifneq ($(VALKEY),)
  $(info INFO: Found running valkey-server.)
  ONLINE = 1
endif

.PHONY: all
all: tests run

.PHONY: tests
tests: $(BIN)/test-ping $(BIN)/test-info $(BIN)/test-hello $(BIN)/test-tab $(BIN)/test-hash

.PHONY: run
run: tests
ifeq ($(ONLINE),1) 
	$(info INFO: [ONLINE] Will test client functionality.)
	$(BIN)/redisx-cli ping "Hello World!"
	$(BIN)/test-info
	$(BIN)/test-ping
	$(BIN)/test-hello
	$(BIN)/test-tab
	$(BIN)/test-hash
else
	$(warning WARNING! [OFFLINE] Will test redisx-cli integrity only.)
	$(BIN)/redisx-cli --help
endif

$(BIN)/test-%: $(OBJ)/test-%.o $(LIB)/libredisx.a
	$(MAKE) $(BIN)
	$(CC) -o $@ $^ $(LDFLAGS) -lredisx

.PHONY: clean-test
clean-test:
	rm -rf bin

clean: clean-test

include build.mk
