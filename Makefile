ifeq (,$(VERBOSE))
    MAKEFLAGS += -s
    ECHO=echo
else
    ECHO=@:
endif

BUILDDIR ?= build
.DEFAULT_GOAL := cmake-build

# Skia is a build dependency, not a per-user cache entry.  Keeping it below
# the build tree also means that `sudo make install` never downloads or builds
# anything as root.
SKIA_BUILD_ROOT := $(BUILDDIR)/_deps
SKIA_SOURCE_DIR := $(SKIA_BUILD_ROOT)/skia
SKIA_LIBRARY := $(SKIA_SOURCE_DIR)/out/awesome-vulkan/libskia.a
SKIA_BUILD_SCRIPT := tools/build-skia-vulkan.sh
SKIA_STAMP := $(SKIA_BUILD_ROOT)/.skia-built

.PHONY: skia-ready

skia-ready: $(SKIA_STAMP)

$(SKIA_STAMP): $(SKIA_BUILD_SCRIPT)
	@if test "$$(id -u)" -eq 0; then \
		echo "Do not build as root. Run 'make' first, then 'sudo make install'." >&2; \
		exit 1; \
	fi
	$(ECHO) "Building Skia in $(SKIA_BUILD_ROOT)…"
	AWESOME_SKIA_BUILD_ROOT="$(abspath $(SKIA_BUILD_ROOT))" $(SKIA_BUILD_SCRIPT)
	touch $(SKIA_STAMP)

# Run "make" in $(BUILDDIR) by default.
# This is required to generate all files already, which should not be generated
# with "(sudo) make install" only later.
cmake-build: $(BUILDDIR)/Makefile
	$(ECHO) "Building…"
	cmake --build $(BUILDDIR)

# Run CMake with CMAKE_ARGS defined on command line ("make CMAKE_ARGS=…").
ifeq ($(origin CMAKE_ARGS),command line)
.PHONY: $(BUILDDIR)/Makefile
endif

$(BUILDDIR)/Makefile: $(SKIA_STAMP)
	$(ECHO) "Creating build directory and running cmake in it. You can also run CMake directly, if you want."
	$(ECHO)
	mkdir -p $(BUILDDIR)
	$(ECHO) "Running cmake…"
	cmake -S "$(CURDIR)" -B "$(BUILDDIR)" \
		-DSKIA_SOURCE_DIR="$(abspath $(SKIA_SOURCE_DIR))" \
		-DSKIA_LIBRARY="$(abspath $(SKIA_LIBRARY))" \
		-DGENERATE_DOC=OFF -DGENERATE_MANPAGES=OFF $(CMAKE_ARGS)

tags:
	git ls-files | xargs ctags

install:
	@if test ! -f "$(BUILDDIR)/CMakeCache.txt" || test ! -x "$(BUILDDIR)/awesome"; then \
		echo "Nothing built to install. Run 'make' as your regular user first." >&2; \
		exit 1; \
	fi
	$(ECHO) "Installing…"
	cmake --install $(BUILDDIR)

distclean:
	$(ECHO) "Cleaning up build directory…"
	$(RM) -r $(BUILDDIR)

# Use an explicit rule to not "update" the Makefile via the implicit rule below.
Makefile: ;

%: $(BUILDDIR)/Makefile
	$(ECHO) "Running make $@ in $(BUILDDIR)…"
	$(MAKE) -C $(BUILDDIR) $@

.PHONY: cmake-build install distclean tags
