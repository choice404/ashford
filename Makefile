# The M6 build. Five artifacts: the ashc binary built by the dusk toolchain,
# the runtime shared library, the compiled hello and hello_v2 modules, and
# the C host that drives them. make smoke runs the whole pipeline and is the
# walking skeleton gate every later milestone keeps green. The runtime is
# threaded, so every gate builds with -pthread and the tsan gate reruns the
# concurrency surface under ThreadSanitizer; test-iname gates the discovery
# table and the freeze, and test-determinism proves two builds of one source
# emit byte identical module C.
#
# DUSK points at the dusk compiler. The compiler is written against dusk
# 1.5's standard library, the two parameter map above all; point DUSK at a
# 1.5.3 or newer binary when the installed one lags.

DUSK    ?= dusk
CC      ?= cc
CFLAGS  ?= -Wall -Wextra -fPIC
OUT     := target/ashc-out

# The runtime translation units a standalone gate compiles into its own binary,
# the shared library split into sources: the runtime, its wire codec, and the
# socket helpers the codec and the client path lean on. Kept as one list so a
# gate that links the runtime in gets net.c and wire.c with it.
RT_UNITS := runtime/src/runtime.c runtime/src/net.c runtime/src/wire.c runtime/src/store.c runtime/src/mesh.c

# The runtime now speaks to the store, so runtime.c references the driver in
# store.c and its vendored SQLite; every gate that links the runtime in gets
# both. RT_INC adds the amalgamation's header to the include path store.c needs,
# and RT_SQLITE is the compiled amalgamation object those gates link beside the
# instrumented units, uninstrumented the way test-store already links it.
RT_INC := -I runtime/include -I runtime/third_party
RT_SQLITE = $(SQLITE_OBJ)

# The SQLite amalgamation, compiled once into its own object and linked into
# the runtime and the store gate. It builds with warnings off and no sanitizer:
# it is a vendored library, not runtime code under review, and ASan tolerates
# an uninstrumented object linked beside instrumented ones the same way it
# tolerates a dlopened module. DQS=0 refuses double quoted string literals,
# OMIT_LOAD_EXTENSION shuts the extension door, and MEMSTATUS=0 drops the
# allocation bookkeeping the store never reads.
SQLITE_OBJ := $(OUT)/sqlite3.o
SQLITE_FLAGS := -fPIC -w -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0

RT_SO      := $(OUT)/libashrt.so
ASHC       := target/dusk-out/ashc
ASHD       := $(OUT)/ashd
MODULE     := $(OUT)/libhello.ash.so
MODULE_V2  := $(OUT)/libhello_v2.ash.so
MODULE_PAY := $(OUT)/libpayment.ash.so
MODULE_NETPAY := $(OUT)/libnet_payment.ash.so
MODULE_LANG := $(OUT)/liblang.ash.so
MODULE_STD := $(OUT)/libstd_user.ash.so
MODULE_LEDGER := $(OUT)/libledger.ash.so
MODULE_CALC := $(OUT)/libmesh_calc.ash.so
HOST       := $(OUT)/host
BIN_DEMO   := $(OUT)/main_demo

.PHONY: all smoke smoke-asan runtime compiler module host daemon test-runtime test-wire test-store-unit test-store test-store-txn test-store-fail test-store-crash test-store-stress test-store-stress-tsan test-store-python test-thread test-iname test-partial test-lang test-std test-python test-bin test-header test-proto test-determinism test-net test-net-tsan test-net2 test-net2-tsan test-net-stress test-net-stress-tsan test-net-python test-mesh-serve test-mesh-pair test-mesh-pair-tsan test-mesh-python test-mesh-stress test-mesh-stress-tsan grpc-venv test-grpc-bridge test-grpc-go test-grpc-node tsan clean

# The full suite, one gate per surface the language carries: the walking
# skeleton, the runtime's own units, the compiled language, the store, the
# network, and the mesh, with the thread sanitizer over the concurrency surface
# last. The stress and sanitizer variants of the store, network, and mesh gates
# stay out and are run on their own, since each is minutes of load on ground the
# functional gate beside it already covers.
all: smoke test-runtime test-wire test-thread test-iname test-partial test-lang test-std test-python test-bin test-header test-proto test-determinism test-store-unit test-store test-store-txn test-store-fail test-store-crash test-store-python test-net test-net2 test-net-python test-mesh-serve test-mesh-pair test-mesh-python tsan

runtime: $(RT_SO)

$(SQLITE_OBJ): runtime/third_party/sqlite3.c runtime/third_party/sqlite3.h
	@mkdir -p $(OUT)
	$(CC) $(SQLITE_FLAGS) -I runtime/third_party -c runtime/third_party/sqlite3.c -o $(SQLITE_OBJ)

$(RT_SO): runtime/src/runtime.c runtime/src/wire.c runtime/src/net.c runtime/src/store.c runtime/src/mesh.c runtime/src/ash_net.h runtime/src/ash_remote.h runtime/include/ash/ash.h runtime/include/ash/ash_abi.h runtime/include/ash/ash_wire.h runtime/include/ash/ash_store.h runtime/third_party/sqlite3.h $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -pthread -I runtime/include -I runtime/src -I runtime/third_party runtime/src/runtime.c runtime/src/wire.c runtime/src/net.c runtime/src/store.c runtime/src/mesh.c $(SQLITE_OBJ) -ldl -o $(RT_SO)

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

$(MODULE_NETPAY): $(ASHC) skeleton/net_payment.ash runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/net_payment.ash

$(MODULE_LANG): $(ASHC) skeleton/lang.ash runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/lang.ash

$(MODULE_STD): $(ASHC) skeleton/std_user.ash $(wildcard lib/ashstd/*.ash) runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/std_user.ash

$(MODULE_LEDGER): $(ASHC) skeleton/ledger.ash lib/ashstd/store.ash runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/ledger.ash

$(MODULE_CALC): $(ASHC) skeleton/mesh_calc.ash runtime/include/ash/ash_abi.h
	$(ASHC) build skeleton/mesh_calc.ash

$(BIN_DEMO): $(ASHC) skeleton/main_demo.ash $(RT_SO) runtime/include/ash/ash_abi.h
	$(ASHC) build --bin skeleton/main_demo.ash

host: $(HOST)

$(HOST): skeleton/host.c $(RT_SO)
	$(CC) $(CFLAGS) -pthread -I runtime/include skeleton/host.c -L $(OUT) -lashrt \
	    -Wl,-rpath,'$$ORIGIN' -o $(HOST)

daemon: $(ASHD)

# The network daemon: a thin main over the runtime that loads modules, loads a
# token, and makes one ash_runtime_serve call, the accept loop and the dispatch
# now living in libashrt. It links the library like any foreign host and needs
# nothing but the public header; the serve loop it drives is the same code an
# embedded server runs.
$(ASHD): runtime/src/ashd.c $(RT_SO)
	$(CC) $(CFLAGS) -pthread -I runtime/include \
	    runtime/src/ashd.c -L $(OUT) -lashrt \
	    -Wl,-rpath,'$$ORIGIN' -o $(ASHD)

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
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread $(RT_INC) \
	    tests/runtime/test_value.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_value
	./$(OUT)/test_value

# The wire codec gate under ASan: the canonical value encoding against its
# checked-in byte goldens in tests/wire, the encode(decode(b)) == b canonicity
# claim, and a negative corpus of truncations, forbidden tags, lying lengths,
# and a depth bomb, every refusal watched for leaks. Regenerate the goldens
# with ./$(OUT)/test_wire --emit tests/wire after a deliberate format change.
test-wire:
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread $(RT_INC) \
	    tests/runtime/test_wire.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_wire
	./$(OUT)/test_wire tests/wire

# The store driver gate under ASan and LSan: the SQLite backend against a real
# temp file database, the golden row dump in tests/store, the write-then-read
# round trip for every scalar type, the commit and rollback of a transaction,
# and a negative corpus of a dead dsn, broken SQL, a wrong parameter count, and
# a non scalar parameter, every refusal watched for leaks. The sqlite object is
# uninstrumented for speed, which ASan tolerates, while store.c and the test are
# under the sanitizer. RT_UNITS comes along for ash_value_eq; store.c is not in
# it. Regenerate the golden with ./$(OUT)/test_store --emit tests/store after a
# deliberate format change. Not folded into all yet; it is verified on its own.
test-store-unit:
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread $(RT_INC) \
	    tests/runtime/test_store.c $(RT_UNITS) $(RT_SQLITE) \
	    -ldl -o $(OUT)/test_store
	./$(OUT)/test_store tests/store

# The S1 store gate under ASan and LSan: the compiled Ledger signed against a
# temp SQLite file, the schema reconciled into a fresh file, a row written and
# read back, an update round tripped, a missing account answered as the
# contract's own Err, an injection string bound as a value that leaves the table
# standing, and the two refusal signs, a divergent schema (ASH_ERR_TYPE) and a
# missing dsn vow (ASH_ERR_UNBOUND). The runtime, the driver, and the
# uninstrumented sqlite object are compiled in so LSan watches every row that
# lands on the instance to zero leaks; -rdynamic exports the ash_* symbols the
# dlopened module resolves against.
test-store: $(MODULE_LEDGER) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_store_ledger.c $(RT_UNITS) $(RT_SQLITE) \
	    -ldl -o $(OUT)/test_store_ledger
	./$(OUT)/test_store_ledger

# The S2 transactional gate under ASan and LSan: the Ledger's Transfer
# subcontract driven as one all-or-nothing episode. A good transfer commits both
# writes and the file reflects the moved balances; a failed transfer rolls the
# debit back so nothing durable survives; a second call to a resolved
# transactional pledge is ASH_ERR_STATE; and a break mid transaction leaves no
# debit durable. Every persistence assertion reopens the file in a fresh
# instance so the file, not a cache, is the witness.
test-store-txn: $(MODULE_LEDGER) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_store_txn.c $(RT_UNITS) $(RT_SQLITE) \
	    -ldl -o $(OUT)/test_store_txn
	./$(OUT)/test_store_txn

# The S3 failure gate under ASan and LSan: ASH_ERR_STORE proven to be the store
# failing the runtime and nothing else, and the business boundary proven to hold
# against it. A read only connection made from a mode=ro dsn refuses every write
# with ASH_ERR_STORE, a loose insert and a transactional debit both, the latter
# rolled back; a duplicate primary key is the backend's own constraint refusal,
# ASH_ERR_STORE and distinct from a value Err, and the table stands after it; a
# contended writer that loses the file to an open transaction surfaces
# ASH_ERR_STORE rather than a stall. The guard the milestone turns on is here
# too: an overdraft is Err(Insufficient), the ledger's own rule as a value with
# an ASH_OK delivery, never once a store status. Every persistence claim reopens
# the file in a fresh instance so the file is the witness.
test-store-fail: $(MODULE_LEDGER) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_store_fail.c $(RT_UNITS) $(RT_SQLITE) \
	    -ldl -o $(OUT)/test_store_fail
	./$(OUT)/test_store_fail

# The S3 durability gate under ASan and LSan: the rollback on crash proof the
# whole store layer rests on. It forks a child that opens a transaction, buffers
# a debit, signals over a pipe, and is SIGKILLed dead in the transaction; the
# parent reopens the file and reads the account back to find the buffered debit
# gone, rolled back on the next open. A sign kill loop drives that crash over and
# over against one file, the balance never drifting and the reconcile never
# refusing the shape, so a run of crashes cannot corrupt the store. The child
# opens its own runtime because a pool does not cross a fork; it is SIGKILLed so
# no sanitizer watches its allocations, while the parent shuts down clean.
test-store-crash: $(MODULE_LEDGER) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_store_crash.c $(RT_UNITS) $(RT_SQLITE) \
	    -ldl -o $(OUT)/test_store_crash
	./$(OUT)/test_store_crash

# The S3 concurrency gate under ASan and LSan: many worker threads sign one
# Ledger instance per transfer, each its own connection to one shared file, and
# storm the pool with transactional transfers. The loser of a write race is
# ASH_ERR_STORE and an overdraft is a business Err, but every transfer is whole
# either way, so the money in the pool is conserved to the last unit no matter
# how the races fall. The gate seeds a known total, runs the storm, and reads it
# back on one thread: a drift is a half committed episode, and there is none.
test-store-stress: $(MODULE_LEDGER) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_store_stress.c $(RT_UNITS) $(RT_SQLITE) \
	    -ldl -o $(OUT)/test_store_stress
	./$(OUT)/test_store_stress

# The S3 concurrency gate under ThreadSanitizer: the same storm of many
# instances on one file, so TSan watches the pool, the waiters, and the per
# instance lock that keeps each connection single threaded. The dlopened module
# and the uninstrumented sqlite object are tolerated the way every other TSan
# gate tolerates them; -rdynamic exports the ash_* symbols the module resolves
# against. Not in the default all gate because it stands up the store under a
# sanitizer; run it explicitly.
test-store-stress-tsan: $(MODULE_LEDGER) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_store_stress.c $(RT_UNITS) $(RT_SQLITE) \
	    -ldl -o $(OUT)/test_store_stress_tsan
	./$(OUT)/test_store_stress_tsan

# The S4 Python store gate: the ctypes binding drives the compiled Ledger
# against a temp SQLite file with no C written and no generated code, the store
# twin of test-python and test-net-python. It signs with a dsn override, opens
# accounts and reads their balances, rewrites a row, commits a good transfer and
# rolls a bad one back, and asserts the same outcomes the C hosts assert, proving
# the store is invisible to a foreign host. Skips cleanly where python3 is not
# installed. ASan is not needed here, the runtime is a subprocess and the C store
# gates already watch its allocations; libashrt is the ordinary build.
test-store-python: $(RT_SO) $(MODULE_LEDGER)
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 interop/python/demo_ledger.py; \
	else \
	    echo "[test-store-python] python3 not found, skipping"; \
	fi

# The threading gate under ASan: the pool, the per-instance serialization,
# out-of-order waits, and the break race, with every allocation watched.
test-thread:
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread $(RT_INC) \
	    tests/runtime/test_thread.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_thread
	./$(OUT)/test_thread

# The iname gate under ASan: two compiled generations loaded side by side,
# exact name discovery, the version mismatch miss, the freeze, and the
# canonical dump against its golden, tests/runtime/iname.expect.
test-iname: $(MODULE) $(MODULE_V2)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_iname.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_iname
	./$(OUT)/test_iname

# The requirements gate under ASan: the compiled payment module's policy
# trees drive the per-pledge latch, the evaluator's priority order, the
# partial result surface, and the automatic break that keeps its heap for
# the errors it reports.
test-partial: $(MODULE_PAY)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_partial.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_partial
	./$(OUT)/test_partial

# The language lowering gate under ASan: the compiled gauntlet module walks
# every construct the code generator lowers, loops, assignment in all three
# lvalue forms, match with payload bindings, propagation, deep equality, and
# the out of bounds index rule.
test-lang: $(MODULE_LANG)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_lang.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_lang
	./$(OUT)/test_lang

# The standard library gate under ASan: the std_user module merges four
# ashstd modules through the loader, and the host signs MathOps, ListOps,
# and StdUser out of the one library, driving the integer and float
# arithmetic, the list reductions and their Option answers, the error sums
# in the Err box, and the sort that runs through an incorporated clause.
test-std: $(MODULE_STD)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/runtime/test_std.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_std
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
	    tests/runtime/test_header.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_header
	./$(OUT)/test_header

# The proto gate: emit-proto must reproduce every pinned golden byte for
# byte, the .proto a stock protoc consumes and the Go session wrapper that
# carries the stream whose lifetime is the instance's. The shape hash the
# wrapper pins is the same value the module registers, so a drift between
# the bridge surface and the compiled contract shows up here as a diff. The
# goldens walk the bridged type set: payment is the scalar walk, hello an
# enum sum in the Err arm, ledger Unit and a bare sum, main_demo a payload
# sum and a repeated parameter under two services, and std_user the
# multi-contract prefix rule, Option and List wrappers, and the reserved
# SignPledgeRequest disambiguation, with its wrapper pinning four session
# types out of one file. The gauntlet's Map stays a named refusal, proven
# here as a nonzero exit that writes nothing.
test-proto: $(ASHC)
	$(ASHC) emit-proto skeleton/payment.ash
	diff tests/golden/payment.proto $(OUT)/payment.proto
	diff tests/golden/payment_session.go $(OUT)/payment_session.go
	$(ASHC) emit-proto skeleton/hello.ash
	diff tests/golden/hello.proto $(OUT)/hello.proto
	$(ASHC) emit-proto skeleton/ledger.ash
	diff tests/golden/ledger.proto $(OUT)/ledger.proto
	$(ASHC) emit-proto skeleton/main_demo.ash
	diff tests/golden/main_demo.proto $(OUT)/main_demo.proto
	diff tests/golden/payment_session.ts $(OUT)/payment_session.ts
	$(ASHC) emit-proto skeleton/std_user.ash
	diff tests/golden/std_user.proto $(OUT)/std_user.proto
	diff tests/golden/std_user_session.go $(OUT)/std_user_session.go
	diff tests/golden/std_user_session.ts $(OUT)/std_user_session.ts
	rm -f $(OUT)/lang.proto
	! $(ASHC) emit-proto skeleton/lang.ash 2>/dev/null
	test ! -f $(OUT)/lang.proto
	@echo "[test-proto] ok"

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

# The network gate: ashd serves libhello on loopback under a token, and a C
# client links libashrt, connects with ash_runtime_connect, and asserts the
# remote Greeter entries landed in its iname table, that the dump hash the
# handshake checked held, and that a bad token, a forged version, and a dead
# address each land their documented refusal. The harness fixes the port,
# waits for the daemon to accept before the client dials, and kills it after,
# so the socket timing cannot make the gate flaky.
test-net: $(RT_SO) $(MODULE) $(ASHD)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -pthread -I runtime/include -I runtime/src \
	    tests/net/test_handshake.c -L $(OUT) -lashrt \
	    -Wl,-rpath,'$$ORIGIN' -o $(OUT)/test_handshake
	tests/net/run_net.sh $(ASHD) $(OUT)/test_handshake $(MODULE)

# The network gate under ThreadSanitizer: the daemon and the client both built
# with the runtime and the socket code compiled in, so TSan watches the accept
# loop, every connection thread, and the client's handshake for a race. The
# dlopened module is not instrumented, which TSan tolerates; -rdynamic exports
# the ash_* symbols it resolves against. Not in the default all gate because it
# stands up a real daemon under a sanitizer; run it explicitly.
test-net-tsan: $(MODULE)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic \
	    -I runtime/include -I runtime/src \
	    runtime/src/ashd.c runtime/src/runtime.c runtime/src/wire.c \
	    runtime/src/net.c runtime/src/store.c runtime/src/mesh.c $(RT_SQLITE) -ldl -o $(OUT)/ashd_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic \
	    -I runtime/include -I runtime/src \
	    tests/net/test_handshake.c runtime/src/runtime.c runtime/src/wire.c \
	    runtime/src/net.c runtime/src/store.c $(RT_SQLITE) -ldl -o $(OUT)/test_handshake_tsan
	tests/net/run_net.sh $(OUT)/ashd_tsan $(OUT)/test_handshake_tsan $(MODULE)

# The B0 mesh gate under ASan and LSan: a plain C host, not the ashd binary,
# stands a server up through ash_runtime_serve and a client drives it, both in
# one process over two runtimes. The runtime is compiled in so the sanitizer
# watches the accept loop, the connection threads, and the waiters the serve
# call starts, every allocation to zero leaks across the serve and the stop; the
# dlopened module is uninstrumented, which ASan tolerates, and -rdynamic exports
# the ash_* symbols it resolves against. No harness process is needed because
# serve and connect share the process: serve binds and listens before it
# returns, so the client that connects next cannot race the listen.
test-mesh-serve: $(MODULE) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/net/test_mesh_serve.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_mesh_serve
	./$(OUT)/test_mesh_serve $(MODULE)

# The B1 symmetric node gate under ASan and LSan: two runtimes in one process,
# each serving one contract and connecting to the other, so each is a provider and
# a consumer at once. Node A serves Greeter and consumes B's PaymentService, node B
# serves PaymentService and consumes A's Greeter, and two host threads drive the
# two edges at once so one runtime's serve loop and its own reader run together,
# the coexistence B1 pins. The runtime is compiled in so the sanitizer watches
# both accept loops, every connection thread and waiter, and both client readers
# across the two serves, the two connects, and the two stops; the dlopened modules
# are uninstrumented, which ASan tolerates, and -rdynamic exports the ash_* symbols
# they resolve against. No harness process is needed: the servers bind and listen
# before serve returns, so the connects that follow cannot race the listen.
test-mesh-pair: $(MODULE) $(MODULE_NETPAY) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/net/test_mesh_pair.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_mesh_pair
	./$(OUT)/test_mesh_pair $(MODULE) $(MODULE_NETPAY)

# The B1 gate under ThreadSanitizer, the main risk proof: one runtime serving on
# its connection threads and pool while its own reader thread completes a
# fulfillment it made as a client, both directions concurrent across the pair. TSan
# watches the serve side and the connect side of a single runtime for a race the
# two edges could open. The dlopened modules are not instrumented, which TSan
# tolerates; -rdynamic exports the ash_* symbols they resolve against. Explicit,
# never in the default all gate, because it runs two embedded servers under a
# sanitizer.
test-mesh-pair-tsan: $(MODULE) $(MODULE_NETPAY) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic $(RT_INC) \
	    tests/net/test_mesh_pair.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_mesh_pair_tsan
	./$(OUT)/test_mesh_pair_tsan $(MODULE) $(MODULE_NETPAY)

# The B3 N node mesh gate under ASan and LSan: three nodes in one process, three
# runtimes and three servers on three ports, each serving one contract and
# connecting to the other two, so six one directional edges form a full three node
# mesh. A storm of concurrent fulfillments crosses every edge and each result is
# checked against its owner's exact answer, the routing proof that a sign resolves
# one hop to the owner with no relay; a kill phase drops a node mid mesh and demands
# ASH_ERR_NET on exactly the dead edges' in flight waits while the others hold; and
# a memflat phase runs a serving node through a connect and drop loop so LSan proves
# the instance reclaim leaves no leak. The runtime is compiled in so the sanitizer
# watches all three accept loops, every connection thread and waiter, and all six
# client readers; the dlopened modules are uninstrumented, which ASan tolerates, and
# -rdynamic exports the ash_* symbols they resolve against. No harness process is
# needed: the three serves bind and listen before the connects begin.
test-mesh-stress: $(MODULE) $(MODULE_NETPAY) $(MODULE_CALC) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    tests/net/test_mesh_stress.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_mesh_stress
	./$(OUT)/test_mesh_stress $(MODULE) $(MODULE_NETPAY) $(MODULE_CALC)

# The B3 gate under ThreadSanitizer, the milestone's main risk proof: three nodes'
# serve sides and connect sides all running at once under a fulfillment storm, so
# TSan watches every accept loop, connection thread, waiter, and client reader on
# three runtimes for a race the six concurrent edges could open, and the kill and
# memflat phases for a teardown race across a drop. The dlopened modules are not
# instrumented, which TSan tolerates; -rdynamic exports the ash_* symbols they
# resolve against. Explicit, never in the default all gate, because it stands up
# three embedded servers under a sanitizer.
test-mesh-stress-tsan: $(MODULE) $(MODULE_NETPAY) $(MODULE_CALC) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic $(RT_INC) \
	    tests/net/test_mesh_stress.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_mesh_stress_tsan
	./$(OUT)/test_mesh_stress_tsan $(MODULE) $(MODULE_NETPAY) $(MODULE_CALC)

# The N2 transparency gate: ashd serves the payment module on loopback, and one
# client links libashrt and runs the same sign, fulfill, partial, and break
# sequence twice, once by loading the module locally and once by connecting to
# the daemon, asserting the identical outcome from each so the host code is
# proven to not care which side of the wire the contract lives on. A third phase
# kills the daemon mid fulfillment and asserts ASH_ERR_NET on the in flight
# waits. The client is handed the daemon's pid so it severs the connection
# itself, which is how the disconnect is timed against a live wait.
test-net2: $(RT_SO) $(MODULE_NETPAY) $(ASHD)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -pthread -I runtime/include -I runtime/src \
	    tests/net/test_remote.c -L $(OUT) -lashrt \
	    -Wl,-rpath,'$$ORIGIN' -o $(OUT)/test_remote
	tests/net/run_net2.sh $(ASHD) $(OUT)/test_remote $(MODULE_NETPAY)

# The N2 gate under ThreadSanitizer: the daemon and the client both built with
# the runtime and the socket code compiled in, so TSan watches the accept loop,
# every connection and waiter thread, the client's reader thread, and the
# detached-waiter versus break race across the disconnect. The dlopened module
# is not instrumented, which TSan tolerates; -rdynamic exports the ash_* symbols
# it resolves against. Not in the default all gate because it stands up a real
# daemon under a sanitizer; run it explicitly.
test-net2-tsan: $(MODULE_NETPAY)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic \
	    -I runtime/include -I runtime/src \
	    runtime/src/ashd.c runtime/src/runtime.c runtime/src/wire.c \
	    runtime/src/net.c runtime/src/store.c runtime/src/mesh.c $(RT_SQLITE) -ldl -o $(OUT)/ashd_net2_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic \
	    -I runtime/include -I runtime/src \
	    tests/net/test_remote.c runtime/src/runtime.c runtime/src/wire.c \
	    runtime/src/net.c runtime/src/store.c $(RT_SQLITE) -ldl -o $(OUT)/test_remote_tsan
	tests/net/run_net2.sh $(OUT)/ashd_net2_tsan $(OUT)/test_remote_tsan $(MODULE_NETPAY)

# The Python network gate: two ashd daemons serve the payment module on
# loopback, one under a token and one without, and the ctypes binding drives the
# same payment sequence locally and over the wire, proving a Python host changes
# by one line, a load turned into a connect, when the contract moves across the
# network. It walks the token matrix, the tokenless connect, and a kill of the
# tokened daemon mid fulfillment for the ASH_ERR_NET path. Skips cleanly where
# python3 is absent, and has no TSan variant on purpose: the TSan runtime cannot
# be mixed into an uninstrumented python3 process, so the socket concurrency
# stays covered by the C test-net2 and stress tsan gates.
test-net-python: $(RT_SO) $(MODULE_NETPAY) $(ASHD)
	@if command -v python3 >/dev/null 2>&1; then \
	    tests/net/run_net_python.sh $(ASHD) $(MODULE_NETPAY); \
	else \
	    echo "[test-net-python] python3 not found, skipping"; \
	fi

# The B2 foreign language provider gate: a Python host binds a Python function
# over the abstract PaymentService.charge and serves it as a mesh node, and a C
# consumer connects and fulfills it and reads back the value Python computed, two
# processes in two languages meeting at one contract. The harness stands the
# Python provider up in the background, waits until it listens, runs the C
# consumer against it, and asserts the cross language result; the consumer also
# dials with a wrong token and demands ASH_ERR_NET, the provider refusing it
# before any table crosses. The consumer links libashrt like any foreign host and
# drives the ordinary connect, sign, fulfill surface, unaware its answers are
# Python. Skips cleanly where python3 is absent, and has no TSan variant on
# purpose: the TSan runtime cannot be mixed into an uninstrumented python3
# process, so the serve concurrency stays covered by the C mesh-pair tsan gate.
test-mesh-python: $(RT_SO) $(MODULE_PAY)
	@mkdir -p $(OUT)
	@if command -v python3 >/dev/null 2>&1; then \
	    $(CC) $(CFLAGS) -pthread -I runtime/include \
	        tests/net/test_mesh_python.c -L $(OUT) -lashrt \
	        -Wl,-rpath,'$$ORIGIN' -o $(OUT)/test_mesh_python; \
	    tests/net/run_mesh_python.sh $(OUT)/test_mesh_python; \
	else \
	    echo "[test-mesh-python] python3 not found, skipping"; \
	fi

# The N3 resilience gate: ashd serves the payment module on loopback and one
# client hammers it from many connections at once, then tears the world down the
# two violent ways a network can. Phase one is N connections times T threads
# times K fulfillments concurrently, the load TSan needs to catch a race in the
# reader, the waiters, the pool, or the per instance lock. Phase two is a storm
# of short lived connections the daemon must survive while it keeps serving a
# steady worker. Phase three SIGKILLs the daemon mid flight and demands
# ASH_ERR_NET on every in flight wait, a clean state error after, and a clean
# shutdown of every runtime. The client bounds itself with an alarm so a hung
# teardown fails loudly. Kept out of the default all gate because it stands up a
# real daemon and kills it under load; run it explicitly, and beside its own
# ASan build to prove no leak survives the kill. Depends on the payment module
# the N2 gate already builds.
test-net-stress: $(RT_SO) $(MODULE_NETPAY) $(ASHD)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -pthread -I runtime/include -I runtime/src \
	    tests/net/test_stress.c -L $(OUT) -lashrt \
	    -Wl,-rpath,'$$ORIGIN' -o $(OUT)/test_stress
	tests/net/run_stress.sh $(ASHD) $(OUT)/test_stress $(MODULE_NETPAY)

# The N3 resilience gate under ThreadSanitizer: the daemon and the client both
# built with the runtime and the socket code compiled in, so TSan watches the
# accept loop, every connection and waiter thread, the client reader threads,
# and every teardown race the kill storm opens between the waiters, the reader,
# and instance break. The dlopened module is not instrumented, which TSan
# tolerates; -rdynamic exports the ash_* symbols it resolves against. Explicit,
# never in all, because it stands up a real daemon under a sanitizer.
test-net-stress-tsan: $(MODULE_NETPAY)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic \
	    -I runtime/include -I runtime/src \
	    runtime/src/ashd.c runtime/src/runtime.c runtime/src/wire.c \
	    runtime/src/net.c runtime/src/store.c runtime/src/mesh.c $(RT_SQLITE) -ldl -o $(OUT)/ashd_stress_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic \
	    -I runtime/include -I runtime/src \
	    tests/net/test_stress.c runtime/src/runtime.c runtime/src/wire.c \
	    runtime/src/net.c runtime/src/store.c $(RT_SQLITE) -ldl -o $(OUT)/test_stress_tsan
	tests/net/run_stress.sh $(OUT)/ashd_stress_tsan $(OUT)/test_stress_tsan $(MODULE_NETPAY)

# The same concurrency surface under ThreadSanitizer: the stress gate and the
# full host walk, runtime compiled into each binary so TSan sees every lock.
# The dlopened module is not instrumented, which TSan tolerates; the runtime
# and host side of every race is.
tsan: $(MODULE)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread $(RT_INC) \
	    tests/runtime/test_thread.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/test_thread_tsan
	./$(OUT)/test_thread_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -g -pthread -rdynamic $(RT_INC) \
	    skeleton/host.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/host_tsan
	./$(OUT)/host_tsan

# The gRPC bridge prototype: the payment contract served as a gRPC service,
# one signed instance per session, driven by a client that holds no runtime
# handle and knows its instance only by the uint64 the server issued. It
# proves the walk demo_payment.py runs in process survives a process boundary
# with the same answers, that a pledge's Err crosses as a value on an OK rpc
# while an Ashford status crosses as a gRPC code, and that an instance lives
# exactly as long as the Session stream that issued it: a killed client's
# instance is broken at once, and a client that holds its stream and says
# nothing keeps its instance however long it stays quiet. Out of the all gate
# on purpose: this is a prototype answering whether the session model works,
# not a shipped surface.
#
# grpcio is not in the system python3 here, so grpc-venv builds a venv the
# gate prefers. The gate takes the venv if it has grpc, else the system
# python3 if that has grpc, else skips clean the way test-python does.
GRPCVENV := target/grpcvenv
GRPC_GEN := target/grpc-gen
GRPC_PROTO := interop/grpc/payment_bridge.proto

grpc-venv:
	@if [ ! -x $(GRPCVENV)/bin/python ]; then \
	    echo "[grpc-venv] creating $(GRPCVENV)"; \
	    python3 -m venv $(GRPCVENV) && \
	    $(GRPCVENV)/bin/pip install --quiet --disable-pip-version-check \
	        grpcio grpcio-tools; \
	fi
	@$(GRPCVENV)/bin/python -c \
	    "import grpc; print('[grpc-venv] grpcio ' + grpc.__version__)"

# The stubs are generated, never tracked: the .proto is the source and
# grpc_tools regenerates from it, so a stale stub cannot outlive a change to
# the contract surface.
test-grpc-bridge: $(RT_SO) $(MODULE_PAY)
	@py=""; \
	if $(GRPCVENV)/bin/python -c "import grpc, grpc_tools" 2>/dev/null; then \
	    py="$(GRPCVENV)/bin/python"; \
	elif command -v python3 >/dev/null 2>&1 && \
	     python3 -c "import grpc, grpc_tools" 2>/dev/null; then \
	    py="python3"; \
	fi; \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-bridge] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	mkdir -p $(GRPC_GEN); \
	$$py -m grpc_tools.protoc -I interop/grpc \
	    --python_out=$(GRPC_GEN) --grpc_python_out=$(GRPC_GEN) \
	    $(GRPC_PROTO) || exit 1; \
	$$py interop/grpc/bridge_server.py --port 50251 & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	$$py interop/grpc/bridge_client.py --port 50251 --legacy-ttl 2.0; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

# The step 2 gate: the same server, driven by a Go client built from nothing
# but ashc's emitted artifacts. emit-proto writes the .proto and the session
# wrapper, protoc and its Go plugins turn the .proto into typed stubs, and
# the client walks the whole payment lifecycle against the Python server,
# session stream, pinned shape hash, value Err, and the dead client's
# instance collected in milliseconds. Two languages meet at one emitted
# surface with no hand written stub between them. Out of the all gate like
# the bridge gate: it needs protoc, the protoc-gen-go pair, and a Go
# toolchain, and skips clean where any is absent.
GO_DIR   := interop/grpc/go
GOPB_DIR := $(GO_DIR)/paymentpb

test-grpc-go: $(RT_SO) $(MODULE_PAY) $(ASHC)
	@py=""; \
	if $(GRPCVENV)/bin/python -c "import grpc, grpc_tools" 2>/dev/null; then \
	    py="$(GRPCVENV)/bin/python"; \
	elif command -v python3 >/dev/null 2>&1 && \
	     python3 -c "import grpc, grpc_tools" 2>/dev/null; then \
	    py="python3"; \
	fi; \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-go] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	if ! command -v go >/dev/null 2>&1 || ! command -v protoc >/dev/null 2>&1 || \
	   ! PATH="$$PATH:$$HOME/go/bin" command -v protoc-gen-go >/dev/null 2>&1 || \
	   ! PATH="$$PATH:$$HOME/go/bin" command -v protoc-gen-go-grpc >/dev/null 2>&1; then \
	    echo "[test-grpc-go] go or the protoc toolchain not found, skipping"; \
	    exit 0; \
	fi; \
	$(ASHC) emit-proto skeleton/payment.ash || exit 1; \
	mkdir -p $(GOPB_DIR) $(GRPC_GEN); \
	PATH="$$PATH:$$HOME/go/bin" protoc -I $(OUT) \
	    --go_out=$(GO_DIR) --go_opt=module=ashbridge \
	    --go-grpc_out=$(GO_DIR) --go-grpc_opt=module=ashbridge \
	    $(OUT)/payment.proto || exit 1; \
	cp $(OUT)/payment_session.go $(GOPB_DIR)/ || exit 1; \
	(cd $(GO_DIR) && go build -o ../../../$(OUT)/bridge_client_go ./client) || exit 1; \
	$$py -m grpc_tools.protoc -I interop/grpc \
	    --python_out=$(GRPC_GEN) --grpc_python_out=$(GRPC_GEN) \
	    $(GRPC_PROTO) || exit 1; \
	$$py interop/grpc/bridge_server.py --port 50252 & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	./$(OUT)/bridge_client_go -addr 127.0.0.1:50252; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

# The Node twin of the Go gate: the same server, driven by a client whose
# whole diet is two npm packages and the emitted artifacts. The .proto rides
# in through @grpc/proto-loader at runtime, no protoc and no generated stub,
# and the session rides in through the emitted TypeScript wrapper, which
# node runs directly under type stripping. This is the editor extension
# path from the usability notes, proven end to end: no native code crosses
# the marketplace boundary. Out of the all gate like the other gRPC gates,
# skipping clean where node, npm, or grpcio is absent.
NODE_DIR := interop/grpc/node

test-grpc-node: $(RT_SO) $(MODULE_PAY) $(ASHC)
	@py=""; \
	if $(GRPCVENV)/bin/python -c "import grpc, grpc_tools" 2>/dev/null; then \
	    py="$(GRPCVENV)/bin/python"; \
	elif command -v python3 >/dev/null 2>&1 && \
	     python3 -c "import grpc, grpc_tools" 2>/dev/null; then \
	    py="python3"; \
	fi; \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-node] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then \
	    echo "[test-grpc-node] node or npm not found, skipping"; \
	    exit 0; \
	fi; \
	$(ASHC) emit-proto skeleton/payment.ash || exit 1; \
	mkdir -p $(NODE_DIR)/gen $(GRPC_GEN); \
	cp $(OUT)/payment.proto $(OUT)/payment_session.ts $(NODE_DIR)/gen/ || exit 1; \
	if [ ! -d $(NODE_DIR)/node_modules ]; then \
	    (cd $(NODE_DIR) && npm install --no-audit --no-fund) || exit 1; \
	fi; \
	$$py -m grpc_tools.protoc -I interop/grpc \
	    --python_out=$(GRPC_GEN) --grpc_python_out=$(GRPC_GEN) \
	    $(GRPC_PROTO) || exit 1; \
	$$py interop/grpc/bridge_server.py --port 50254 & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	node $(NODE_DIR)/client.mjs --addr 127.0.0.1:50254; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

smoke-asan: $(MODULE)
	$(CC) $(CFLAGS) -fsanitize=address,leak -g -pthread -rdynamic $(RT_INC) \
	    skeleton/host.c $(RT_UNITS) $(RT_SQLITE) -ldl -o $(OUT)/host_asan
	./$(OUT)/host_asan

clean:
	rm -rf target
