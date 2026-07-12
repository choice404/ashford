# The M6 build. Five artifacts: the ashc binary built by the dusk toolchain,
# the runtime shared library, the compiled hello and hello_v2 modules, and
# the C host that drives them. make smoke runs the whole pipeline and is the
# walking skeleton gate every later milestone keeps green. The runtime is
# threaded, so every gate builds with -pthread and the tsan gate reruns the
# concurrency surface under ThreadSanitizer; test-iname gates the discovery
# table and the freeze, and test-determinism proves two builds of one source
# emit byte identical module C.
#
# DUSK points at the dusk compiler. The PATH binary can lag the toolchain
# repo; override with DUSK="cargo run --quiet --bin dusk --" run from the
# cool-lang checkout, or with an absolute path to a newer build.

DUSK    ?= dusk
CC      ?= cc
CFLAGS  ?= -Wall -Wextra -fPIC
OUT     := target/ashc-out

RT_SO      := $(OUT)/libashrt.so
ASHC       := target/dusk-out/ashc
MODULE     := $(OUT)/libhello.ash.so
MODULE_V2  := $(OUT)/libhello_v2.ash.so
MODULE_PAY := $(OUT)/libpayment.ash.so
HOST       := $(OUT)/host

.PHONY: all smoke smoke-asan runtime compiler module host test-runtime test-thread test-iname test-partial test-determinism tsan clean

all: smoke test-runtime test-thread test-iname test-partial test-determinism tsan

runtime: $(RT_SO)

$(RT_SO): runtime/src/runtime.c runtime/include/ash/ash.h runtime/include/ash/ash_abi.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -pthread -I runtime/include runtime/src/runtime.c -ldl -o $(RT_SO)

compiler: $(ASHC)

$(ASHC): $(wildcard compiler/*.dusk)
	$(DUSK) build compiler/ashc.dusk

module: $(MODULE)

$(MODULE): $(ASHC) skeleton/hello.ash runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/hello.ash

$(MODULE_V2): $(ASHC) skeleton/hello_v2.ash runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/hello_v2.ash

$(MODULE_PAY): $(ASHC) skeleton/payment.ash runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/payment.ash

host: $(HOST)

$(HOST): skeleton/host.c $(RT_SO)
	$(CC) $(CFLAGS) -pthread -I runtime/include skeleton/host.c -L $(OUT) -lashrt \
	    -Wl,-rpath,'$$ORIGIN' -o $(HOST)

smoke: $(RT_SO) $(MODULE) $(HOST)
	./$(HOST)

# The leak gate. The runtime compiles straight into the host here so LSan sees
# every allocation, and -rdynamic exports the ash_* symbols the dlopened
# module resolves against. valgrind covers the same ground where installed.
# The runtime's own unit gate: the deep value helpers, deep copy, and the
# copy-in isolation the memory model promises. Compiled with the runtime in
# one binary under ASan and LSan so every instance allocation is watched.
test-runtime:
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -I runtime/include \
	    tests/runtime/test_value.c runtime/src/runtime.c -ldl -o $(OUT)/test_value
	./$(OUT)/test_value

# The threading gate under ASan: the pool, the per-instance serialization,
# out-of-order waits, and the break race, with every allocation watched.
test-thread:
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -I runtime/include \
	    tests/runtime/test_thread.c runtime/src/runtime.c -ldl -o $(OUT)/test_thread
	./$(OUT)/test_thread

# The iname gate under ASan: two compiled generations loaded side by side,
# exact name discovery, the version mismatch miss, the freeze, and the
# canonical dump against its golden, tests/runtime/iname.expect.
test-iname: $(MODULE) $(MODULE_V2)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic -I runtime/include \
	    tests/runtime/test_iname.c runtime/src/runtime.c -ldl -o $(OUT)/test_iname
	./$(OUT)/test_iname

# The requirements gate under ASan: the compiled payment module's policy
# trees drive the per-pledge latch, the evaluator's priority order, the
# partial result surface, and the automatic break that keeps its heap for
# the errors it reports.
test-partial: $(MODULE_PAY)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic -I runtime/include \
	    tests/runtime/test_partial.c runtime/src/runtime.c -ldl -o $(OUT)/test_partial
	./$(OUT)/test_partial

# The determinism gate: two builds of the same source must emit byte
# identical module C, which is what keeps every mangled name and shape hash
# in the iname story reproducible.
test-determinism: $(ASHC)
	@mkdir -p $(OUT)
	$(ASHC) build skeleton/hello.ash
	mv $(OUT)/hello.c $(OUT)/hello.c.first
	$(ASHC) build skeleton/hello.ash
	diff $(OUT)/hello.c.first $(OUT)/hello.c
	$(ASHC) build skeleton/hello_v2.ash
	mv $(OUT)/hello_v2.c $(OUT)/hello_v2.c.first
	$(ASHC) build skeleton/hello_v2.ash
	diff $(OUT)/hello_v2.c.first $(OUT)/hello_v2.c
	$(ASHC) build skeleton/payment.ash
	mv $(OUT)/payment.c $(OUT)/payment.c.first
	$(ASHC) build skeleton/payment.ash
	diff $(OUT)/payment.c.first $(OUT)/payment.c
	rm -f $(OUT)/hello.c.first $(OUT)/hello_v2.c.first $(OUT)/payment.c.first
	@echo "[determinism] ok"

# The same concurrency surface under ThreadSanitizer: the stress gate and the
# full host walk, runtime compiled into each binary so TSan sees every lock.
# The dlopened module is not instrumented, which TSan tolerates; the runtime
# and host side of every race is.
tsan: $(MODULE)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -I runtime/include \
	    tests/runtime/test_thread.c runtime/src/runtime.c -ldl -o $(OUT)/test_thread_tsan
	./$(OUT)/test_thread_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic -I runtime/include \
	    skeleton/host.c runtime/src/runtime.c -ldl -o $(OUT)/host_tsan
	./$(OUT)/host_tsan

smoke-asan: $(MODULE)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic -I runtime/include \
	    skeleton/host.c runtime/src/runtime.c -ldl -o $(OUT)/host_asan
	./$(OUT)/host_asan

clean:
	rm -rf target
