#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

// A lazy accounting budget for connection-owned and request/response payloads.
// The 80% baseline is split evenly; the remaining 20% is split by arrival age.
class ConnectionMemoryBudget {
private:
    struct allocation_record {
        std::size_t used = 0;
    };

    struct shared_state {
        explicit shared_state(std::size_t configured_limit) : limit(configured_limit) {}

        mutable std::mutex mutex;
        std::size_t limit;
        std::size_t total_used = 0;
        std::uint64_t next_id = 1;
        std::map<std::uint64_t, allocation_record> connections;

        std::size_t allowance_for_locked(std::uint64_t id) const noexcept {
            const auto found = connections.find(id);
            if (found == connections.end() || connections.empty()) return 0;

            const std::size_t count = connections.size();
            const std::size_t baseline_pool = limit / 5 * 4 + (limit % 5) * 4 / 5;
            const std::size_t age_pool = limit - baseline_pool;
            const std::size_t baseline_each = baseline_pool / count;
            std::size_t baseline_remainder = baseline_pool % count;

            std::size_t rank = 0;
            auto record = connections.begin();
            while (record != connections.end() && record->first != id) {
                ++record;
                ++rank;
            }
            if (record == connections.end()) return 0;

            // sum(1..count), evaluated in floating point to avoid integer overflow;
            // the configured server connection limit bounds practical precision needs.
            const long double total_weight =
                static_cast<long double>(count) * (static_cast<long double>(count) + 1.0L) / 2.0L;
            std::size_t assigned_age_bytes = 0;
            std::size_t age_share = 0;
            for (std::size_t age_rank = 0; age_rank < count; ++age_rank) {
                const std::size_t weight = count - age_rank;
                const std::size_t share = static_cast<std::size_t>(
                    static_cast<long double>(age_pool) * static_cast<long double>(weight) / total_weight);
                assigned_age_bytes += share;
                if (age_rank == rank) age_share = share;
            }

            const std::size_t age_remainder = age_pool - assigned_age_bytes;
            const std::size_t baseline_share = baseline_each + (rank < baseline_remainder ? 1 : 0);
            return baseline_share + age_share + (rank < age_remainder ? 1 : 0);
        }
    };

public:
    class Lease {
    private:
        std::shared_ptr<shared_state> state_;
        std::uint64_t id_ = 0;

        void unregister() noexcept {
            if (!state_ || id_ == 0) return;
            auto state = std::move(state_);
            const std::uint64_t id = std::exchange(id_, 0);
            {
                std::lock_guard lock(state->mutex);
                const auto found = state->connections.find(id);
                if (found != state->connections.end()) {
                    state->total_used -= found->second.used;
                    state->connections.erase(found);
                }
            }
        }

        Lease(std::shared_ptr<shared_state> state, std::uint64_t id)
            : state_(std::move(state)), id_(id) {}

        friend class ConnectionMemoryBudget;

    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept
            : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}

        Lease& operator=(Lease&& other) noexcept {
            if (this == &other) return *this;
            unregister();
            state_ = std::move(other.state_);
            id_ = std::exchange(other.id_, 0);
            return *this;
        }

        ~Lease() { unregister(); }

        bool try_reserve(std::size_t bytes) noexcept {
            if (bytes == 0) return true;
            if (!state_ || id_ == 0) return false;
            std::lock_guard lock(state_->mutex);
            const auto found = state_->connections.find(id_);
            if (found == state_->connections.end()) return false;
            if (bytes > state_->limit - state_->total_used) return false;
            const std::size_t allowance = state_->allowance_for_locked(id_);
            if (found->second.used > allowance || bytes > allowance - found->second.used) return false;
            found->second.used += bytes;
            state_->total_used += bytes;
            return true;
        }

        void release(std::size_t bytes) noexcept {
            if (bytes == 0 || !state_ || id_ == 0) return;
            std::lock_guard lock(state_->mutex);
            const auto found = state_->connections.find(id_);
            if (found == state_->connections.end()) return;
            const std::size_t released = std::min(bytes, found->second.used);
            found->second.used -= released;
            state_->total_used -= released;
        }

        std::size_t allowance() const noexcept {
            if (!state_ || id_ == 0) return 0;
            std::lock_guard lock(state_->mutex);
            return state_->allowance_for_locked(id_);
        }

        std::size_t used() const noexcept {
            if (!state_ || id_ == 0) return 0;
            std::lock_guard lock(state_->mutex);
            const auto found = state_->connections.find(id_);
            return found == state_->connections.end() ? 0 : found->second.used;
        }
    };

private:
    std::shared_ptr<shared_state> state_;

public:
    explicit ConnectionMemoryBudget(std::size_t limit) : state_(std::make_shared<shared_state>(limit)) {}

    ConnectionMemoryBudget(const ConnectionMemoryBudget&) = delete;
    ConnectionMemoryBudget& operator=(const ConnectionMemoryBudget&) = delete;

    Lease register_connection() {
        std::lock_guard lock(state_->mutex);
        const std::uint64_t id = state_->next_id++;
        state_->connections.emplace(id, allocation_record{});
        return Lease(state_, id);
    }

    bool set_limit(std::size_t limit) noexcept {
        if (limit == 0) return false;
        std::lock_guard lock(state_->mutex);
        if (!state_->connections.empty()) return false;
        state_->limit = limit;
        return true;
    }

    std::size_t limit() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->limit;
    }

    std::size_t used() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->total_used;
    }
};
