# Part of redisx
#
# Generates headless README.md and HTML documentation 
#
# Author: Attila Kovacs

CC ?= gcc

# Check if there is a doxygen we can run
ifndef DOXYGEN
  DOXYGEN := $(shell which doxygen)
else
  $(shell test -f $(DOXYGEN))
endif

# Build only if there is doxygen
ifneq ($(.SHELLSTATUS),0)
  $(info WARNING! Doxygen is not available. Will skip)
endif

# Doxygen documentation (HTML and man pages) under docs/
.PHONY: all
all: Doxyfile ../src ../include README.md
	@echo "   [doxygen]"
	@$(DOXYGEN) Doxyfile

.PHONY: clean
clean:

.PHONY: distclean
distclean: clean
	rm -rf html *.tag README.md README-undecorated.md

.INTERMEDIATE: docedit
docedit: src/docedit.c
	$(CC) -o $@ $^

# Generate headless and undecorated README variants
README.md README-undecorated.md: ../README.md docedit
	@echo "   [README variants]"
	@./docedit

.PHONY: help
help:
	@echo
	@echo "Syntax: make [target]"
	@echo
	@echo "The following targets are available:"
	@echo
	@echo "  all           (default) Build HTML documentation."
	@echo "  clean         Removes intermediate products."
	@echo "  distclean     Deletes all generated files."
	@echo
