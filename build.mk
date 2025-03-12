
# ============================================================================
# Generic build targets and recipes for xchange.
# 
# You can include this in your Makefile also.
# ============================================================================

# Regular object files
$(OBJ)/%.o: %.c $(OBJ) Makefile
	$(CC) -o $@ -c $(CPPFLAGS) $(CFLAGS) $<

# Share library recipe
$(LIB)/%.so.$(SO_VERSION):
	@$(MAKE) $(LIB)
	$(CC) -o $@ $(CPPFLAGS) $(CFLAGS) $^ -shared -fPIC -Wl,-soname,$(subst $(LIB)/,,$@) $(LDFLAGS)

# Unversioned shared libs (for linking against)
$(LIB)/lib%.so:
	@rm -f $@
	ln -sr $< $@

# Static library: *.a
$(LIB)/%.a:
	@$(MAKE) $(LIB)
	ar -rc $@ $^
	ranlib $@

# Simple binaries
$(BIN)/%: $(OBJ)/%.o
	@$(MAKE) $(BIN)
	$(CC) -o $@ $^ $(LDFLAGS)

# Link binaries against redisx lib
$(BIN)/%: LDFLAGS :=  $(LDFLAGS) -lpopt -lredisx

# Create sub-directories for build targets
dep $(OBJ) $(LIB) $(BIN) apidoc:
	mkdir -p $@

# Remove intermediate files locally
.PHONY: clean-local
clean-local:
	rm -rf obj

# Remove all locally built files, effectively restoring the repo to its 
# pristine state
.PHONY: distclean-local
distclean-local: clean-local
	rm -rf bin lib apidoc infer-out

# Remove intermediate files (general)
.PHONY: clean
clean: clean-local

# Remove intermediate files (general)
.PHONY: distclean
distclean: clean distclean-local

# Static code analysis using 'cppcheck'
.PHONY: analyze
analyze:
	@echo "   [analyze]"
	@cppcheck $(CPPFLAGS) $(CHECKOPTS) src

# Doxygen documentation (HTML and man pages) under apidocs/
.PHONY: dox
dox: README.md Doxyfile apidoc $(SRC) $(INC)
	@echo "   [doxygen]"
	@$(DOXYGEN)

