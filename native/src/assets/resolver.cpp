#include "assets/dependencies.h"

#include "core/profile.h"
#include "core/files.h"
#include "weapons/stage.h"

#include <algorithm>
#include <array>
#include <ranges>

#ifdef _WIN32
#include <windows.h>
#endif

namespace w2b::assets
{
    namespace
    {
        using py::Json;
        using Edge = std::array<std::string, 4>;
        using Names = std::set<std::string>;

        struct Source
        {
            Path file, owner;
        };

        int ZonePriority(std::string_view zone)
        {
            if (zone.starts_with("nazi_zombie"))
                return 0;
            return zone.starts_with("localized") ? 2 : 1;
        }

        bool SameComponent(const Path& left, const Path& right)
        {
#ifdef _WIN32
            return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
            return left == right;
#endif
        }

        bool IsUnder(const Path& file, const Path& root)
        {
            if (root == ".")
                return file.is_relative();
            return std::ranges::mismatch(root, file, SameComponent).in1 == root.end();
        }

        class Resolver
        {
        public:
            Resolver(const Path& base, std::vector<Path> sourceRoots, StockLookup lookup, StockSounds sounds)
                : lookup_(std::move(lookup)), stockSounds_(std::move(sounds)), reader_(base), sourceRoots_(std::move(sourceRoots))
            {
            }

            AssetGraph Run(AssetSet pending)
            {
                while (!pending.empty())
                {
                    const auto current = pending.extract(pending.begin());
                    const auto& asset = current.value();
                    if (nodes_.contains(asset))
                        continue;
                    const auto& [kind, name] = asset;
                    const auto source = [&] { const profile::Scope scope("resolver: find"); return Find(asset); }();
                    auto& node = nodes_[asset];
                    node = {{"kind", kind}, {"name", name}, {"status", source.file.empty() ? "missing_compiled_source" : "resolved"}};
                    if (source.file.empty())
                        continue;
                    node["source"] = files::Utf8(source.file);
                    AddProvenance(source.file, node);
                    const auto children = [&] { const profile::Scope scope("resolver: read dependencies"); return ReadDependencies(asset, source, node); }();
                    for (const auto& child : children)
                    {
                        SourcePath(child);
                        edges_.insert({kind, name, child.first, child.second});
                        if (!nodes_.contains(child))
                            pending.insert(child);
                    }
                }
                auto rows = Json::array();
                auto missing = Json::array();
                for (auto& [_, node] : nodes_)
                {
                    if (node.at("status") != "resolved")
                        missing.push_back(node);
                    rows.push_back(std::move(node));
                }
                auto roots = Roots();
                auto rootsJson = Json::array();
                for (const auto& root : roots)
                    rootsJson.push_back(files::Utf8(root));
                return {{{"nodes", std::move(rows)}, {"edges", edges_}, {"roots", std::move(rootsJson)}, {"missing", std::move(missing)}},
                        std::move(roots), stockRoots_};
            }

        private:
            std::vector<Path> Roots() const
            {
                auto sorted = stockRoots_;
                std::ranges::sort(sorted, {}, [](const StockRoot& hit) {
                    return std::pair{ZonePriority(hit.first), std::string_view(hit.first)};
                });
                auto roots = sourceRoots_;
                for (const auto& [_, root] : sorted)
                    roots.push_back(root);
                return roots;
            }

            std::optional<StockRoot> Fetch(const Asset& asset)
            {
                if (!lookup_)
                    return std::nullopt;
                auto result = lookup_(asset);
                if (result)
                {
                    const auto known = std::ranges::find(stockRoots_, result->first, &StockRoot::first);
                    if (known == stockRoots_.end())
                        stockRoots_.push_back(*result);
                    else
                        known->second = result->second;
                    soundAliases_.erase(result->second);
                }
                return result;
            }

            Source Find(const Asset& asset)
            {
                const auto relative = SourcePath(asset);
                const bool audio = asset.first == "loadedsound";
                for (const auto& root : sourceRoots_)
                    if (auto source = FindFile(root, relative, audio); !source.empty())
                        return {std::move(source), root};
                if (asset.first.starts_with("accuracy_"))
                {
                    // These files belong to weapons and have no stock index entries.
                    for (const auto& [_, root] : stockRoots_)
                        if (auto source = FindFile(root, relative, false); !source.empty())
                            return {std::move(source), root};
                    return {};
                }
                if (const auto stock = Fetch(asset))
                    return {FindFile(stock->second, relative, audio), stock->second};
                return {};
            }

            void AddProvenance(const Path& source, Json& node) const
            {
                const auto stock = std::ranges::find_if(stockRoots_, [&](const auto& zone) { return IsUnder(source, zone.second); });
                if (stock == stockRoots_.end())
                    node["provenance"] = "map_or_companion";
                else
                {
                    node["provenance"] = "WAW_STOCK_ASSET";
                    node["zone"] = stock->first;
                }
            }

            void AddBounce(std::string_view name, AssetSet& dependencies)
            {
                const auto prefix = std::string(name) + "_";
                dependencies.emplace("sound", prefix + "default");
                const auto add = [&](const Names& aliases) {
                    for (auto alias = aliases.lower_bound(prefix); alias != aliases.end() && alias->starts_with(prefix); ++alias)
                        dependencies.emplace("sound", *alias);
                };
                constexpr std::string_view suffix = ".w2bsnd.json";
                for (const auto& root : Roots())
                {
                    auto [entry, inserted] = soundAliases_.try_emplace(root);
                    if (inserted)
                    {
                        const auto folder = root / "soundaliases";
                        for (const auto& file : files::RGlob(folder, suffix))
                        {
                            auto alias = weapons::PosixPath(file.lexically_relative(folder));
                            if (alias.ends_with(suffix))
                                alias.resize(alias.size() - suffix.size());
                            entry->second.insert(std::move(alias));
                        }
                    }
                    add(entry->second);
                }
                if (stockSounds_)
                {
                    if (!indexedSounds_)
                        indexedSounds_ = stockSounds_();
                    add(*indexedSounds_);
                }
            }

            AssetSet ReadModel(const Asset& asset, const Source& source, Json& node)
            {
                const auto model = weapons::ReadAssetJson(source.file);
                auto dependencies = reader_.ReadModel(model);
                const auto lods = model.find("lods");
                if (lods == model.end())
                    return dependencies;
                auto roots = Roots();
                std::vector<std::pair<std::string, Path>> meshes;
                for (const auto& lod : *lods)
                {
                    const auto& file = lod.at("file").get_ref<const std::string&>();
                    meshes.emplace_back(file, weapons::FindModelLod(roots, source.owner, file));
                }
                if (std::ranges::any_of(meshes, [](const auto& mesh) { return mesh.second.empty(); }) && Fetch(asset))
                {
                    roots = Roots();
                    for (auto& [file, mesh] : meshes)
                        mesh = weapons::FindModelLod(roots, source.owner, file);
                }
                for (const auto& [file, mesh] : meshes)
                {
                    if (mesh.empty())
                    {
                        node["status"] = "missing_model_geometry";
                        node["missing_files"].push_back(file);
                        continue;
                    }
                    if (!IsUnder(mesh, source.owner / files::FromUtf8(file)))
                        node["recovered_geometry"].push_back(files::Utf8(Path(mesh).make_preferred()));
                    reader_.AddGeometry(mesh, dependencies);
                }
                return dependencies;
            }

            AssetSet ReadDependencies(const Asset& asset, const Source& source, Json& node)
            {
                if (asset.first == "weapon")
                    return reader_.ReadWeapon(source.file, [&](auto prefix, auto& found) { AddBounce(prefix, found); });
                if (asset.first == "xmodel")
                    return ReadModel(asset, source, node);
                return reader_.Read(asset.first, source.file);
            }

            StockLookup lookup_;
            StockSounds stockSounds_;
            DependencyReader reader_;
            std::vector<Path> sourceRoots_;
            std::vector<StockRoot> stockRoots_;
            std::map<Asset, Json> nodes_;
            std::set<Edge> edges_;
            std::map<Path, Names> soundAliases_;
            std::optional<Names> indexedSounds_;
        };
    }

    AssetGraph Expand(const Path& base, const std::vector<Path>& sourceRoots, const AssetSet& requests, StockLookup lookup, StockSounds sounds)
    {
        return Resolver(base, sourceRoots, std::move(lookup), std::move(sounds)).Run(requests);
    }
}
