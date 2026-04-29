
include ../config.mk

LIB ?= ../lib

# Flags for measuring test coverage
# Our own include directory
CPPFLAGS += -I../include
CFLAGS += -I../include -pg -Wall
LDFLAGS += -lm -lredisx -fprofile-arcs -ftest-coverage

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
tests: test-ping test-info test-hello test-tab test-hash

.PHONY: run
run: redisx-cli tests
ifeq ($(ONLINE),1) 
	$(info INFO: [ONLINE] Will test client functionality.)
	../$(BIN)/redisx-cli ping "Hello World!"
	./test-info
	./test-ping
	./test-hello
	./test-tab
	./test-hash
else
	$(warning WARNING! [OFFLINE] Will test redisx-cli integrity only.)
	../$(BIN)/redisx-cli --help
endif

.PHONY: coverage
coverage:
	$(MAKE) cov

cov: lcov.info
	genhtml $< -o $@

lcov.info:
	geninfo . -o $@
	lcov --remove $@ $(EXCLUDE_COV) -o $@

.PRECIOUS: %.c.gcov
%.c.gcov: %.gcno %.gcda
	gcov -b $*

.PRECIOUS: %.gcno %.gcda
%.gcno %.gcda: src/%.c
	$(MAKE) clean
	$(MAKE) run

.PHONY: redisx-cli
redisx-cli:
	$(MAKE) -C .. tools

test-%: test-%.o $(OBJECTS) | Makefile
	@echo sources: $(SOURCES)
	@echo objects: $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)

test-%.o: test-%.c | Makefile
	$(CC) -c -o $@ $(CPPFLAGS) $(CFLAGS) $<

%.o: %.c | Makefile
	$(CC) -c -o $@ $(CPPFLAGS) $(CFLAGS) -fprofile-arcs -ftest-coverage $<

# Static code analysis using 'cppcheck'
.PHONY: analyze
analyze:
	@echo "   [analyze]"
	@cppcheck $(CPPFLAGS) $(CHECKOPTS) $(SRC)

.PHONY: clean-test
clean-test:
	@rm -f $(TESTS)

.PHONY: clean-cov
clean-cov:
	@rm -rf gmon.out *.gcov *.gcda *.gcno cov cov.info

.PHONY: clean-data
clean-data:
	@rm -rf data

.PHONY: clean
clean: clean-test clean-cov
	@rm -f *.o

.PHONY: distclean
distclean: clean clean-data
	@rm -f lcov.info
	@rm -rf cov

.PHONY: help
help:
	@echo
	@echo "Syntax: make [target]"
	@echo
	@echo "The following targets are available:"
	@echo
	@echo "  run           (default) Compiles and runs regression tests."
	@echo "  coverage      Extracts test coverage data."
	@echo "  analyze       Static analysis with cppcheck."
	@echo "  clean         Removes intermediate products."
	@echo "  distclean     Deletes all generated files."
	@echo


vpath %.c ../src
