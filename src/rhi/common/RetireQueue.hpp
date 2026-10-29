#pragma once
#include "rhi/Types.hpp"

#include <array>
#include <deque>
#include <functional>
#include <mutex>

namespace rhi {
    using QueueSerials = std::array<uint64_t, kQueueCount>;

    // destroy-later list. an object is released once every queue has finished the work that was submitted
    // before Destroy() was called; no per-frame bookkeeping in callers
    class RetireQueue {
    public:
        void Push(const QueueSerials& submitted, std::function<void()> release) {
            std::lock_guard lock(mutex);
            items.push_back({submitted, std::move(release)});
        }

        // runs releases whose snapshot is covered by `completed`. releases run outside the lock
        void Collect(const QueueSerials& completed) {
            std::vector<std::function<void()>> ready;
            {
                std::lock_guard lock(mutex);
                while (!items.empty() && Covered(items.front().serials, completed)) {
                    ready.push_back(std::move(items.front().release));
                    items.pop_front();
                }
                // later entries can be covered even when an earlier one is not (queues finish out of order)
                for (auto it = items.begin(); it != items.end();) {
                    if (Covered(it->serials, completed)) {
                        ready.push_back(std::move(it->release));
                        it = items.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto& fn : ready)
                fn();
        }

        void FlushAll() {
            std::deque<Item> all;
            {
                std::lock_guard lock(mutex);
                all.swap(items);
            }
            for (auto& item : all)
                item.release();
        }

        size_t Pending() const {
            std::lock_guard lock(mutex);
            return items.size();
        }

    private:
        struct Item {
            QueueSerials serials;
            std::function<void()> release;
        };

        static bool Covered(const QueueSerials& need, const QueueSerials& done) {
            for (size_t i = 0; i < kQueueCount; i++)
                if (done[i] < need[i])
                    return false;
            return true;
        }

        mutable std::mutex mutex;
        std::deque<Item> items;
    };
}
