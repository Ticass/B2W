// ipak window repair and the overheat HUD (see modsupport.h).
#include "modsupport.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/process.h"
#include "finish.h"
#include "weapons/weapon.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <tuple>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr unsigned CHUNK = 0x8000, WINDOW = 0x40000, HEADER = 128, SKIP = 0xCF, INDEX = 1, DATA = 2;

        unsigned U32(const std::string& d, const size_t at)
        {
            unsigned v = 0;
            std::memcpy(&v, d.data() + at, 4);
            return v;
        }

        void Put32(std::string& d, const size_t at, const unsigned v)
        {
            std::memcpy(d.data() + at, &v, 4);
        }

        std::vector<std::pair<size_t, unsigned>> Sections(const std::string& d)
        {
            std::vector<std::pair<size_t, unsigned>> out;
            const unsigned count = U32(d, 12);
            for (unsigned i = 0; i < count; i++)
                out.emplace_back(16 + 16 * i, U32(d, 16 + 16 * i));
            return out;
        }

        // (offset, size, count) of the section of a type
        std::tuple<unsigned, unsigned, unsigned> Section(const std::string& d, const unsigned kind)
        {
            for (const auto& [position, type] : Sections(d))
                if (type == kind)
                    return {U32(d, position + 4), U32(d, position + 8), U32(d, position + 12)};
            throw std::runtime_error("StopIteration");
        }

        size_t BlockLength(const std::string& d, const size_t position)
        {
            const unsigned count = static_cast<unsigned char>(d[position + 3]);
            size_t sum = 0;
            for (unsigned i = 0; i < count; i++)
                sum += U32(d, position + 4 + 4 * i) & 0xFFFFFF;
            return (HEADER + sum + HEADER - 1) & ~static_cast<size_t>(HEADER - 1);
        }

        std::optional<std::tuple<unsigned, size_t, size_t>> FirstViolation(const std::string& d)
        {
            const auto [indexOffset, indexSize, count] = Section(d, INDEX);
            const auto dataOffset = std::get<0>(Section(d, DATA));
            std::vector<std::pair<unsigned, unsigned>> entries;
            for (unsigned i = 0; i < count; i++)
                entries.emplace_back(U32(d, indexOffset + 16 * i + 8), i);
            std::sort(entries.begin(), entries.end());
            for (const auto& [relative, entry] : entries)
            {
                const size_t start = static_cast<size_t>(dataOffset) + relative;
                const size_t end = start + U32(d, indexOffset + 16 * entry + 12);
                const size_t grid = start & ~static_cast<size_t>(CHUNK - 1);
                size_t position = start;
                while (position < end)
                {
                    const size_t windowEnd = grid + ((position - grid) / WINDOW + 1) * WINDOW;
                    const size_t length = BlockLength(d, position);
                    if (position + length > windowEnd)
                        return std::make_tuple(entry, position, windowEnd);
                    position += length;
                }
            }
            return std::nullopt;
        }
    } // namespace

    std::vector<unsigned> RepairIpak(const fs::path& path)
    {
        std::string d;
        if (!files::ReadBytes(path, d))
            throw StepError("FileNotFoundError", "cannot read " + files::Utf8(path));
        std::vector<unsigned> inserted;
        while (const auto violation = FirstViolation(d))
        {
            const auto [entry, position, windowEnd] = *violation;
            const auto padding = static_cast<unsigned>(windowEnd - position);
            if (padding % CHUNK)
            {
                char at[32];
                std::snprintf(at, sizeof at, "%#zx", position);
                throw StepError("ValueError", files::Utf8(path.filename()) + ": block at " + at + " is not chunk aligned; cannot move it");
            }
            std::string skip(padding, '\0');
            Put32(skip, 0, 1u << 24);
            Put32(skip, 4, SKIP << 24 | (padding - HEADER));
            d.insert(position, skip);
            for (const auto& [header, kind] : Sections(d))
            {
                const unsigned sectionOffset = U32(d, header + 4), size = U32(d, header + 8);
                if (kind == DATA)
                    Put32(d, header + 8, size + padding);
                else if (sectionOffset > position)
                    Put32(d, header + 4, sectionOffset + padding);
            }
            Put32(d, 8, static_cast<unsigned>(d.size()));
            const size_t relative = position - std::get<0>(Section(d, DATA));
            const auto [indexOffset, indexSize, count] = Section(d, INDEX);
            for (unsigned i = 0; i < count; i++)
            {
                const size_t at = indexOffset + 16 * i;
                const unsigned offset = U32(d, at + 8);
                if (offset >= relative)
                    Put32(d, at + 8, offset + padding);
                else if (i == entry)
                    Put32(d, at + 12, U32(d, at + 12) + padding);
            }
            inserted.push_back(padding);
        }
        if (!inserted.empty())
            files::WriteBytes(path, d);
        return inserted;
    }

    std::string RepairOverheatBytecode(std::string data)
    {
        if (data.rfind("\x1bLua", 0) != 0)
            throw StepError("ValueError", "overheat HUD is not compiled Lua");
        for (const auto& [old, replacement] : {std::pair<std::string, std::string>{std::string("beingAnimation\0", 15), std::string("beginAnimation\0", 15)},
                                               {std::string("pluse_low\0", 10), std::string("pulse_low\0", 10)}})
        {
            size_t count = 0;
            for (size_t at = data.find(old); at != std::string::npos; at = data.find(old, at + old.size()))
                count++;
            if (count != 1)
                throw StepError("ValueError", "unrecognized stock overheat HUD constant");
            data.replace(data.find(old), old.size(), replacement);
        }
        return data;
    }

    std::vector<std::string> StageOverheatHud(const fs::path& bo2, const fs::path& project, const fs::path& work, const fs::path& unlinker)
    {
        const std::string hud = "ui_mp/t6/zombie/otherammocounters.lua";
        const auto runtime = project / "content_source/weapons.runtime.json";
        std::error_code e;
        if (!fs::is_regular_file(runtime, e))
            return {};
        bool overheats = false;
        for (const auto& name : py::Json::parse(files::ReadText(runtime)).value("weapons", py::Json::array()))
        {
            const auto file = project / "content_source/weapons" / files::FromUtf8(name.get<std::string>());
            if (fs::is_regular_file(file, e) && weapons::ReadInfo(file).Get("overheatWeapon") == "1")
            {
                overheats = true;
                break;
            }
        }
        if (!overheats)
            return {};
        const auto dump = fs::weakly_canonical(fs::absolute(work), e) / "overheat_hud_stock";
        fs::create_directories(dump);
        const auto proc = process::RunMerged({files::Utf8(fs::weakly_canonical(unlinker, e).make_preferred()), "--no-color", "--include-assets", "rawfile", "--output-folder",
                                              files::Utf8(fs::path(dump).make_preferred()), files::Utf8((bo2 / "zone/all/patch_zm.ff").make_preferred())});
        files::WriteText(dump / "extract.log", encoding::UniversalNewlines(encoding::Utf8Replace(proc.out)));
        const auto source = dump / files::FromUtf8(hud);
        std::string bytes;
        if (proc.exitCode || !files::ReadBytes(source, bytes))
            throw StepError("RuntimeError", "cannot extract stock overheat HUD; see " + files::Utf8((dump / "extract.log").make_preferred()));
        files::WriteBytes(project / files::FromUtf8(hud), RepairOverheatBytecode(bytes));
        return {"rawfile," + hud};
    }
} // namespace w2b::pipeline
