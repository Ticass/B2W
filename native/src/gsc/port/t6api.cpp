#include "t6api.h"

#include "core/files.h"
#include "core/text.h"
#include "gsc/tokens.h"

#include <stdexcept>

namespace w2b::gsc::port
{
    namespace
    {
        util::Json ReadJson(const std::filesystem::path& path)
        {
            std::string text;
            if (!files::ReadBytes(path, text))
                return util::Json();
            return util::Json::parse(text);
        }

        std::unordered_map<std::string, Range> Ranges(const util::Json& table)
        {
            std::unordered_map<std::string, Range> out;
            for (const auto& [name, range] : table.items())
                out[name] = {range[0].get<int>(), range[1].get<int>()};
            return out;
        }
    } // namespace

    T6Api T6Api::Load(const std::filesystem::path& snapshot, const std::filesystem::path& cache)
    {
        const auto stock = ReadJson(snapshot);
        const auto cached = ReadJson(cache);
        const auto& data = !stock.is_null() ? stock : cached; // api(stock or data, ...)
        if (data.is_null())
            throw std::runtime_error("no T6 script API snapshot or cache");
        T6Api api;
        api.builtins = Ranges(data["builtins"]);
        api.builtins.emplace("replacefunc", Range{2, 2}); // PLUTONIUM_BUILTINS, overridden by the table
        api.methods = Ranges(data["methods"]);
        for (const auto& [path, names] : data["scripts"].items())
        {
            std::map<std::string, int> functions;
            for (const auto& [name, count] : names.items())
                functions[name] = count.get<int>();
            api.scriptIndex.emplace(path, api.scripts.size()); // exact key, looked up as script.lower()
            for (const auto& [name, _] : names.items())
                api.byName[name].push_back(path);
            api.scripts.emplace_back(path, std::move(functions));
        }
        if (!cached.is_null() && cached.contains("stock_assets"))
            for (const auto& [kind, names] : cached["stock_assets"].items())
                for (const auto& name : names)
                    api.stockAssets[kind].insert(name.get<std::string>());
        return api;
    }

    bool T6Api::Defines(const std::string& script, const std::string& name) const
    {
        // scripts.get(script.lower()): keys as the snapshot stores them
        const auto it = scriptIndex.find(Lower(script));
        return it != scriptIndex.end() && scripts[it->second].second.count(name) != 0;
    }

    std::optional<Range> T6Api::BuiltinRange(const std::string& name, const std::optional<bool> method) const
    {
        const auto find = [&](const std::unordered_map<std::string, Range>& table) -> std::optional<Range>
        {
            const auto it = table.find(name);
            if (it == table.end())
                return std::nullopt;
            return it->second;
        };
        if (!method)
        {
            const auto a = find(builtins), b = find(methods);
            if (a && b)
                return Range{std::min(a->lo, b->lo), std::max(a->hi, b->hi)};
            return a ? a : b;
        }
        return find(*method ? methods : builtins);
    }

    std::optional<bool> T6Api::BuiltinAccepts(const std::string& name, const int argc, const std::optional<bool> method) const
    {
        const auto range = BuiltinRange(name, method);
        if (!range)
            return std::nullopt;
        return range->lo <= argc && argc <= range->hi;
    }

    const std::vector<std::string>& T6Api::ByName(const std::string& name) const
    {
        static const std::vector<std::string> none;
        const auto it = byName.find(name);
        return it == byName.end() ? none : it->second;
    }
} // namespace w2b::gsc::port
