# The terminal holder: docs/panes.md. Needs Zig (PIN names the version) and a network the first time.
# GHOSTTY_SRC names a checkout to use instead of cloning one under third_party.
include PIN
GHOSTTY_SRC ?= third_party/ghostty
LIB := $(GHOSTTY_SRC)/zig-out/lib/libghostty-vt.a
CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra
UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
LDLIBS := -lc++
else
CPPFLAGS += -D_GNU_SOURCE
LDLIBS := -lutil -lstdc++ -lm
endif

build/holder: src/main.c src/vt.c src/vt.h $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(GHOSTTY_SRC)/zig-out/include -o $@ src/main.c src/vt.c $(LIB) $(LDLIBS)

build:
	mkdir -p build

$(LIB):
	@test "$$(zig version)" = "$(GHOSTTY_ZIG)" || { echo "holder needs zig $(GHOSTTY_ZIG)"; exit 1; }
	@test -d $(GHOSTTY_SRC)/.git || git clone --filter=blob:none --no-checkout https://github.com/ghostty-org/ghostty.git $(GHOSTTY_SRC)
	git -C $(GHOSTTY_SRC) -c advice.detachedHead=false checkout -q $(GHOSTTY_COMMIT)
	cd $(GHOSTTY_SRC) && zig build -Demit-lib-vt -Doptimize=ReleaseFast

clean:
	rm -rf build
.PHONY: clean
