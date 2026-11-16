// Completed timeline waits retire only proven work, including concurrent/stale waits.
#undef NDEBUG
#include <atomic>
#include <cassert>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/vk_gpu_tick_cache.h"
#include "video_core/texture_cache/readback_queue.h"

static void test_readback_retirement() {
    std::atomic<std::uint64_t> completed{0};
    VideoCore::ReadbackQueue<int> pending;
    pending.Push(0x1000, 4, 3, 10);
    pending.Push(0x2000, 4, 5, 20);
    pending.Push(0x3000, 4, 9, 30);
    std::vector<int> retired;
    const auto retire = [&] {
        pending.Retire([&](auto tick) {
            return completed.load(std::memory_order_acquire) >= tick;
        }, [&](auto& entry) { retired.push_back(entry.payload); });
    };
    Vulkan::PublishWaitedGpuTick(completed, 100, false); // failed wait proves nothing
    retire();
    assert(retired.empty() && pending.Bytes() == 12);
    Vulkan::PublishWaitedGpuTick(completed, 3, true);
    retire();
    assert((retired == std::vector<int>{10}));
    assert(pending.RequiredTick(0x2000, 4) == 5 && pending.RequiredTick(0x3000, 4) == 9);
    Vulkan::PublishWaitedGpuTick(completed, 2, true); // an older wait returns late
    assert(completed.load(std::memory_order_acquire) == 3);
    Vulkan::PublishWaitedGpuTick(completed, 5, false);
    retire();
    assert((retired == std::vector<int>{10}));
    Vulkan::PublishWaitedGpuTick(completed, 5, true);
    retire();
    assert((retired == std::vector<int>{10, 20}));
    Vulkan::PublishWaitedGpuTick(completed, 8, true);
    Vulkan::PublishWaitedGpuTick(completed, 9, false);
    retire();
    assert(pending.RequiredTick(0x3000, 4) == 9 && pending.Bytes() == 4);
    assert((retired == std::vector<int>{10, 20}));
    Vulkan::PublishWaitedGpuTick(completed, 9, true);
    retire();
    assert((retired == std::vector<int>{10, 20, 30}) && pending.Bytes() == 0);

    const std::uint64_t large_tick = (std::uint64_t{1} << 32) + 7;
    Vulkan::PublishWaitedGpuTick(completed, large_tick, true);
    Vulkan::PublishWaitedGpuTick(completed, large_tick - 1, true);
    assert(completed.load(std::memory_order_acquire) == large_tick);
}

static void test_nonblocking_poll() {
    std::atomic<std::uint64_t> completed{0};
    VideoCore::ReadbackQueue<int> pending;
    pending.Push(0x1000, 4, 3, 10);
    pending.Push(0x2000, 4, 5, 20);
    pending.Push(0x3000, 4, 9, 30);
    std::vector<int> retired;
    const auto retire = [&] {
        pending.Retire([&](auto tick) {
            return completed.load(std::memory_order_acquire) >= tick;
        }, [&](auto& entry) { retired.push_back(entry.payload); });
    };
    // Scripted driver outcomes make an unintended timeout retry observable: the
    // next result is success, but that result belongs to a later explicit poll.
    const std::vector<bool> outcomes{false, true, false, true, true};
    std::vector<std::uint64_t> requested;
    const auto driver_poll = [&](std::uint64_t tick) {
        assert(requested.size() < outcomes.size());
        requested.push_back(tick);
        return outcomes[requested.size() - 1];
    };
    assert(!Vulkan::PollGpuTick(completed, 3, driver_poll));
    assert((requested == std::vector<std::uint64_t>{3}));
    retire();
    assert(completed.load(std::memory_order_acquire) == 0 && retired.empty());
    assert(pending.Bytes() == 12);

    assert(Vulkan::PollGpuTick(completed, 3, driver_poll));
    assert((requested == std::vector<std::uint64_t>{3, 3}));
    assert(completed.load(std::memory_order_acquire) == 3);
    retire();
    assert((retired == std::vector<int>{10}));
    assert(pending.RequiredTick(0x2000, 4) == 5 && pending.RequiredTick(0x3000, 4) == 9);
    assert(Vulkan::PollGpuTick(completed, 0, driver_poll));
    assert(Vulkan::PollGpuTick(completed, 2, driver_poll));
    assert(Vulkan::PollGpuTick(completed, 3, driver_poll));
    assert(requested.size() == 2); // cache hits do not touch the driver

    assert(!Vulkan::PollGpuTick(completed, 9, driver_poll));
    assert((requested == std::vector<std::uint64_t>{3, 3, 9}));
    retire();
    assert(completed.load(std::memory_order_acquire) == 3 && pending.Bytes() == 8);
    assert((retired == std::vector<int>{10}));
    assert(Vulkan::PollGpuTick(completed, 5, driver_poll));
    retire();
    assert(completed.load(std::memory_order_acquire) == 5);
    assert((retired == std::vector<int>{10, 20}));
    assert(pending.RequiredTick(0x3000, 4) == 9 && pending.Bytes() == 4);
    assert(Vulkan::PollGpuTick(completed, 9, driver_poll));
    retire();
    assert((requested == std::vector<std::uint64_t>{3, 3, 9, 5, 9}));
    assert((retired == std::vector<int>{10, 20, 30}) && pending.Bytes() == 0);
    assert(Vulkan::PollGpuTick(completed, 9, driver_poll)); // all outcomes already consumed
    assert(requested.size() == outcomes.size());

    // Interleave another confirmed waiter inside the driver call. A timed-out
    // target stays incomplete; a later successful old target cannot regress a
    // concurrently published newer completion.
    unsigned calls = 0;
    assert(!Vulkan::PollGpuTick(completed, 20, [&](auto tick) {
        assert(tick == 20);
        ++calls;
        Vulkan::PublishWaitedGpuTick(completed, 12, true);
        return false;
    }));
    assert(calls == 1 && completed.load(std::memory_order_acquire) == 12);
    assert(Vulkan::PollGpuTick(completed, 15, [&](auto tick) {
        assert(tick == 15);
        ++calls;
        Vulkan::PublishWaitedGpuTick(completed, 18, true);
        return true;
    }));
    assert(calls == 2 && completed.load(std::memory_order_acquire) == 18);
}

static void test_concurrent_completion() {
    constexpr unsigned writers = 8;
    constexpr std::uint64_t newest_success = 256;
    std::atomic<std::uint64_t> completed{0};
    std::atomic<unsigned> writers_done{0};
    std::latch ready{writers + 1}, start{1};
    VideoCore::ReadbackQueue<int> pending;
    pending.Push(0x1000, 4, 128, 1);
    pending.Push(0x2000, 4, newest_success + 1, 2);
    std::vector<int> retired;
    // Only this observer touches the queue until it is joined. Publishers model
    // different waiters and stale counter observations returning out of order.
    std::thread observer([&] {
        ready.count_down();
        start.wait();
        std::uint64_t previous = 0;
        do {
            const auto known = completed.load(std::memory_order_acquire);
            assert(known >= previous && known <= newest_success);
            previous = known;
            pending.Retire([&](auto tick) { return known >= tick; }, [&](auto& entry) {
                assert(entry.payload == 1); // newer work must remain pending
                retired.push_back(entry.payload);
            });
            std::this_thread::yield();
        } while (writers_done.load(std::memory_order_acquire) != writers);
    });
    std::vector<std::thread> publishers;
    for (unsigned writer = 0; writer < writers; ++writer) {
        publishers.emplace_back([&, writer] {
            ready.count_down();
            start.wait();
            if (writer == 0)
                Vulkan::PublishWaitedGpuTick(completed, newest_success, true);
            for (unsigned i = 0; i < 2048; ++i) {
                const auto old_tick = 1 + (i * 37 + writer * 19) % 128;
                Vulkan::PublishWaitedGpuTick(completed, old_tick, true);
                Vulkan::PublishWaitedGpuTick(completed, 1000000 + i, false);
            }
            // All threads end with an old completion, so a plain store cannot
            // accidentally pass merely because the highest waiter ran last.
            Vulkan::PublishWaitedGpuTick(completed, 1, true);
            writers_done.fetch_add(1, std::memory_order_release);
        });
    }
    ready.wait();
    start.count_down();
    for (auto& publisher : publishers) publisher.join();
    observer.join();
    assert(completed.load(std::memory_order_acquire) == newest_success);
    pending.Retire([&](auto tick) { return completed.load(std::memory_order_acquire) >= tick; },
                   [&](auto& entry) { retired.push_back(entry.payload); });
    assert((retired == std::vector<int>{1}));
    assert(pending.RequiredTick(0x2000, 4) == newest_success + 1 && pending.Bytes() == 4);
    Vulkan::PublishWaitedGpuTick(completed, newest_success + 1, true);
    pending.Retire([&](auto tick) { return completed.load(std::memory_order_acquire) >= tick; },
                   [&](auto& entry) { retired.push_back(entry.payload); });
    assert((retired == std::vector<int>{1, 2}) && pending.Bytes() == 0);
}

struct PoolTimeline {
    std::atomic<std::uint64_t> completed{0};
    std::uint64_t refreshed{};
    unsigned refreshes{};
    std::vector<bool> outcomes;
    std::vector<std::uint64_t> polled;

    std::uint64_t KnownGpuTick() const { return completed.load(std::memory_order_acquire); }
    void Refresh() {
        ++refreshes;
        Vulkan::PublishWaitedGpuTick(completed, refreshed, true);
    }
    bool Poll(std::uint64_t tick) {
        return Vulkan::PollGpuTick(completed, tick, [&](auto requested) {
            polled.push_back(requested);
            assert(polled.size() <= outcomes.size());
            return outcomes[polled.size() - 1];
        });
    }
};

static void test_pool_reuse_after_refresh() {
    PoolTimeline timeline;
    const std::vector<std::uint64_t> ticks{5, 9};
    timeline.refreshed = 5;
    // The first scan sees no free slot. After Refresh proves tick5, retry must
    // reuse slot0 rather than reporting overflow with its captured old tick0.
    const auto found = Vulkan::FindReusableGpuResource(ticks, 0, timeline, false);
    assert(found && *found == 0);
    assert(timeline.refreshes == 1 && timeline.polled.empty());

    PoolTimeline large;
    const std::uint64_t tick = (std::uint64_t{1} << 32) + 7;
    large.refreshed = tick;
    const std::vector<std::uint64_t> large_ticks{tick, tick + 2};
    const auto reused = Vulkan::FindReusableGpuResource(large_ticks, 0, large, false);
    assert(reused && *reused == 0);
}

static void test_pool_deferred_selection() {
    {
        PoolTimeline timeline;
        timeline.completed.store(3);
        const std::vector<std::uint64_t> ticks{3, 9, 12};
        const auto found = Vulkan::FindReusableGpuResource(ticks, 2, timeline, true);
        assert(found && *found == 0); // wrapped cached slot needs no driver call
        assert(timeline.refreshes == 0 && timeline.polled.empty());
    }
    {
        PoolTimeline timeline;
        timeline.outcomes = {false, true};
        const std::vector<std::uint64_t> ticks{9, 5, 12};
        assert(!Vulkan::FindReusableGpuResource(ticks, 2, timeline, true));
        assert((timeline.polled == std::vector<std::uint64_t>{5}));
        assert(timeline.KnownGpuTick() == 0); // timeout never frees a resource
        // A later explicit allocation request gets the next driver outcome.
        const auto found = Vulkan::FindReusableGpuResource(ticks, 2, timeline, true);
        assert(found && *found == 1); // oldest needed tick, not newest submission
        assert((timeline.polled == std::vector<std::uint64_t>{5, 5}));
        assert(timeline.refreshes == 0 && timeline.KnownGpuTick() == 5);
    }
    {
        PoolTimeline timeline;
        const std::vector<std::uint64_t> empty;
        assert(!Vulkan::FindReusableGpuResource(empty, 0, timeline, true));
        assert(timeline.polled.empty()); // caller must grow an empty pool
        const std::vector<std::uint64_t> unused{0, 12};
        const auto found = Vulkan::FindReusableGpuResource(unused, 0, timeline, true);
        assert(found && *found == 0 && timeline.polled.empty());
    }
}

int main() {
    test_readback_retirement();
    test_nonblocking_poll();
    test_concurrent_completion();
    test_pool_reuse_after_refresh();
    test_pool_deferred_selection();
}
