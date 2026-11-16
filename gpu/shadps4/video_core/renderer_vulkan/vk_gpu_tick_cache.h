// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Vulkan {

// A successful timeline wait proves only its requested tick complete. Another
// waiter or counter query may already have published a later completion.
inline void PublishWaitedGpuTick(std::atomic<std::uint64_t>& completed,
                                 std::uint64_t waited_tick, bool wait_succeeded) noexcept {
    if (!wait_succeeded) {
        return;
    }
    auto known = completed.load(std::memory_order_relaxed);
    while (known < waited_tick &&
           !completed.compare_exchange_weak(known, waited_tick, std::memory_order_release,
                                             std::memory_order_relaxed)) {
    }
}

// The callback performs one status query for this tick, never for later work.
// A pending/error result leaves the completion cache unchanged and never retries.
template <typename Poll>
bool PollGpuTick(std::atomic<std::uint64_t>& completed, std::uint64_t requested_tick, Poll&& poll) {
    if (completed.load(std::memory_order_acquire) >= requested_tick) {
        return true;
    }
    if (!poll(requested_tick)) {
        return false;
    }
    PublishWaitedGpuTick(completed, requested_tick, true);
    return true;
}

// Select only resources whose last use is proven complete. The pool owns the
// tick array and reservation; this helper never mutates it or grows the pool.
// Timeline supplies KnownGpuTick(), Refresh() and Poll(tick). Keeping selection
// here lets the real pool's refresh/retry behavior be tested without a device.
template <typename Timeline>
std::optional<std::size_t> FindReusableGpuResource(std::span<const std::uint64_t> ticks,
                                                 std::size_t hint, Timeline& timeline,
                                                 bool defer_counter_refresh) {
    auto completed = timeline.KnownGpuTick();
    // Every retry observes the newly confirmed value, not a captured old tick.
    const auto search = [&](std::size_t begin, std::size_t end) -> std::optional<std::size_t> {
        for (auto index = begin; index < end; ++index) {
            if (completed >= ticks[index]) return index;
        }
        return std::nullopt;
    };
    auto found = search(hint, ticks.size());
    if (!found) {
        if (defer_counter_refresh) {
            // Search the wrapped cached range before asking the driver.
            found = search(0, hint);
            if (!found) {
                std::optional<std::uint64_t> needed;
                for (const auto tick : ticks) {
                    if (tick && (!needed || tick < *needed)) needed = tick;
                }
                if (needed) (void)timeline.Poll(*needed);
                completed = timeline.KnownGpuTick();
                found = search(hint, ticks.size());
            }
        } else {
            timeline.Refresh();
            completed = timeline.KnownGpuTick();
            found = search(hint, ticks.size());
        }
    }
    if (!found) found = search(0, hint);
    return found;
}

} // namespace Vulkan
