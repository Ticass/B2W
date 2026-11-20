#include "pipeline/menu_song.h"

#include "audio/convert.h"
#include "core/encoding.h"
#include "audio/flac_batch.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/process.h"
#include "pipeline/csvtable.h"
#include "pipeline/menu_media.h"
#include "sound/bank_schema.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

namespace w2b::pipeline
{
    namespace
    {
        namespace fs = std::filesystem;

        template<typename T> T Read(std::string_view buffer, std::size_t offset)
        {
            T value{};
            std::memcpy(&value, buffer.data() + offset, sizeof(value));
            return value;
        }

        bool PcmFormat(std::string_view format, std::string_view form)
        {
            constexpr std::array rates{8000u, 12000u, 16000u, 24000u, 32000u, 44100u, 48000u, 96000u, 192000u};
            const auto channels = Read<std::uint16_t>(format, 2);
            const auto rate = Read<std::uint32_t>(format, 4);
            return form == "WAVE" && Read<std::uint16_t>(format, 0) == 1 && Read<std::uint16_t>(format, 14) == 16 &&
                   (channels == 1 || channels == 2) && std::ranges::find(rates, rate) != rates.end() &&
                   Read<std::uint16_t>(format, 12) == channels * 2 &&
                   Read<std::uint32_t>(format, 8) == rate * channels * 2;
        }

        std::optional<std::string> ParseSong(const std::string& original, bool fromDecoder)
        {
            audio::Chunks parts;
            try
            {
                parts = audio::ParseChunks(original);
            }
            catch (const audio::SoundError&)
            {
                if (fromDecoder)
                    throw;
                return std::nullopt;
            }
            const auto& format = *parts.Find("fmt ");
            if (format.size() < 16)
                throw std::invalid_argument("Loading song has a truncated WAV format.");
            if (!PcmFormat(format, parts.form))
            {
                if (fromDecoder)
                    throw std::invalid_argument("Decoded loading song must be a mono/stereo 16-bit PCM WAV at a supported BO2 sample rate.");
                return std::nullopt;
            }
            const auto& samples = *parts.Find("data");
            if (samples.empty())
                throw std::invalid_argument("Loading song contains no audio samples.");
            return audio::CanonicalPcm(samples, Read<std::uint16_t>(format, 2), Read<std::uint32_t>(format, 4));
        }

        std::string ReadSong(const fs::path& input, const fs::path& decoder)
        {
            std::string bytes;
            if (!files::ReadBytes(input, bytes))
                throw std::invalid_argument("Cannot read loading song: " + files::Utf8(input));
            if (auto pcm = ParseSong(bytes, false); pcm)
                return std::move(*pcm);
            const auto program = audio::FindFfmpeg(decoder);
            if (program.empty())
                throw std::invalid_argument("Loading-song conversion needs FFmpeg. Use the desktop package or install FFmpeg.");
            const TemporaryMenuFile scratch;
            const auto command = process::RunMerged({files::Utf8(program), "-v", "error", "-nostdin", "-y", "-i",
                files::Utf8(fs::absolute(input)), "-map", "0:a:0", "-vn", "-ac", "2", "-ar", "48000",
                "-c:a", "pcm_s16le", "-f", "wav", files::Utf8(scratch.Path())});
            if (command.exitCode || !files::ReadBytes(scratch.Path(), bytes))
            {
                const auto decoded = encoding::Utf8Replace(command.out);
                const auto message = util::Strip(decoded);
                throw std::invalid_argument("Cannot decode loading song: " + std::string(message.substr(message.size() > 1000
                    ? message.size() - 1000 : 0)));
            }
            return ParseSong(bytes, true).value();
        }

        CsvRows SongAliases(const std::vector<std::string>& names, const std::string& relativeWave)
        {
            // BO2 linker uses this column order and the same routing as stock loading music.
            const auto defaults = ReadCsv(
                ",,,streamed,bus_music,grp_menu,snp_music,,0,0,79,79,0,5000,5000,allon,default,allon,default,1,8,"
                "oldest,oldest,0,0,100,100,0,1,2d,music_all,looping,,1,0,0,0,0,0.25,no,yes,none,0,,no,,,no,yes,no,500,"
                "5000,yes,no,,0,,no,no,no\n");
            CsvRows rows{std::vector<std::string>(sound::T6AliasColumns.begin(), sound::T6AliasColumns.end())};
            for (const auto& name : names)
            {
                auto row = defaults.front();
                row[0] = name;
                row[1] = relativeWave;
                rows.push_back(std::move(row));
            }
            return rows;
        }
    }

    void ValidateMenuSong(const launcher::Settings& settings)
    {
        if (settings.loading_song.empty())
            return;
        if (("mus_load_" + settings.project + "_patch").size() >= 64)
            throw std::invalid_argument("Loading music requires a BO2 map name of at most 48 characters.");
        const auto input = files::FromUtf8(settings.loading_song);
        if (!fs::is_regular_file(input) || fs::file_size(input) == 0)
            throw std::invalid_argument("Choose an existing, nonempty loading-song audio file.");
    }

    util::Json StageMenuSong(const launcher::Settings& settings, const fs::path& root, const fs::path& decoder)
    {
        const auto bank = "waw_" + settings.project + "_menu.all";
        const auto aliases = root / "soundbank" / files::FromUtf8(bank + ".aliases.csv");
        const auto relative = "sound/waw_menu/" + settings.project + ".wav";
        const auto wave = root / files::FromUtf8(relative);
        if (settings.loading_song.empty())
        {
            fs::remove(aliases);
            fs::remove(wave);
            return util::Json::object();
        }
        const auto data = ReadSong(files::FromUtf8(settings.loading_song), decoder);
        const std::vector<std::string> names{"mus_load_" + settings.project, "mus_load_" + settings.project + "_patch"};
        files::WriteBytes(wave, data);
        files::WriteBytes(aliases, CsvText(SongAliases(names, relative), "\r\n"));
        return {{"bank", bank}, {"aliases", names}, {"pcm", relative}, {"sha256", hash::Sha256Hex(data)}};
    }
}
