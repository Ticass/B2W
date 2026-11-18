#pragma once
// Named scoped timers for finding hot spots without a system profiler. With
// WAW2BO2_PROFILE=1 each Scope adds its wall time and call count to a named
// total (summed across threads) and the totals print to stderr at exit,
// largest first. Otherwise a Scope costs one cached flag check.

namespace w2b::profile
{
    class Scope
    {
    public:
        // `name` must outlive the program (a string literal)
        explicit Scope(const char* name);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        const char* name_;
        long long start_ = 0;
    };

    bool Enabled();
} // namespace w2b::profile
