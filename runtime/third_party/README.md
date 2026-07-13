# Vendored third party sources

## SQLite

`sqlite3.c` and `sqlite3.h` are the SQLite amalgamation, pinned so the store
driver is diagnosed against a known source and every build is reproducible with
no system dependency.

- Version: 3.46.1
- Source: https://sqlite.org/2024/sqlite-amalgamation-3460100.zip
- SHA of the pin lives in the file's own `SQLITE_VERSION` define.

The amalgamation compiles into `libashrt.so` as one translation unit. The store
driver in `runtime/src/store.c` is the only code that speaks to `sqlite3_*`
directly; everything else reaches storage through the `AshStore` vtable in
`runtime/include/ash/ash_store.h`. Replacing the pin is a deliberate act: drop
the new amalgamation here and update this note.
