# bartap

A zero-heap, zero-dependency EPUB reading library in ISO C11, for microcontrollers. The caller owns all
memory and pulls content one record at a time.

**Status: under construction.** Only the foundations exist so far; there is no way to open a book yet.
`REWRITE-DECISIONS.md` is the authoritative design.

## Build and test

```sh
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build-san -DBARTAP_SANITIZE=ON && cmake --build build-san && ctest --test-dir build-san --output-on-failure
```

The tests need Python 3 and zlib. To use the library from another project, `add_subdirectory` this
repository and link `bartap`; tests are off by default in that case.
