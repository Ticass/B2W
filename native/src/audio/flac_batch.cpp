#include "audio/flac_batch.h"

#include "core/files.h"
#include "core/parallel.h"
#include "core/process.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace w2b::audio
{
    namespace
    {
        template <std::size_t Size>
        std::array<unsigned char, Size> Header(const files::fs::path& path)
        {
            std::array<unsigned char, Size> bytes{};
            std::ifstream stream(path, std::ios::binary);
            if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
                throw std::runtime_error("truncated audio header: " + files::Utf8(path));
            return bytes;
        }

        void CheckFormat(const files::fs::path& source, const files::fs::path& encoded)
        {
            const auto wav = Header<44>(source);
            const auto flac = Header<26>(encoded);
            const auto text = [](const auto& header, std::size_t offset)
            { return std::string_view(reinterpret_cast<const char*>(header.data()) + offset, 4); };
            const auto little = [&](std::size_t offset)
            {
                return std::uint32_t(wav[offset]) | (std::uint32_t(wav[offset + 1]) << 8) |
                       (std::uint32_t(wav[offset + 2]) << 16) | (std::uint32_t(wav[offset + 3]) << 24);
            };
            const auto channels = little(22) & 0xffffu;
            const auto rate = little(24);
            const auto pcmBytes = little(40);
            std::uint64_t streamInfo = 0;
            for (std::size_t i = 18; i < flac.size(); ++i)
                streamInfo = (streamInfo << 8) | flac[i];
            if (text(wav, 0) != "RIFF" || text(wav, 8) != "WAVE" || text(wav, 36) != "data" ||
                !channels || text(flac, 0) != "fLaC" || (flac[4] & 0x7f) != 0 ||
                streamInfo >> 44 != rate || ((streamInfo >> 41) & 7) + 1 != channels ||
                ((streamInfo >> 36) & 31) + 1 != 16 ||
                (streamInfo & 0xfffffffffull) != pcmBytes / (2 * channels))
                throw std::runtime_error("FLAC stream format differs from source PCM: " + files::Utf8(source));
        }

        bool Publish(const files::fs::path& scratch, const files::fs::path& target)
        {
            // A background scanner can briefly hold a newly encoded file.
            for (unsigned attempt = 0; attempt < 8; ++attempt)
            {
                std::error_code error;
                files::fs::rename(scratch, target, error);
                if (!error)
                    return true;
                if (error.value() != ERROR_SHARING_VIOLATION && error.value() != ERROR_LOCK_VIOLATION)
                    throw files::fs::filesystem_error("could not publish streamed FLAC", scratch, target, error);
                std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
            }
            return false;
        }

        py::Json Encode(const std::string& encoder, const py::Json& task)
        {
            const auto source = task.at("source").get<std::string>();
            const auto target = task.at("target").get<std::string>();
            const auto scratch = task.at("scratch").get<std::string>();
            const auto sourcePath = files::FromUtf8(source);
            const auto scratchPath = files::FromUtf8(scratch);
            const auto originalBytes = files::fs::file_size(sourcePath);
            const auto encoded = process::Run({encoder, "-hide_banner", "-loglevel", "error", "-nostdin",
                "-i", source, "-map", "0:a:0", "-map_metadata", "-1", "-c:a", "flac", "-sample_fmt", "s16",
                "-threads", "1", "-compression_level", "5", "-fflags", "+bitexact",
                "-metadata_header_padding", "0", "-f", "flac", scratch}, {}, 180);
            if (encoded.exitCode != 0 || encoded.timedOut)
                throw std::runtime_error("FLAC encoding failed for " + source + ": " + encoded.err);
            CheckFormat(sourcePath, scratchPath);
            // The linker stores WAV samples without the canonical 44-byte header.
            const auto pcmBytes = originalBytes - 44;
            const auto encodedBytes = files::fs::file_size(scratchPath);
            const bool saved = encodedBytes < pcmBytes && Publish(scratchPath, files::FromUtf8(target));
            return {{"encoded", saved}, {"original_bytes", pcmBytes},
                    {"output_bytes", saved ? encodedBytes : pcmBytes}};
        }
    }

    py::Json EncodeFlacBatch(const files::fs::path& request)
    {
        const auto input = py::Json::parse(files::ReadText(request));
        const auto encoder = input.at("encoder").get<std::string>();
        const auto& tasks = input.at("tasks");
        std::vector<py::Json> results(tasks.size());
        parallel::For(tasks.size(), [&](std::size_t i) { results[i] = Encode(encoder, tasks[i]); },
                      input.at("workers").get<unsigned>());
        return results;
    }
}
