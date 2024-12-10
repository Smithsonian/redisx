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

# Build for distribution
.PHONY: distro
distro: shared $(DOC_TARGETS)

# Build everything...
.PHONY: all
all: shared static $(DOC_TARGETS) check

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

# 'test' + 'analyze'
.PHONY: check
check: test analyze

# Static code analysis via Facebook's infer
.PHONY: infer
infer: clean
	infer run -- make shared

# Remove intermediates
.PHONY: clean
clean:
	rm -f $(OBJECTS) README-redisx.md gmon.out

# Remove all generated files
.PHONY: distclean
distclean:
	rm -f Doxyfile.local $(LIB)/libredisx.so* $(LIB)/libredisx.a


# ----------------------------------------------------------------------------
# The nitty-gritty stuff below
# ----------------------------------------------------------------------------

SOURCES = $(SRC)/redisx.c $(SRC)/resp.c $(SRC)/redisx-net.c $(SRC)/redisx-hooks.c $(SRC)/redisx-client.c \
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

# Some standard GNU targets, that should always exist...
.PHONY: html
html: local-dox

.PHONY: dvi
dvi:

.PHONY: ps
ps:

.PHONY: pdf
pdf:

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

# Standard install commands
INSTALL_PROGRAM ?= install
INSTALL_DATA ?= install -m 644

.PHONY: install
install: install-libs install-headers install-html

.PHONY: install-libs
install-libs:
ifneq ($(wildcard $(LIB)/*),)
	@echo "installing libraries to $(DESTDIR)$(libdir)"
	install -d $(DESTDIR)$(libdir)
	$(INSTALL_PROGRAM) -D $(LIB)/lib*.so* $(DESTDIR)$(libdir)/
else
	@echo "WARNING! Skipping libs install: needs 'shared' and/or 'static'"
endif

.PHONY: install-headers
install-headers:
	@echo "installing headers to $(DESTDIR)$(includedir)"
	install -d $(DESTDIR)$(includedir)
	$(INSTALL_DATA) -D include/* $(DESTDIR)$(includedir)/

.PHONY: install-html
install-html:
ifneq ($(wildcard apidoc/html/search/*),)
	@echo "installing API documentation to $(DESTDIR)$(htmldir)"
	install -d $(DESTDIR)$(htmldir)/search
	$(INSTALL_DATA) -D apidoc/html/search/* $(DESTDIR)$(htmldir)/search/
	$(INSTALL_DATA) -D apidoc/html/*.* $(DESTDIR)$(htmldir)/
	@echo "installing Doxygen tag file to $(DESTDIR)$(docdir)"
	install -d $(DESTDIR)$(docdir)
	$(INSTALL_DATA) -D apidoc/*.tag $(DESTDIR)$(docdir)/
else
	@echo "WARNING! Skipping apidoc install: needs doxygen and 'local-dox'"
endif

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
	@echo "  analyze       Performs static analysis with 'cppcheck'."
	@echo "  all           All of the above."
	@echo "  distro        shared libs and documentation (default target)."
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

