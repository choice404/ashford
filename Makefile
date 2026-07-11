# The M0 build. Four artifacts: the ashc binary built by the dusk toolchain,
# the runtime shared library, the compiled hello module, and the C host that
# drives them. make smoke runs the whole pipeline and is the walking skeleton
# gate every later milestone keeps green.
#
# DUSK points at the dusk compiler. The PATH binary can lag the toolchain
# repo; override with DUSK="cargo run --quiet --bin dusk --" run from the
# cool-lang checkout, or with an absolute path to a newer build.

DUSK    ?= dusk
CC      ?= cc
CFLAGS  ?= -Wall -Wextra -fPIC
OUT     := target/ashc-out

RT_SO   := $(OUT)/libashrt.so
ASHC    := target/dusk-out/ashc
MODULE  := $(OUT)/libhello.ash.so
HOST    := $(OUT)/host

.PHONY: all smoke runtime compiler module host clean

all: smoke

runtime: $(RT_SO)

$(RT_SO): runtime/src/runtime.c runtime/include/ash/ash.h runtime/include/ash/ash_abi.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -I runtime/include runtime/src/runtime.c -ldl -o $(RT_SO)

compiler: $(ASHC)

$(ASHC): $(wildcard compiler/*.dusk)
	$(DUSK) build compiler/ashc.dusk

module: $(MODULE)

$(MODULE): $(ASHC) skeleton/hello.ash runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/hello.ash

host: $(HOST)

$(HOST): skeleton/host.c $(RT_SO)
	$(CC) $(CFLAGS) -I runtime/include skeleton/host.c -L $(OUT) -lashrt \
	    -Wl,-rpath,'$$ORIGIN' -o $(HOST)

smoke: $(RT_SO) $(MODULE) $(HOST)
	./$(HOST)

# The leak gate. The runtime compiles straight into the host here so LSan sees
# every allocation, and -rdynamic exports the ash_* symbols the dlopened
# module resolves against. valgrind covers the same ground where installed.
smoke-asan: $(MODULE)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -rdynamic -I runtime/include \
	    skeleton/host.c runtime/src/runtime.c -ldl -o $(OUT)/host_asan
	./$(OUT)/host_asan

clean:
	rm -rf target
