# CustomMemoryAllocator

## Build and test

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Global allocation

The global `new` target replaces ordinary `new`, `new[]`, `delete`, and
`delete[]` after the caller initializes the allocator. This includes ordinary
allocations made inside dependencies used by that caller.

## Changelog

- v1.0.0 uses one ordered free list, first fit allocation, and immediate merging
- v2.0.0 searches for the smallest usable block to reduce wasted space
- v3.0.0 groups free blocks by size to reduce search work
- v4.0.0 caches small blocks per thread to avoid repeated global locking
- v5.0.0 merges adjacent blocks only when an allocation needs more space
- v6.0.0 refills caches in batches and creates reusable small block slabs
- v7.0.0 grows busy cache classes while limiting retained memory
- v8.0.0 group 4's custom mem alloc using 128bit occupancy bitmap

## v8.0.0 (Group 4)

Starts from v7.0.0. Each thread cache now keeps a 128-bit occupancy bitmap,
one bit per size class. When a request's own class is empty, the lookup jumps
to the next non-empty class with one trailing-zero count instead of walking
the empty bins one at a time. The central pool has done this since v3.0.0;
the thread cache did not.

Expected to help workloads with many different small sizes (nested stress,
linked list). Expected to do nothing for workloads that allocate one size
repeatedly (matrix array).

Build with `-DCUSTOM_MEMORY_PROFILE=ON` to count cache hits and misses, bins
visited, empty bins skipped, refills, flushes and coalescing passes. Printed to
stderr at shutdown. Off by default so timing runs contain no counting code.

Fetch with `GIT_TAG v8.0.0`, or point UsingMatrixClass at a local clone with
`-DFETCHCONTENT_SOURCE_DIR_CUSTOMMEMORYALLOCATOR=/path/to/clone`.