# ===============================================================================
# WARNING! You should leave this Makefile alone probably
#          To configure the build, you can edit config.mk, or else you export the 
#          equivalent shell variables prior to invoking 'make' to adjust the
#          build configuration. 
# ===============================================================================

include config.mk

# ===============================================================================
# Specific build targets and recipes below...
# ===============================================================================

# The version of the shared .so libraries
SO_VERSION := 1

# Check if there is a doxygen we can run
ifndef DOXYGEN
  DOXYGEN := $(shell which doxygen)
else
  $(shell test -f $(DOXYGEN))
endif

# If there is doxygen, build the API documentation also by default
ifeq ($(.SHELLSTATUS),0)
  DOC_TARGETS += local-dox
else
  $(info WARNING! Doxygen is not available. Will skip 'dox' target) 
endif


# Link against thread lib
LDFLAGS += -pthread

# Build everything...
.PHONY: all
all: shared static $(DOC_TARGETS) check

# Build for distribution
.PHONY: distro
distro: shared $(DOC_TARGETS)

# Shared libraries (versioned and unversioned)
.PHONY: shared
shared: $(LIB)/libredisx.so

# Legacy static libraries (locally built)
.PHONY: static
static: $(LIB)/libredisx.a

# Run regression tests
.PHONY: test
test:
	make -C test

# Remove intermediates
.PHONY: clean
clean:
	rm -f $(OBJECTS) README-redisx.md gmon.out

# Remove all generated files
.PHONY: distclean
distclean: clean
	rm -f Doxyfile.local $(LIB)/libredisx.so* $(LIB)/libredisx.a


# ----------------------------------------------------------------------------
# The nitty-gritty stuff below
# ----------------------------------------------------------------------------

SOURCES = $(SRC)/redisx.c $(SRC)/redisx-net.c $(SRC)/redisx-hooks.c $(SRC)/redisx-client.c \
          $(SRC)/redisx-tab.c $(SRC)/redisx-sub.c $(SRC)/redisx-script.c

# Generate a list of object (obj/*.o) files from the input sources
OBJECTS := $(subst $(SRC),$(OBJ),$(SOURCES))
OBJECTS := $(subst .c,.o,$(OBJECTS))

$(LIB)/libredisx.so: $(LIB)/libredisx.so.$(SO_VERSION)

# Shared library
$(LIB)/libredisx.so.$(SO_VERSION): $(SOURCES)

# Static library
$(LIB)/libredisx.a: $(OBJECTS)

README-redisx.md: README.md
	LINE=`sed -n '/\# /{=;q;}' $<` && tail -n +$$((LINE+2)) $< > $@

dox: README-redisx.md

.INTERMEDIATE: Doxyfile.local
Doxyfile.local: Doxyfile Makefile
	sed "s:resources/header.html::g" $< > $@
	sed -i "s:^TAGFILES.*$$:TAGFILES = :g" $@

# Local documentation without specialized headers. The resulting HTML documents do not have
# Google Search or Analytics tracking info.
.PHONY: local-dox
local-dox: README-redisx.md Doxyfile.local
	doxygen Doxyfile.local

# Default values for install locations
# See https://www.gnu.org/prep/standards/html_node/Directory-Variables.html 
prefix ?= /usr
exec_prefix ?= $(prefix)
libdir ?= $(exec_prefix)/lib
includedir ?= $(prefix)/include
datarootdir ?= $(prefix)/share
datadir ?= $(datarootdir)
mydatadir ?= $(datadir)/redisx
docdir ?= $(datarootdir)/doc/redisx
htmldir ?= $(docdir)/html

.PHONY: install
install: install-libs install-headers install-apidoc

.PHONY: install-libs
install-libs: shared
	@echo "installing libraries to $(libdir)"
	install -d $(libdir)
	install -m 755 -D $(LIB)/lib*.so* $(libdir)/

.PHONY: install-headers
install-headers:
	@echo "installing headers to $(includedir)"
	install -d $(includedir)
	install -m 644 -D include/* $(includedir)/

.PHONY: install-apidoc
install-apidoc: $(DOC_TARGETS)
	@echo "installing API documentation to $(htmldir)"
	install -d $(htmldir)/search
	install -m 644 -D apidoc/html/search/* $(htmldir)/search/
	install -m 644 -D apidoc/html/*.* $(htmldir)/
	@echo "installing Doxygen tag file to $(docdir)"
	install -d $(docdir)
	install -m 644 -D apidoc/*.tag $(docdir)/

# Built-in help screen for `make help`
.PHONY: help
help:
	@echo
	@echo "Syntax: make [target]"
	@echo
	@echo "The following targets are available:"
	@echo
	@echo "  shared        Builds the shared 'libredisx.so' (linked to versioned)."
	@echo "  static        Builds the static 'lib/libredisx.a' library."
	@echo "  local-dox     Compiles local HTML API documentation using 'doxygen'."
	@echo "  check         Performs static analysis with 'cppcheck'."
	@echo "  all           All of the above."
	@echo "  install       Install components (e.g. 'make prefix=<path> install')"
	@echo "  clean         Removes intermediate products."
	@echo "  distclean     Deletes all generated files."
	@echo

# This Makefile depends on the config and build snipplets.
Makefile: config.mk build.mk

# ===============================================================================
# Generic targets and recipes below...
# ===============================================================================

include build.mk

	
