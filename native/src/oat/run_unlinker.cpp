// OpenAssetTools' Unlinker in process (see run_unlinker.h).
#include "oat/run_unlinker.h"

#include "oat/oat_state.h"

#include "ObjWriting.h"
#include "Unlinker.h"
#include "UnlinkerArgs.h"
#include "Utils/Logging/Log.h"

namespace w2b::oat
{
    namespace
    {
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

        UnlinkerRun Captured(const std::vector<std::string>& arguments)
        {
            const CapturedOutput output;
            UnlinkerRun run;
            run.exitCode = Run(arguments);
            run.log = output.Text();
            return run;
        }
    } // namespace

    std::mutex& StateLock()
    {
        static std::mutex lock;
        return lock;
    }

    UnlinkerRun RunUnlinker(const std::vector<std::string>& arguments)
    {
        const std::lock_guard guard(StateLock());
        return Captured(arguments);
    }

    std::optional<UnlinkerRun> TryRunUnlinker(const std::vector<std::string>& arguments)
    {
        const std::unique_lock guard(StateLock(), std::try_to_lock);
        if (!guard.owns_lock())
            return std::nullopt;
        return Captured(arguments);
    }

    const char* Version()
    {
        return W2B_OAT_VERSION;
    }
} // namespace w2b::oat
