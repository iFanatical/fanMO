# fanMO -- a native Qt6 mod organizer for Skyrim SE on Linux.
#
# Programs sharing one core: fanmo (the UI), fanmo-cli (the same operations,
# headless), fanmo-tests and fanmo-uitests.
#
# Gentoo: dev-qt/qtbase[network,widgets,concurrent], dev-libs/openssl,
#         app-arch/bzip2, sys-libs/zlib, dev-libs/xxhash, and 7-Zip at
#         runtime (7z or 7zz).

CORE_PKGS = Qt6Core Qt6Network Qt6Concurrent libcrypto
UI_PKGS   = Qt6Widgets Qt6Gui $(CORE_PKGS)

CXX      ?= g++
MOC      ?= $(shell pkg-config --variable=libexecdir Qt6Core)/moc

override CXXFLAGS += -std=c++20 -O2 -Wall -Wextra -fPIC -Isrc \
                     $(shell pkg-config --cflags $(UI_PKGS))
CORE_LIBS = $(shell pkg-config --libs $(CORE_PKGS)) -lbz2 -lz -lxxhash
UI_LIBS   = $(shell pkg-config --libs $(UI_PKGS)) -lbz2 -lz -lxxhash

PREFIX   ?= $(HOME)/.local

CORE_SRC  = $(wildcard src/core/*.cpp)
UI_SRC    = $(wildcard src/ui/*.cpp)
CORE_MOC  = $(shell grep -l Q_OBJECT src/core/*.h 2>/dev/null)
UI_MOC    = $(shell grep -l Q_OBJECT src/ui/*.h 2>/dev/null)

CORE_OBJ  = $(CORE_SRC:src/%.cpp=build/%.o) $(CORE_MOC:src/%.h=build/moc/%.o)
UI_OBJ    = $(UI_SRC:src/%.cpp=build/%.o) $(UI_MOC:src/%.h=build/moc/%.o)

all: fanmo fanmo-cli fanmo-tests fanmo-uitests

fanmo: $(CORE_OBJ) $(UI_OBJ) build/main.o
	$(CXX) -o $@ $^ $(UI_LIBS)

fanmo-cli: $(CORE_OBJ) build/cli/main.o
	$(CXX) -o $@ $^ $(CORE_LIBS)

fanmo-tests: $(CORE_OBJ) build/tests/main.o
	$(CXX) -o $@ $^ $(CORE_LIBS)

fanmo-uitests: $(CORE_OBJ) $(UI_OBJ) build/tests/ui.o
	$(CXX) -o $@ $^ $(UI_LIBS)

test: fanmo-tests fanmo-uitests
	./fanmo-tests
	./fanmo-uitests

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -MMD -MP -o $@ $<

build/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -MMD -MP -o $@ $<

build/moc/%.cpp: src/%.h
	@mkdir -p $(dir $@)
	$(MOC) $< -o $@

build/moc/%.o: build/moc/%.cpp
	$(CXX) -c $(CXXFLAGS) -o $@ $<

-include $(shell find build -name '*.d' 2>/dev/null)

# For clangd.
compile_flags.txt: Makefile
	printf '%s\n' -xc++ $(CXXFLAGS) > $@

# Installs the programs only. It does not register fanmo as your nxm://
# handler; `fanmo-cli register-nxm` does that, when you decide to.
install: fanmo fanmo-cli
	install -Dm755 fanmo $(DESTDIR)$(PREFIX)/bin/fanmo
	install -Dm755 fanmo-cli $(DESTDIR)$(PREFIX)/bin/fanmo-cli
	install -Dm644 fanmo.desktop $(DESTDIR)$(PREFIX)/share/applications/fanmo.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/fanmo $(DESTDIR)$(PREFIX)/bin/fanmo-cli \
	      $(DESTDIR)$(PREFIX)/share/applications/fanmo.desktop

clean:
	rm -rf fanmo fanmo-cli fanmo-tests fanmo-uitests build

.PHONY: all test install uninstall clean
