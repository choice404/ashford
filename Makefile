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
MODULE_LANG := $(OUT)/liblang.ash.so
MODULE_STD := $(OUT)/libstd_user.ash.so
HOST       := $(OUT)/host
BIN_DEMO   := $(OUT)/main_demo

.PHONY: all smoke smoke-asan runtime compiler module host test-runtime test-wire test-thread test-iname test-partial test-lang test-std test-python test-bin test-header test-determinism tsan clean

all: smoke test-runtime test-wire test-thread test-iname test-partial test-lang test-std test-python test-bin test-header test-determinism tsan

runtime: $(RT_SO)

$(RT_SO): runtime/src/runtime.c runtime/src/wire.c runtime/include/ash/ash.h runtime/include/ash/ash_abi.h runtime/include/ash/ash_wire.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -pthread -I runtime/include runtime/src/runtime.c runtime/src/wire.c -ldl -o $(RT_SO)

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

$(MODULE_LANG): $(ASHC) skeleton/lang.ash runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/lang.ash

$(MODULE_STD): $(ASHC) skeleton/std_user.ash $(wildcard lib/ashstd/*.ash) runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/std_user.ash

$(BIN_DEMO): $(ASHC) skeleton/main_demo.ash $(RT_SO) runtime/include/ash/ash_abi.h
	$(ASHC) build --bin skeleton/main_demo.ash

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

# The wire codec gate under ASan: the canonical value encoding against its
# checked-in byte goldens in tests/wire, the encode(decode(b)) == b canonicity
# claim, and a negative corpus of truncations, forbidden tags, lying lengths,
# and a depth bomb, every refusal watched for leaks. Regenerate the goldens
# with ./$(OUT)/test_wire --emit tests/wire after a deliberate format change.
test-wire:
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -I runtime/include \
	    tests/runtime/test_wire.c runtime/src/wire.c runtime/src/runtime.c -ldl -o $(OUT)/test_wire
	./$(OUT)/test_wire tests/wire

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

# The language lowering gate under ASan: the compiled gauntlet module walks
# every construct the code generator lowers, loops, assignment in all three
# lvalue forms, match with payload bindings, propagation, deep equality, and
# the out of bounds index rule.
test-lang: $(MODULE_LANG)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic -I runtime/include \
	    tests/runtime/test_lang.c runtime/src/runtime.c -ldl -o $(OUT)/test_lang
	./$(OUT)/test_lang

# The standard library gate under ASan: the std_user module merges four
# ashstd modules through the loader, and the host signs MathOps, ListOps,
# and StdUser out of the one library, driving the integer and float
# arithmetic, the list reductions and their Option answers, the error sums
# in the Err box, and the sort that runs through an incorporated clause.
test-std: $(MODULE_STD)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic -I runtime/include \
	    tests/runtime/test_std.c runtime/src/runtime.c -ldl -o $(OUT)/test_std
	./$(OUT)/test_std

# The Python interop gate: the ctypes binding drives the payment walk and
# the by reference protocol with no C written and no generated code, which
# is the product claim that the documented ABI alone is enough for a foreign
# host. Skips cleanly where python3 is not installed. There is no TSan
# variant on purpose: the TSan runtime cannot be mixed into an
# uninstrumented python3 process, so the concurrency surface stays covered
# by the C tsan gate.
test-python: $(RT_SO) $(MODULE) $(MODULE_PAY)
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 interop/python/demo_payment.py; \
	else \
	    echo "[test-python] python3 not found, skipping"; \
	fi

# The standalone gate: build --bin turns main_demo into an executable, and
# both exits the grammar promises are asserted from the shell. Three args on
# the Ok path exit 3, the word fail takes the Err path, which must exit 1
# and carry the rendered payload on stderr while stdout stays silent.
test-bin: $(BIN_DEMO)
	./$(BIN_DEMO) a b c; test $$? -eq 3
	./$(BIN_DEMO); test $$? -eq 0
	./$(BIN_DEMO) x fail y >$(OUT)/main_demo.out 2>$(OUT)/main_demo.err; \
	    code=$$?; test $$code -eq 1 \
	    && grep -q 'asked to fail' $(OUT)/main_demo.err \
	    && test ! -s $(OUT)/main_demo.out
	@echo "[test-bin] ok"

# The header gate: emit-header must reproduce the pinned golden byte for
# byte, and the C smoke host compiles against nothing but that header and
# ash.h, resolves the mangled name it spells, and signs under its hash.
test-header: $(ASHC) $(MODULE) $(RT_SO)
	$(ASHC) emit-header skeleton/hello.ash
	diff tests/golden/hello.ash.h $(OUT)/hello.ash.h
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic \
	    -I runtime/include -I $(OUT) \
	    tests/runtime/test_header.c runtime/src/runtime.c -ldl -o $(OUT)/test_header
	./$(OUT)/test_header

# The determinism gate: two builds of the same source must emit byte
# identical module C, which is what keeps every mangled name and shape hash
# in the iname story reproducible.
test-determinism: $(ASHC) $(RT_SO)
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
	$(ASHC) build skeleton/lang.ash
	mv $(OUT)/lang.c $(OUT)/lang.c.first
	$(ASHC) build skeleton/lang.ash
	diff $(OUT)/lang.c.first $(OUT)/lang.c
	$(ASHC) build skeleton/std_user.ash
	mv $(OUT)/std_user.c $(OUT)/std_user.c.first
	$(ASHC) build skeleton/std_user.ash
	diff $(OUT)/std_user.c.first $(OUT)/std_user.c
	$(ASHC) build --bin skeleton/main_demo.ash
	mv $(OUT)/main_demo.c $(OUT)/main_demo.c.first
	mv $(OUT)/main_demo.main.c $(OUT)/main_demo.main.c.first
	$(ASHC) build --bin skeleton/main_demo.ash
	diff $(OUT)/main_demo.c.first $(OUT)/main_demo.c
	diff $(OUT)/main_demo.main.c.first $(OUT)/main_demo.main.c
	$(ASHC) emit-header skeleton/hello.ash
	mv $(OUT)/hello.ash.h $(OUT)/hello.ash.h.first
	$(ASHC) emit-header skeleton/hello.ash
	diff $(OUT)/hello.ash.h.first $(OUT)/hello.ash.h
	rm -f $(OUT)/hello.c.first $(OUT)/hello_v2.c.first $(OUT)/payment.c.first $(OUT)/lang.c.first $(OUT)/std_user.c.first
	rm -f $(OUT)/main_demo.c.first $(OUT)/main_demo.main.c.first $(OUT)/hello.ash.h.first
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
