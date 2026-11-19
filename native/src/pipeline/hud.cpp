// The converted HUD's runtime files (see hud.h).
#include "hud.h"

#include "csvtable.h"

#include "core/files.h"
#include "core/hash.h"
#include "core/pytext.h"
#include "extract/cache.h"
#include "finish.h"
#include "modsupport.h"
#include "weapons/weapon.h"

#include <algorithm>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;

    namespace
    {
        const std::string RUNTIME = "ui_mp/t6/hud.lua", BASE = "ui_mp/t6/waw_hud_base.lua", SUPPORT = "ui_mp/t6/waw_hud_runtime.lua", MENU = "ui_mp/t6/waw_menu.lua";
        const std::string AMMO = "ui_mp/t6/zombie/ammoareazombie.lua", SCORE = "ui_mp/t6/zombie/hudcompetitivescoreboardzombie.lua",
                          OTHER = "ui_mp/t6/zombie/otherammocounters.lua";
        const std::vector<std::tuple<std::string, std::string, std::string>> WRAPPERS{{AMMO, "ui_mp/t6/zombie/waw_ammoarea_base.lua", "waw_ammoarea"},
                                                                                      {SCORE, "ui_mp/t6/zombie/waw_scoreboard_base.lua", "waw_scoreboard"},
                                                                                      {OTHER, "ui_mp/t6/zombie/waw_otherammo_base.lua", "waw_otherammo"}};
        const std::string MAPS = "zm/waw_hud_maps.csv", WEAPONS = "zm/waw_hud_weapons.csv", GLYPHS = "zm/waw_hud_glyphs.csv", MANIFEST = "content_source/waw_hud.json";
        const std::string MENU_TABLE = "zm/waw_menus.csv", MENU_GLYPHS = "zm/waw_menu_glyphs.csv", MENU_MANIFEST = "content_source/waw_menu.json";

        std::string Read(const fs::path& p)
        {
            std::string bytes;
            if (!files::ReadBytes(p, bytes))
                throw StepError("FileNotFoundError", "[Errno 2] No such file or directory: '" + files::Utf8(fs::path(p).make_preferred()) + "'");
            return bytes;
        }

        std::string CheckedBytecode(const std::string& wrapper)
        {
            const auto compat = extract::ResourceRoot() / "src/waw2bo2/compat";
            const auto source = compat / files::FromUtf8(wrapper + ".lua");
            const auto hashes = py::Json::parse(files::ReadText(compat / files::FromUtf8(wrapper + ".json")));
            const auto luac = compat / files::FromUtf8(wrapper + ".luac");
            if (hash::Sha256Hex(Read(source)) != hashes["source_sha256"].get<std::string>() || hash::Sha256Hex(Read(luac)) != hashes["bytecode_sha256"].get<std::string>())
                throw StepError("ValueError", "Recompile " + wrapper + " with tools/compile_menu_layers.ps1");
            return Read(luac);
        }

        void Write(const fs::path& path, const std::string& bytes)
        {
            fs::create_directories(path.parent_path());
            files::WriteBytes(path, bytes);
        }

        std::string Field(const py::Json& info, const char* key, const std::string& fallback)
        {
            return info.contains(key) && info[key].is_string() ? info[key].get<std::string>() : fallback;
        }
    } // namespace

    void WriteTable(const fs::path& path, const std::vector<std::vector<std::string>>& rows)
    {
        fs::create_directories(path.parent_path());
        files::WriteText(path, CsvText(rows, "\n"));
    }

    std::vector<std::string> StageHudRuntime(const fs::path& bo2, const fs::path& project, const std::string& name)
    {
        const auto manifest = project / MANIFEST;
        std::error_code e;
        if (!fs::is_regular_file(manifest, e))
            return {};
        const auto data = py::Json::parse(files::ReadText(manifest));
        const auto p = [&](const std::string& relative) { return project / files::FromUtf8(relative); };
        Write(p(SUPPORT), CheckedBytecode("waw_hud"));
        // assemble the converted HUD before the native ForceHUDRefresh
        Write(p(BASE), Read(bo2 / "raw" / files::FromUtf8(RUNTIME)));
        Write(p(RUNTIME), CheckedBytecode("waw_hud_entry"));
        for (const auto& [module, base, wrapperName] : WRAPPERS)
        {
            auto original = Read(bo2 / "raw" / files::FromUtf8(module));
            if (module == OTHER)
                original = RepairOverheatBytecode(original);
            const auto wrapper = CheckedBytecode(wrapperName);
            if (original.rfind("\x1bLua", 0) != 0 || original.substr(0, 16) != wrapper.substr(0, 16))
                throw StepError("ValueError", "Unsupported stock T6 HUD bytecode format");
            Write(p(base), original);
            Write(p(module), wrapper);
        }
        WriteTable(p(MAPS), {{name, "1"}});
        std::vector<std::vector<std::string>> rows;
        const auto runtime = project / "content_source/weapons.runtime.json";
        if (fs::is_regular_file(runtime, e))
            for (const auto& weapon : py::Json::parse(files::ReadText(runtime)).value("weapons", py::Json::array()))
            {
                const auto weaponName = weapon.get<std::string>();
                const auto info = project / "content_source/weapons" / files::FromUtf8(weaponName);
                if (!fs::is_regular_file(info, e))
                    continue;
                const auto d = weapons::ReadInfo(info);
                std::string lower = weaponName;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::string inventory = Field(d, "inventoryType", "");
                std::transform(inventory.begin(), inventory.end(), inventory.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool overheat = Field(d, "overheatWeapon", "") == "1";
                rows.push_back({weaponName, Field(d, "ammoCounterClip", "None"), Field(d, "clipSize", "0"), Field(d, "overheatWeapon", "0"),
                                inventory == "altmode" ? "" : Field(d, "displayName", ""), Field(d, "altWeapon", ""),
                                lower.find("tesla_gun") != std::string::npos ? "ammo_counter_tesla" : "", overheat ? Field(d, "hudIcon", "") : "",
                                Field(d, "maxAmmo", "0"), Field(d, "lowAmmoWarningThreshold", "0")});
            }
        WriteTable(p(WEAPONS), rows);
        Write(p(MENU), CheckedBytecode("waw_menu"));
        std::vector<std::string> tables{MAPS, WEAPONS, GLYPHS};
        std::vector<std::string> materials;
        for (const auto& m : data["materials"])
            materials.push_back(m.get<std::string>());
        if (fs::is_regular_file(p(MENU_MANIFEST), e))
        {
            const auto staged = py::Json::parse(files::ReadText(p(MENU_MANIFEST)));
            if (staged.contains("staged_materials"))
            {
                tables.push_back(MENU_TABLE);
                tables.push_back(MENU_GLYPHS);
                for (const auto& m : staged["staged_materials"])
                    if (std::find(materials.begin(), materials.end(), m.get<std::string>()) == materials.end())
                        materials.push_back(m.get<std::string>());
            }
        }
        if (std::find(tables.begin(), tables.end(), MENU_TABLE) == tables.end())
        {
            WriteTable(p(MENU_TABLE), {});
            tables.push_back(MENU_TABLE);
        }
        std::vector<std::string> lines;
        std::vector<std::string> modules{RUNTIME, BASE, SUPPORT, MENU};
        for (const auto& [module, base, _] : WRAPPERS)
            modules.push_back(module), modules.push_back(base);
        for (const auto& m : modules)
            lines.push_back("rawfile," + m);
        for (const auto& t : tables)
            lines.push_back("stringtable," + t);
        for (const auto& m : materials)
            lines.push_back("material," + m);
        return lines;
    }
} // namespace w2b::pipeline
