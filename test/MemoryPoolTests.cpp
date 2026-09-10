#include "custom_memory/MemoryPool.hpp"

#include <cassert>
#include <cstddef>
#include <iterator>
#include <new>
#include <thread>

namespace {

custom_memory::MemoryError reported_error =
    custom_memory::MemoryError::invalid_pointer;
bool error_reported = false;

void recordError(custom_memory::MemoryError error, const void*) noexcept {
    reported_error = error;
    error_reported = true;
}

}

int main() {
    auto& pool = custom_memory::MemoryPool::instance();
    assert(pool.initialize(1024 * 1024));
    pool.setErrorHandler(recordError);

    void* first = pool.allocate(128);
    void* first_guard = pool.allocate(32);
    void* second = pool.allocate(256);
    void* second_guard = pool.allocate(32);
    assert(pool.owns(first));
    assert(pool.owns(second));

    pool.deallocate(first);
    pool.deallocate(second);
    void* best_fit = pool.allocate(96);
    assert(best_fit == first);
    pool.deallocate(best_fit);
    pool.deallocate(first_guard);
    pool.deallocate(second_guard);
    assert(pool.statistics().live_allocations == 0);

    void* combined = pool.allocate(512);
    assert(pool.owns(combined));
    pool.deallocate(combined);

    void* adjacent_first = pool.allocate(1024);
    void* adjacent_second = pool.allocate(1024);
    void* adjacent_guard = pool.allocate(64);
    void* fillers[1024]{};
    std::size_t filler_count = 0;
    try {
        while (filler_count < 1024) {
            fillers[filler_count++] = pool.allocate(64);
        }
    } catch (const std::bad_alloc&) {
    }

    pool.deallocate(adjacent_first);
    pool.deallocate(adjacent_second);
    void* coalesced = pool.allocate(1800);
    assert(pool.owns(coalesced));
    pool.deallocate(coalesced);
    pool.deallocate(adjacent_guard);
    while (filler_count > 0) {
        pool.deallocate(fillers[--filler_count]);
    }

    constexpr std::size_t varied_count = 256;
    constexpr std::size_t varied_sizes[] = {
        24, 48, 96, 160, 320, 640, 1280
    };
    void* varied[varied_count]{};
    for (std::size_t round = 0; round < 64; ++round) {
        for (std::size_t index = 0; index < varied_count; ++index) {
            varied[index] = pool.allocate(
                varied_sizes[(index + round) % std::size(varied_sizes)]
            );
        }
        for (std::size_t index = 0; index < varied_count; index += 2) {
            pool.deallocate(varied[index]);
        }
        for (std::size_t index = 1; index < varied_count; index += 2) {
            pool.deallocate(varied[index]);
        }
    }
    assert(pool.statistics().live_allocations == 0);

    std::thread workers[4];
    for (std::size_t worker = 0; worker < std::size(workers); ++worker) {
        workers[worker] = std::thread([&pool, worker] {
            for (std::size_t iteration = 0; iteration < 4096; ++iteration) {
                const std::size_t bytes =
                    16 + ((iteration + worker) % 32) * 24;
                void* pointer = pool.allocate(bytes);
                pool.deallocate(pointer);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    assert(pool.statistics().live_allocations == 0);

    // Sparse thread cache: on a fresh thread (empty cache) free blocks in a
    // few widely spaced size classes, then request sizes whose OWN class is
    // empty.. every probe must be a valid, distinct pool block. With profiling
    // on, every probe must also be a cache hit that skipped at least one
    // empty bin (occupancy bitmap reason)
    {
        bool sparse_ok = true;
        std::thread([&pool, &sparse_ok] {
            const std::size_t sparse_sizes[] = {200, 1400, 3000, 5200, 7600};
            void* parked[std::size(sparse_sizes)];
            for (std::size_t index = 0; index < std::size(sparse_sizes); ++index) {
                parked[index] = pool.allocate(sparse_sizes[index]);
            }
            for (void* pointer : parked) {
                pool.deallocate(pointer);
            }
            const custom_memory::ProfileCounters before = pool.profileCounters();
            const std::size_t probe_sizes[] = {100, 1300, 2900, 5100, 7500};
            void* reused[std::size(probe_sizes)];
            for (std::size_t index = 0; index < std::size(probe_sizes); ++index) {
                reused[index] = pool.allocate(probe_sizes[index]);
                sparse_ok = sparse_ok && pool.owns(reused[index]);
                for (std::size_t other = 0; other < index; ++other) {
                    sparse_ok = sparse_ok && reused[other] != reused[index];
                }
            }
            const custom_memory::ProfileCounters after = pool.profileCounters();
            #if CUSTOM_MEMORY_PROFILE
                        sparse_ok = sparse_ok &&
                            after.cache_hits == before.cache_hits + std::size(probe_sizes);
                        sparse_ok = sparse_ok && after.cache_misses == before.cache_misses;
                        sparse_ok = sparse_ok &&
                            after.bins_skipped >= before.bins_skipped + std::size(probe_sizes);
            #else
                        (void)before;
                        (void)after;
            #endif
            for (void* pointer : reused) {
                pool.deallocate(pointer);
            }
        }).join();
        assert(sparse_ok);
    }

    // Random churn across every small size class keeps the bitmap and the
    // bin lists in step; any drift would surface as a bad pointer below.
    {
        void* churn[256] = {};
        std::size_t seed = 6252026;
        for (std::size_t iteration = 0; iteration < 20000; ++iteration) {
            seed = seed * 1103515245u + 12345u;
            const std::size_t slot = (seed >> 8) % std::size(churn);
            if (churn[slot] != nullptr) {
                pool.deallocate(churn[slot]);
                churn[slot] = nullptr;
            } else {
                const std::size_t bytes = 1 + ((seed >> 16) % 8000);
                churn[slot] = pool.allocate(bytes);
                assert(pool.owns(churn[slot]));
            }
        }
        for (void*& pointer : churn) {
            if (pointer != nullptr) {
                pool.deallocate(pointer);
                pointer = nullptr;
            }
        }
    }
    assert(pool.statistics().live_allocations == 0);
    assert(!error_reported);

    auto* bytes = static_cast<std::byte*>(pool.allocate(8));
    bytes[8] = std::byte{0};
    pool.deallocate(bytes);
    assert(error_reported);
    assert(reported_error == custom_memory::MemoryError::rear_canary_corrupted);

    pool.setErrorHandler(nullptr);
    assert(pool.shutdown());
}
