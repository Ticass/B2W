// OpenAssetTools' Unlinker in process (see run_unlinker.h).
#include "oat/run_unlinker.h"

#include "ObjWriting.h"
#include "Unlinker.h"
#include "UnlinkerArgs.h"
#include "Utils/Logging/Log.h"

#include <iostream>
#include <mutex>
#include <sstream>

namespace w2b::oat
{
    namespace
    {
        // Points std::cout and std::cerr at one buffer while alive: OAT logs
        // only through them, and the converter reads the Unlinker's log.
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

        // UnlinkerCli's main(), with the program's global state reset first so
        // one run cannot inherit options of the previous one
        int Run(const std::vector<std::string>& arguments)
        {
            ObjWriting::Configuration = {};
            con::reset_counts();
            con::init();
            std::vector<const char*> argv{"Unlinker"};
            for (const auto& argument : arguments)
                argv.push_back(argument.c_str());
            UnlinkerArgs args;
            auto shouldContinue = true;
            if (!args.ParseArgs(static_cast<int>(argv.size()), argv.data(), shouldContinue))
                return 1;
            if (!shouldContinue)
                return 0;
            return Unlinker::Create(std::move(args))->Start() ? 0 : 1;
        }

        std::mutex runLock;

        UnlinkerRun Captured(const std::vector<std::string>& arguments)
        {
            const CapturedOutput output;
            UnlinkerRun run;
            run.exitCode = Run(arguments);
            run.log = output.Text();
            return run;
        }
    } // namespace

    UnlinkerRun RunUnlinker(const std::vector<std::string>& arguments)
    {
        const std::lock_guard guard(runLock);
        return Captured(arguments);
    }

    std::optional<UnlinkerRun> TryRunUnlinker(const std::vector<std::string>& arguments)
    {
        const std::unique_lock guard(runLock, std::try_to_lock);
        if (!guard.owns_lock())
            return std::nullopt;
        return Captured(arguments);
    }

    const char* Version()
    {
        return W2B_OAT_VERSION;
    }
} // namespace w2b::oat
