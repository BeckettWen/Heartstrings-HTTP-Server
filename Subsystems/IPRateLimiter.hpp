#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

class IPRateLimiter {
public:
    static constexpr std::size_t requests_per_window = 150;
    static constexpr std::chrono::seconds window_duration{15};

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::size_t> request_counts_;
    std::chrono::steady_clock::time_point window_started_ =
        std::chrono::steady_clock::now();

    void expire_window_locked() {
        const auto now = std::chrono::steady_clock::now();
        if (now - window_started_ >= window_duration) {
            request_counts_.clear();
            window_started_ = now;
        }
    }

public:
    // Reserve one successful-request slot. Call release_reservation() if the
    // completed response is not in the 2xx/3xx success range.
    bool reserve(const std::string& ip_address) {
        std::lock_guard lock(mutex_);
        expire_window_locked();
        const std::size_t count = request_counts_[ip_address];
        if (count >= requests_per_window) return false;
        ++request_counts_[ip_address];
        return true;
    }

    void release_reservation(const std::string& ip_address) {
        std::lock_guard lock(mutex_);
        expire_window_locked();
        auto found = request_counts_.find(ip_address);
        if (found != request_counts_.end() && found->second > 0) {
            --found->second;
            if (found->second == 0) request_counts_.erase(found);
        }
    }

    static bool is_success_status(int status_code) {
        return status_code >= 200 && status_code < 400;
    }
};
