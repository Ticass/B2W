#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace w2b::parallel
{
    inline unsigned Workers()
    {
        return std::max(1u, std::thread::hardware_concurrency());
    }

    // Runs work(i) for i in [0, count) on all cores; rethrows the first error.
    template<typename Work> void For(const size_t count, Work&& work)
    {
        const auto workers = static_cast<unsigned>(std::min<size_t>(Workers(), count));
        if (workers <= 1)
        {
            for (size_t i = 0; i < count; i++)
                work(i);
            return;
        }
        std::atomic<size_t> next{0};
        std::exception_ptr failure;
        std::mutex lock;
        const auto run = [&]
        {
            try
            {
                for (auto i = next++; i < count; i = next++)
                    work(i);
            }
            catch (...)
            {
                const std::lock_guard guard(lock);
                if (!failure)
                    failure = std::current_exception();
                next = count;
            }
        };
        std::vector<std::thread> threads;
        for (unsigned i = 1; i < workers; i++)
            threads.emplace_back(run);
        run();
        for (auto& thread : threads)
            thread.join();
        if (failure)
            std::rethrow_exception(failure);
    }
} // namespace w2b::parallel
