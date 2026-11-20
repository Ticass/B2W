#include "pipeline/menu_runtime.h"

#include "core/files.h"
#include "core/hash.h"
#include "pipeline/csvtable.h"

namespace w2b::pipeline
{
    namespace
    {
        namespace fs = std::filesystem;
        using util::Json;

        struct Widget
        {
            const char* runtime;
            const char* original;
            const char* table;
            const char* wrapper;
        };

        constexpr Widget layers{"ui_mp/t6/zombie/gamemapzombie.lua", "ui_mp/t6/zombie/waw_gamemap_base.lua",
                                "zm/waw_menu_layers.csv", "menu_layers"};
        constexpr Widget preview{"ui/t6/mapinfoimage.lua", "ui/t6/waw_mapinfoimage_base.lua",
                                 "zm/waw_menu_preview.csv", "menu_preview"};

        std::string ReadBytes(const fs::path& path)
        {
            std::string bytes;
            if (!files::ReadBytes(path, bytes))
                throw std::runtime_error("cannot read " + files::Utf8(path));
            return bytes;
        }

        void ClearWidget(const fs::path& root, const Widget& widget)
        {
            for (auto file : {widget.runtime, widget.original, widget.table})
                fs::remove(root / file);
        }

        void StageWidget(const fs::path& bo2, const fs::path& root, const fs::path& compat, const Widget& widget,
                         const std::vector<std::string>& row)
        {
            const auto original = bo2 / "raw" / widget.runtime;
            const auto stock = ReadBytes(original);
            if (!stock.starts_with("\x1bLua"))
                throw std::invalid_argument("Expected stock compiled T6 menu: " + files::Utf8(original));
            const std::string name = widget.wrapper;
            const auto compiled = ReadBytes(compat / (name + ".luac"));
            const auto source = ReadBytes(compat / (name + ".lua"));
            const auto manifest = Json::parse(files::ReadText(compat / (name + ".json")));
            if (hash::Sha256Hex(compiled) != manifest.at("bytecode_sha256").get_ref<const std::string&>() ||
                hash::Sha256Hex(source) != manifest.at("source_sha256").get_ref<const std::string&>())
            {
                const auto label = name == layers.wrapper ? "Menu layer" : name;
                throw std::invalid_argument(label + " source/bytecode mismatch; rebuild with tools/compile_menu_layers.ps1.");
            }
            if (compiled.size() < 16 || stock.size() < 16 || compiled.compare(0, 16, stock, 0, 16) != 0)
                throw std::invalid_argument("Stock menu bytecode uses an unsupported T6 VM format.");
            files::WriteBytes(root / widget.original, stock);
            files::WriteBytes(root / widget.runtime, compiled);
            files::WriteText(root / widget.table, CsvText({row}, "\n"));
        }
    }

    void ClearMenuRuntime(const fs::path& root, const Json& metadata)
    {
        if (metadata.value("layer_layout", std::string{}) == "aligned")
            ClearWidget(root, layers);
        if (metadata.value("preview_layout", std::string{}) == "full")
            ClearWidget(root, preview);
    }

    Json StageMenuRuntime(const fs::path& bo2, const fs::path& root, const std::string& project, const fs::path& compat,
                          bool aligned)
    {
        auto result = Json::object();
        if (aligned)
        {
            const auto material = "menu_" + project + "_blit";
            StageWidget(bo2, root, compat, layers, {project, material});
            result = {{"layer_layout", "aligned"}, {"layer_rawfiles", {layers.original, layers.runtime}},
                      {"layer_materials", {material}}, {"blit", "menu_" + project + "_loc"}};
        }
        StageWidget(bo2, root, compat, preview, {project});
        result["preview_layout"] = "full";
        result["preview_rawfiles"] = {preview.original, preview.runtime};
        return result;
    }
}
