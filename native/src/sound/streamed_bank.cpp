#include "sound/streamed_bank.h"

#include "audio/flac_batch.h"
#include "core/files.h"
#include "core/parallel.h"
#include "sound/stage.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <set>

namespace w2b::sound
{
    namespace
    {
        using util::Json;
        namespace fs = std::filesystem;

        std::string Lower(std::string text)
        {
            for (auto& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + ('a' - 'A'));
            return text;
        }

        std::set<std::string> StreamSources(const std::vector<T6AliasRow>& rows)
        {
            std::set<std::string> loaded, streams;
            for (const auto& row : rows)
                if (row["Storage"] == "loaded")
                    loaded.insert(Lower(row["FileSource"]));
            for (const auto& row : rows)
            {
                const auto& path = row["FileSource"];
                if (row["Storage"] == "streamed" && path.ends_with(".wav") && !loaded.contains(Lower(path)))
                    streams.insert(path);
            }
            return streams;
        }

        class Scratch
        {
        public:
            explicit Scratch(const fs::path& root)
            {
                static std::atomic<unsigned long long> sequence{0};
                do
                {
                    root_ = root / files::FromUtf8("waw2bo2_stream_" + std::to_string(GetCurrentProcessId()) + "_" +
                                                  std::to_string(sequence++));
                } while (!fs::create_directory(root_));
            }
            Scratch(const Scratch&) = delete;
            Scratch& operator=(const Scratch&) = delete;
            ~Scratch()
            {
                std::error_code error;
                for (const auto& file : files_)
                    fs::remove(file, error);
                fs::remove(root_, error);
            }
            fs::path Task()
            {
                auto& path = files_.emplace_back(root_ / files::FromUtf8(std::to_string(files_.size())));
                return path;
            }

        private:
            fs::path root_;
            std::vector<fs::path> files_;
        };

        fs::path ContainedPath(const fs::path& root, std::string_view relative)
        {
            auto path = fs::weakly_canonical(root / files::FromUtf8(relative));
            const auto inside = path.lexically_relative(root);
            if (inside.empty() || inside.is_absolute() || *inside.begin() == "..")
                throw SoundError("unsafe streamed audio path: " + std::string(relative));
            return path;
        }

        unsigned EncoderWorkers()
        {
            const auto* configured = std::getenv("WAW2BO2_WORKERS");
            if (!configured || !*configured)
                return std::min(8u, parallel::Workers());
            long long requested = 0;
            bool overflow = false;
            if (!util::ParseInt(configured, requested, overflow) || (!overflow && requested < 1))
                throw SoundError("WAW2BO2_WORKERS must be a positive integer");
            return overflow ? 8u : static_cast<unsigned>(std::min(8LL, requested));
        }
    }

    bool StreamFlacEnabled()
    {
        const auto* enabled = std::getenv("WAW2BO2_STREAM_FLAC");
        return enabled && std::string_view(enabled) == "1";
    }

    Json PackStreamedAudio(std::vector<T6AliasRow>& rows, const fs::path& pcmRoot, const fs::path& configuredEncoder)
    {
        const auto sources = StreamSources(rows);
        if (sources.empty())
            return {{"files", 0}, {"original_bytes", 0}, {"output_bytes", 0}};
        const auto encoder = audio::FindFfmpeg(configuredEncoder);
        if (encoder.empty())
            throw SoundError("Streamed FLAC encoding needs FFmpeg");
        const auto root = fs::canonical(pcmRoot);
        Scratch scratch(root);
        auto tasks = Json::array();
        std::vector<std::string> targets;
        std::set<std::wstring> paths;
        bool duplicate = false;
        for (const auto& name : sources)
        {
            const auto source = ContainedPath(root, name);
            targets.push_back(name.substr(0, name.size() - 4) + ".flac");
            const auto target = ContainedPath(root, targets.back());
            auto key = target.wstring();
            std::ranges::transform(key, key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            duplicate |= !paths.insert(std::move(key)).second;
            tasks.push_back({{"source", files::Utf8(source)}, {"target", files::Utf8(target)},
                             {"scratch", files::Utf8(scratch.Task())}});
        }

        const auto results = audio::EncodeFlacBatch(files::Utf8(encoder), tasks, duplicate ? 1 : EncoderWorkers());
        std::map<std::string, std::string> encoded;
        std::uint64_t originalBytes = 0, outputBytes = 0;
        std::size_t i = 0;
        for (const auto& name : sources)
        {
            const auto& entry = results.at(i);
            if (entry.at("encoded").get<bool>())
                encoded.emplace(name, targets[i]);
            originalBytes += entry.at("original_bytes").get<std::uint64_t>();
            outputBytes += entry.at("output_bytes").get<std::uint64_t>();
            ++i;
        }
        for (auto& row : rows)
            if (row["Storage"] == "streamed")
                if (const auto changed = encoded.find(row["FileSource"]); changed != encoded.end())
                    row["FileSource"] = changed->second;
        return {{"files", encoded.size()}, {"original_bytes", originalBytes}, {"output_bytes", outputBytes}};
    }
}
