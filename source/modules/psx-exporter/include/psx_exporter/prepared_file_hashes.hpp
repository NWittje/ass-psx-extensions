#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace psx::exporter {
namespace prepared_hash_detail {
struct StartThread {
    template<class Work>
    std::jthread operator()(Work&& work) const {
        return std::jthread(std::forward<Work>(work));
    }
};
} // namespace prepared_hash_detail

// Startup-worker work only: never use this on a dynamics or SDK callback.
// The reader opens an independent handle/hash state for each immutable file.
// Results retain input order. Every started thread is joined before returning
// or rethrowing; no partial result may become a published cache inventory.
// The factory is an inlined construction seam for testing startup failures.
template<class Path, class Reader, class Starter = prepared_hash_detail::StartThread>
auto prepared_file_hashes(const std::vector<Path>& paths, Reader&& read,
                          Starter&& start = Starter{}) {
    using Digest = std::decay_t<std::invoke_result_t<Reader&, const Path&>>;
    std::vector<Digest> results(paths.size());
    if (paths.empty()) return results;

    constexpr std::size_t maximum_workers = 4;
    const auto workers = std::min(maximum_workers, paths.size());
    std::atomic<std::size_t> first_failure{paths.size()};
    std::atomic<bool> cancel{false};
    std::array<std::exception_ptr, maximum_workers> errors{};
    std::array<std::size_t, maximum_workers> error_at{};
    error_at.fill(paths.size());

    const auto work = [&](std::size_t worker) noexcept {
        for (auto i = worker; i < paths.size(); i += workers) {
            if (cancel.load(std::memory_order_relaxed) ||
                i >= first_failure.load(std::memory_order_relaxed)) break;
            try {
                results[i] = read(paths[i]);
            } catch (...) {
                errors[worker] = std::current_exception();
                error_at[worker] = i;
                auto previous = first_failure.load(std::memory_order_relaxed);
                while (i < previous && !first_failure.compare_exchange_weak(
                    previous, i, std::memory_order_relaxed)) {}
                break;
            }
        }
    };

    std::array<std::jthread, maximum_workers - 1> threads;
    try {
        for (std::size_t i = 1; i < workers; ++i)
            threads[i - 1] = start([&, i] { work(i); });
    } catch (...) {
        // Already admitted readers keep valid storage until they have left.
        // A thread-start failure cannot publish a partly checked namespace.
        cancel.store(true, std::memory_order_relaxed);
        for (auto& thread : threads) if (thread.joinable()) thread.join();
        throw;
    }
    work(0);
    for (auto& thread : threads) if (thread.joinable()) thread.join();

    const auto failed = first_failure.load(std::memory_order_relaxed);
    if (failed != paths.size()) {
        for (std::size_t i = 0; i < workers; ++i)
            if (error_at[i] == failed) std::rethrow_exception(errors[i]);
    }
    return results;
}
} // namespace psx::exporter
