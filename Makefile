# ============================================================================
# ld -- makefile
# ----------------------------------------------------------------------------
# Builds the linker itself.  The result is a host binary that produces
# AArch64 (Android) ELF images; it can be built for any host that has a C
# compiler.
#
#   make                build bin/ld
#   make test           build and run the test suite
#   make clean          remove the build artefacts
#   make install        install to $(PREFIX)/bin
# ============================================================================
PREFIX  ?= /usr/local
CC      ?= cc
CFLAGS  ?= -O2 -g
LDFLAGS ?=
LDLIBS  ?=

# The flags that are needed to build the linker, they are not part of CFLAGS
# so that CFLAGS can be overridden from the command line.
LD_CFLAGS := -std=c11 -Wall -Wextra -Wshadow -Wstrict-prototypes \
             -Wmissing-prototypes -Iinclude

BIN     := bin/ld
SRCDIR  := src
OBJDIR  := build

SOURCES := $(wildcard $(SRCDIR)/*.c)
OBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
DEPS    := $(OBJECTS:.o=.d)

.PHONY: all clean test install uninstall

all: $(BIN)

$(BIN): $(OBJECTS) | bin
	$(CC) $(CFLAGS) $(LD_CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(LD_CFLAGS) -MMD -MP -c -o $@ $<

bin $(OBJDIR):
	mkdir -p $@

test: $(BIN)
	./tests/run.sh

install: $(BIN)
	install -Dm755 $(BIN) "$(PREFIX)/bin/ld"
	ln -sf ld "$(PREFIX)/bin/aarch64-linux-android-ld"

uninstall:
	rm -f "$(PREFIX)/bin/ld" "$(PREFIX)/bin/aarch64-linux-android-ld"

clean:
	rm -rf $(OBJDIR) $(BIN)

-include $(DEPS)
