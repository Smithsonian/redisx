# Part of RedisX
#
# Build exammple programs
#
# Author: Attila Kovacs

EXAMPLES = $(subst .c,,$(wildcard *.c))

LIB ?= ../lib

ifdef XCHANGE
  CPPFLAGS += -I$(XCHANGE)/include
  LDFLAGS += -L$(XCHANGE)/lib
endif

CPPFLAGS += -I../include
LDFLAGS += -L$(LIB) -lredisx -lxchange -lpthread


.PHONY: all
all: $(EXAMPLES)

# Static code analysis using 'cppcheck'
.PHONY: analyze
analyze:
	@echo "   [analyze]"
	@cppcheck $(CPPFLAGS) $(CHECKOPTS) .

.PHONY: clean
clean:

.PHONY: distclean
distclean:
	@rm -f $(EXAMPLES)

example-%: example-%.c
	$(CC) -o $@ $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS)

.PHONY: help
help:
	@echo
	@echo "Syntax: make [target]"
	@echo
	@echo "The following targets are available:"
	@echo
	@echo "  all           (default) Build all example programs."
	@echo "  analyze       Runs 'cppcheck' static analysis tool on sources."
	@echo "  clean         Removes intermediate products."
	@echo "  distclean     Deletes all generated files."
	@echo

