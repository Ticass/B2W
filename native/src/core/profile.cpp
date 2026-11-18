// Named scoped timers (see profile.h).
#include "core/profile.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace w2b::profile
{
    namespace
    {
        struct Total
        {
            long long ticks = 0, calls = 0;
        };

        struct Totals
        {
            std::mutex lock;
            std::map<std::string, Total> byName;
        };

        Totals& Shared()
        {
            static Totals totals;
            return totals;
        }

        long long Now()
        {
            LARGE_INTEGER counter;
            QueryPerformanceCounter(&counter);
            return counter.QuadPart;
        }

        void Report()
        {
            auto& totals = Shared();
            std::vector<std::pair<std::string, Total>> rows;
            {
                const std::lock_guard guard(totals.lock);
                rows.assign(totals.byName.begin(), totals.byName.end());
            }
            std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            std::fprintf(stderr, "== profile (WAW2BO2_PROFILE): seconds summed over threads, calls\n");
            for (const auto& [name, total] : rows)
                std::fprintf(stderr, "%10.3f s %10lld  %s\n", static_cast<double>(total.ticks) / static_cast<double>(frequency.QuadPart), total.calls, name.c_str());
            std::fflush(stderr);
        }
    } // namespace

    bool Enabled()
    {
        static const bool enabled = []
        {
            const char* flag = std::getenv("WAW2BO2_PROFILE");
            const bool on = flag && std::string(flag) == "1";
            if (on)
            {
                // the totals exist before the handler registers, so they are
                // destroyed only after it ran
                Shared();
                std::atexit(Report);
            }
            return on;
        }();
        return enabled;
    }

    Scope::Scope(const char* name) : name_(name)
    {
        if (Enabled())
            start_ = Now();
    }

    Scope::~Scope()
    {
        if (!start_)
            return;
        const auto elapsed = Now() - start_;
        auto& totals = Shared();
        const std::lock_guard guard(totals.lock);
        auto& total = totals.byName[name_];
        total.ticks += elapsed;
        total.calls++;
    }
} // namespace w2b::profile
