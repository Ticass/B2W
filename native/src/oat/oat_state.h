#pragma once
// OpenAssetTools keeps process-wide state (its configuration, log counters,
// loaded containers), so every use of it in this process holds one lock and
// captures its log, which it writes only to std::cout and std::cerr.

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace w2b::oat
{
    std::mutex& StateLock();

    // Points std::cout and std::cerr at one buffer while alive.
    class CapturedOutput
    {
    public:
        CapturedOutput()
        {
            std::cout.flush();
            std::cerr.flush();
            out_ = std::cout.rdbuf(&buffer_);
            err_ = std::cerr.rdbuf(&buffer_);
        }
        ~CapturedOutput()
        {
            std::cout.rdbuf(out_);
            std::cerr.rdbuf(err_);
        }
        CapturedOutput(const CapturedOutput&) = delete;
        CapturedOutput& operator=(const CapturedOutput&) = delete;

        std::string Text() const
        {
            return buffer_.str();
        }

    private:
        std::stringbuf buffer_;
        std::streambuf* out_ = nullptr;
        std::streambuf* err_ = nullptr;
    };
} // namespace w2b::oat
