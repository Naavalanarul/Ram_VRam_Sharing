// HeapArena stands behind a hooked malloc, so the failures that matter are the
// ones that corrupt a program rather than merely waste space: two live
// allocations overlapping, a freed span not coming back, or an alignment
// promise broken.

#include <gtest/gtest.h>
#include <meminfo/client/heap_arena.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using meminfo::client::HeapArena;

namespace {
constexpr size_t kArenaBytes = 1u << 20; // 1 MiB of pretend address space

// The arena never dereferences what it manages, so a fake base is enough and
// keeps the test free of any real mapping.
uint8_t* fake_base() {
    static std::vector<uint8_t> storage(kArenaBytes);
    return storage.data();
}
} // namespace

TEST(HeapArenaTest, AllocationsDoNotOverlap) {
    HeapArena arena(fake_base(), kArenaBytes);

    struct Span { uintptr_t start; uintptr_t end; };
    std::vector<Span> spans;

    const size_t sizes[] = {64, 4096, 100, 1u << 16, 7, 333, 1u << 14};
    for (size_t size : sizes) {
        void* p = arena.allocate(size);
        ASSERT_NE(p, nullptr) << "allocation of " << size << " bytes failed";
        EXPECT_EQ(arena.block_size(p), size);
        const auto start = reinterpret_cast<uintptr_t>(p);
        spans.push_back({start, start + size});
    }

    std::sort(spans.begin(), spans.end(),
              [](const Span& a, const Span& b) { return a.start < b.start; });
    for (size_t i = 1; i < spans.size(); ++i) {
        EXPECT_GE(spans[i].start, spans[i - 1].end)
            << "allocation " << i << " overlaps the one before it";
    }
}

TEST(HeapArenaTest, HonoursAlignment) {
    HeapArena arena(fake_base(), kArenaBytes);

    for (size_t alignment : {size_t{16}, size_t{64}, size_t{4096}}) {
        // A small odd-sized allocation first, so the next one is not aligned by
        // accident.
        ASSERT_NE(arena.allocate(37), nullptr);

        void* p = arena.allocate(128, alignment);
        ASSERT_NE(p, nullptr) << "alignment " << alignment;
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignment, 0u)
            << "alignment " << alignment << " not honoured";
    }
}

TEST(HeapArenaTest, FreedSpaceIsReusableAfterCoalescing) {
    HeapArena arena(fake_base(), kArenaBytes);

    // Carve the arena into quarters, then release them in an order that only
    // yields one big block if adjacent free spans merge.
    const size_t quarter = kArenaBytes / 4;
    std::vector<void*> blocks;
    for (int i = 0; i < 4; ++i) {
        void* p = arena.allocate(quarter);
        ASSERT_NE(p, nullptr) << "quarter " << i;
        blocks.push_back(p);
    }
    EXPECT_EQ(arena.allocate(1), nullptr) << "arena reported space it does not have";

    arena.deallocate(blocks[1]);
    arena.deallocate(blocks[3]);
    arena.deallocate(blocks[0]);
    arena.deallocate(blocks[2]);

    EXPECT_EQ(arena.stats().in_use_bytes, 0u);
    EXPECT_EQ(arena.stats().live_blocks, 0u);
    EXPECT_EQ(arena.stats().largest_free_block, kArenaBytes)
        << "free blocks did not merge back into one span";

    void* whole = arena.allocate(kArenaBytes);
    EXPECT_NE(whole, nullptr) << "the whole arena is free but cannot be allocated";
}

TEST(HeapArenaTest, RejectsWhatItDoesNotOwn) {
    HeapArena arena(fake_base(), kArenaBytes);

    int stack_value = 0;
    EXPECT_FALSE(arena.owns(&stack_value));
    EXPECT_EQ(arena.block_size(&stack_value), 0u);

    // A hooked free() passes everything through here, so foreign pointers,
    // null, double frees and interior pointers must all be no-ops rather than
    // corrupting the free list.
    arena.deallocate(&stack_value);
    arena.deallocate(nullptr);

    void* p = arena.allocate(256);
    ASSERT_NE(p, nullptr);
    arena.deallocate(p);
    arena.deallocate(p); // double free
    EXPECT_EQ(arena.stats().in_use_bytes, 0u);

    void* q = arena.allocate(256);
    ASSERT_NE(q, nullptr);
    arena.deallocate(static_cast<uint8_t*>(q) + 8); // interior pointer
    EXPECT_EQ(arena.stats().in_use_bytes, 256u) << "an interior pointer freed a live block";
}

TEST(HeapArenaTest, SurvivesConcurrentUse) {
    HeapArena arena(fake_base(), kArenaBytes);

    constexpr int kThreads = 8;
    constexpr int kRounds = 400;
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&arena, &failures, t]() {
            std::mt19937 rng(static_cast<unsigned>(t));
            std::uniform_int_distribution<size_t> size_dist(16, 2048);
            std::vector<void*> held;

            for (int i = 0; i < kRounds; ++i) {
                if (held.size() > 8 || (i % 3 == 0 && !held.empty())) {
                    arena.deallocate(held.back());
                    held.pop_back();
                    continue;
                }
                const size_t bytes = size_dist(rng);
                void* p = arena.allocate(bytes);
                if (!p) continue; // legitimately full; not a failure
                if (arena.block_size(p) != bytes) ++failures;
                held.push_back(p);
            }
            for (void* p : held) arena.deallocate(p);
        });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(arena.stats().in_use_bytes, 0u) << "blocks leaked under concurrency";
    EXPECT_EQ(arena.stats().largest_free_block, kArenaBytes)
        << "arena did not return to one free span";
}
