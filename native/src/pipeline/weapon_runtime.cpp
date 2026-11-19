// The weapons mod.ff carries (see weapon_runtime.h).
#include "weapon_runtime.h"

#include "mapents/map_entity.h"
#include "core/encoding.h"
#include "core/hash.h"
#include "core/pyre.h"
#include "core/unicode.h"
#include "csvtable.h"
#include "extract/cache.h"
#include "gsc/port/api.h"
#include "gsc/port/generators.h"
#include "modutil.h"
#include "sound/bank.h"
#include "textures/iwi.h"
#include "weapons/weapon.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::IsFile;
    using mod::Lower;
    using mod::StartsWith;
    using py::Json;

    namespace
    {
        const std::vector<std::pair<std::string, std::string>> PREFIXES{
            {"xmodel", "waw_xmodel/"}, {"material", "waw_material/"}, {"xanim", "waw_xanim/"}, {"accuracy", "waw_accuracy/"}};
        const std::set<std::string> ENGINE_WEAPONS{"none"};
        const std::set<std::string> BO2_PRIMARY_FRAGS{"fraggrenade", "stielhandgranate"};
        const std::set<std::string> POINTER_STRING_FIELDS{"CSPFT_STRING", "WFT_ANIM_NAME"};
        const std::string WAW_WEAPON_PREFIX = "waw_";

        // str.split(): runs of whitespace
        std::vector<std::string> SplitWords(const std::string& text)
        {
            std::vector<std::string> out;
            std::u32string cur;
            for (const auto c : unicode::Decode(text))
                if (unicode::IsSpace(c))
                {
                    if (!cur.empty())
                        out.push_back(unicode::Encode(cur)), cur.clear();
                }
                else
                    cur += c;
            if (!cur.empty())
                out.push_back(unicode::Encode(cur));
            return out;
        }

        std::string Field(const weapons::InfoFields& fields, const std::string& key)
        {
            return std::string(fields.Get(key));
        }

        // str -> its Latin-1 bytes (None when a code point is past 0xFF)
        std::optional<std::string> Latin1(const std::string& utf8)
        {
            std::string out;
            for (const auto c : unicode::Decode(utf8))
            {
                if (c > 0xFF)
                    return std::nullopt;
                out += static_cast<char>(c);
            }
            return out;
        }

        std::string FromLatin1(const std::string& bytes)
        {
            std::u32string t;
            for (const unsigned char c : bytes)
                t += static_cast<char32_t>(c);
            return unicode::Encode(t);
        }

        // animnotes.notification_offset on a padded copy: past the data is no valid suffix
        size_t NoteOffset(const std::string& raw)
        {
            if (raw.size() < 7)
                throw weapons::WeaponError("raw animation has no valid notification suffix");
            const auto padded = raw + std::string(1 << 16, '\0');
            const auto start = sound::NotificationOffsetOf(padded);
            if (start >= raw.size())
                throw weapons::WeaponError("raw animation has no valid notification suffix");
            return start;
        }

        // animnotes.add_sound_events
        std::pair<std::string, Json> AddSoundEvents(const std::string& raw, const std::vector<std::pair<std::string, std::string>>& soundMap)
        {
            std::vector<std::pair<std::string, std::string>> keys; // latin-1 note name -> alias
            bool any = false;
            for (const auto& [key, alias] : soundMap)
            {
                if (StartsWith(key, "sndnt#"))
                    continue;
                if (raw.find(key + std::string(1, '\0')) != std::string::npos)
                    any = true;
                if (const auto bytes = Latin1(key))
                    keys.emplace_back(*bytes, alias);
            }
            if (!any)
                return {raw, Json::array()};
            uint16_t version = 0;
            if (raw.size() >= 2)
                std::memcpy(&version, raw.data(), 2);
            if (raw.size() < 4 || version != 17)
                throw weapons::WeaponError("weapon sound notifications require a raw v17 animation");
            size_t start = 0, pos = 0;
            std::vector<std::pair<std::string, uint16_t>> notes;
            try
            {
                start = NoteOffset(raw);
                pos = start + 1;
                for (int n = 0; n < static_cast<unsigned char>(raw[start]); n++)
                {
                    const auto end = raw.find('\0', pos);
                    if (end == std::string::npos || end + 3 > raw.size())
                        throw weapons::WeaponError("truncated");
                    uint16_t frame;
                    std::memcpy(&frame, raw.data() + end + 1, 2);
                    notes.emplace_back(raw.substr(pos, end - pos), frame);
                    pos = end + 3;
                }
            }
            catch (const weapons::WeaponError&)
            {
                pos = std::string::npos;
            }
            if (pos != raw.size())
                throw weapons::WeaponError("raw animation has no valid notification suffix");
            const std::set<std::pair<std::string, uint16_t>> existing(notes.begin(), notes.end());
            std::vector<std::pair<std::string, uint16_t>> result;
            Json added = Json::array();
            for (const auto& [name, frame] : notes)
            {
                result.emplace_back(name, frame);
                // keys is a dict: the last mapping of a note wins
                const std::string* alias = nullptr;
                for (const auto& [k, a] : keys)
                    if (k == name)
                        alias = &a;
                if (!alias)
                    continue;
                const auto native = Latin1("sndnt#" + *alias);
                if (!native)
                    throw weapons::WeaponError("'latin-1' codec can't encode character");
                if (!existing.count({*native, frame}))
                {
                    result.emplace_back(*native, frame);
                    added.push_back(Json{{"source", FromLatin1(name)}, {"sound", *alias}, {"frame", frame}});
                }
            }
            if (added.empty())
                return {raw, Json::array()};
            if (result.size() > 254)
                throw weapons::WeaponError("converted animation exceeds notification count limit including end event");
            std::string out = raw.substr(0, start);
            out += static_cast<char>(result.size());
            for (const auto& [name, frame] : result)
            {
                out += name;
                out += '\0';
                out += static_cast<char>(frame & 0xff);
                out += static_cast<char>(frame >> 8);
            }
            return {out, added};
        }

        // animnotes.stage_weapon_events
        void StageWeaponEvents(const fs::path& content)
        {
            const auto path = content / "weapons.stage.json";
            auto stage = mod::Json(path);
            Json report = Json::object();
            for (const auto& entry : stage["weapons"])
            {
                const auto weapon = content / "weapons" / files::FromUtf8(entry["name"].get<std::string>());
                if (!IsFile(weapon))
                    continue;
                auto fields = weapons::ReadInfo(weapon);
                std::vector<std::pair<std::string, std::string>> soundMap;
                const auto soundMapText = Field(fields, "notetrackSoundMap"); // SplitLines views into it
                for (const auto& line : py::SplitLines(soundMapText))
                {
                    const auto words = SplitWords(std::string(line));
                    if (words.empty())
                        continue;
                    if (words.size() != 2)
                        throw StepError("ValueError", "dictionary update sequence element has length " + std::to_string(words.size()) + "; 2 is required");
                    const std::string key(words[0]), value(words[1]);
                    auto it = std::find_if(soundMap.begin(), soundMap.end(), [&](const auto& p) { return p.first == key; });
                    if (it == soundMap.end())
                        soundMap.emplace_back(key, value);
                    else
                        it->second = value;
                }
                if (soundMap.empty())
                    continue;
                std::vector<std::string> keys;
                for (const auto& [key, _] : fields)
                    keys.push_back(key);
                for (const auto& key : keys)
                {
                    const auto name = Field(fields, key);
                    if (!StartsWith(name, "waw_xanim/"))
                        continue;
                    const auto source = content / "xanim" / files::FromUtf8(name);
                    if (!IsFile(source))
                        continue;
                    const auto [raw, added] = AddSoundEvents(mod::Bytes(source), soundMap);
                    if (added.empty())
                        continue;
                    // Shared animations may map the same note to different weapon sounds.
                    const auto digest = hash::Sha256Hex(raw).substr(0, 16);
                    const auto targetName = weapons::OutputName("xanim", "sound_events/" + digest + "/" + name.substr(std::string("waw_xanim/").size()));
                    files::WriteBytes(content / "xanim" / files::FromUtf8(targetName), raw);
                    fields[key] = targetName;
                    report[targetName] = Json{{"source", name}, {"added", added}};
                }
                files::WriteText(weapon, weapons::WriteInfo(fields));
            }
            stage["animation_sound_events"] = report;
            files::WriteText(path, py::JsonDumps(stage, 2) + "\n");
        }

        // weapons.field_schema in header order
        std::vector<std::pair<std::string, std::string>> FieldSchema(const fs::path& header)
        {
            static const pyre::Regex field("\\{\"([^\"]+)\".*?([A-Z_0-9]+)\\s*\\}");
            std::vector<std::pair<std::string, std::string>> result;
            const auto text = encoding::UniversalNewlines(encoding::Utf8Replace(mod::Bytes(header))); // SplitLines views into it
            for (const auto& line : py::SplitLines(text))
                if (const auto m = field.Search(line))
                {
                    const auto key = *m->Group(1);
                    auto it = std::find_if(result.begin(), result.end(), [&](const auto& p) { return p.first == key; });
                    if (it == result.end())
                        result.emplace_back(key, *m->Group(2));
                    else
                        it->second = *m->Group(2);
                }
            if (result.size() < 100)
                throw weapons::WeaponError("weapon field schema incomplete: " + mod::Str(header));
            return result;
        }

        std::vector<std::string> GltfMaterials(const fs::path& path)
        {
            const auto data = mod::Bytes(path);
            Json document;
            if (data.size() >= 20 && data.compare(0, 4, "glTF") == 0)
            {
                uint32_t length;
                std::memcpy(&length, data.data() + 12, 4);
                document = Json::parse(data.substr(20, length));
            }
            else
                document = Json::parse(data);
            std::vector<std::string> out;
            for (const auto& m : document.value("materials", Json::array()))
                out.push_back(m.at("name").get<std::string>());
            return out;
        }

        struct Equivalent
        {
            std::string name, rule;
            std::vector<std::string> ddsImages;

            std::string Describe(const std::string& kind, const std::string& waw, const std::string& why) const
            {
                const std::string how = rule == "same_name" ? "same name in BO2" : "compat/bo2_equivalents.json";
                return "BO2_FALLBACK " + kind + " " + waw + " -> " + name + ": " + why + "; " + how;
            }
        };

        // bo2equiv.Bo2Equivalents
        class Bo2Equivalents
        {
        public:
            Bo2Equivalents(const std::optional<fs::path>& bo2, const std::map<std::string, std::set<std::string>>& loaded)
            {
                if (bo2)
                    raw_ = *bo2 / "raw";
                for (const auto& [kind, names] : loaded)
                    for (const auto& n : names)
                        loaded_[kind].insert(Lower(n));
                const auto table = extract::ResourceRoot() / "src/waw2bo2/compat/bo2_equivalents.json";
                const auto data = IsFile(table) ? mod::Json(table) : Json::object(); // items() views into it
                for (const auto& [kind, entries] : data.items())
                        if (!StartsWith(kind, "_"))
                            for (const auto& [waw, bo2Name] : entries.items())
                                table_[kind][Lower(waw)] = bo2Name.get<std::string>();
            }

            std::optional<Equivalent> Find(const std::string& kind, const std::string& waw)
            {
                if (auto images = Available(kind, waw))
                    return Equivalent{waw, "same_name", *images};
                const auto k = table_.find(kind);
                if (k != table_.end())
                    if (const auto m = k->second.find(Lower(waw)); m != k->second.end() && !m->second.empty())
                        if (auto images = Available(kind, m->second))
                            return Equivalent{m->second, "table", *images};
                return std::nullopt;
            }

            void Stage(const Equivalent& found, const fs::path& project)
            {
                for (const auto& image : found.ddsImages)
                {
                    auto file = image;
                    std::replace(file.begin(), file.end(), '*', '_');
                    const auto dst = project / "images" / files::FromUtf8(file + ".iwi");
                    if (!IsFile(dst))
                        files::WriteBytes(dst, textures::DdsToIwi(mod::Bytes(*raw_ / "images" / files::FromUtf8(image + ".dds"))).bytes);
                }
            }

        private:
            bool IsLoaded(const std::string& kind, const std::string& name) const
            {
                const auto it = loaded_.find(kind);
                return it != loaded_.end() && it->second.count(Lower(name));
            }

            std::optional<std::vector<std::string>> Available(const std::string& kind, const std::string& name)
            {
                if (!raw_ || kind != "xmodel" || IsLoaded(kind, name))
                    return std::nullopt;
                const auto path = *raw_ / "xmodel" / files::FromUtf8(name + ".json");
                if (!IsFile(path))
                    return std::nullopt;
                return ModelImages(path);
            }

            std::optional<std::vector<std::string>> ModelImages(const fs::path& path)
            {
                std::set<std::string> dds;
                try
                {
                    const auto model = mod::Json(path);
                    for (const auto& lod : model.value("lods", Json::array()))
                        for (auto material : GltfMaterials(*raw_ / files::FromUtf8(lod.at("file").get<std::string>())))
                        {
                            std::replace(material.begin(), material.end(), '*', '_');
                            const auto mat = *raw_ / "materials" / files::FromUtf8(material + ".json");
                            if (!IsFile(mat))
                                return std::nullopt;
                            for (const auto& texture : mod::Json(mat).value("textures", Json::array()))
                            {
                                auto image = texture.value("image", std::string());
                                if (StartsWith(image, ","))
                                    image = image.substr(1);
                                if (IsLoaded("image", image))
                                    return std::nullopt; // a second copy in mod.ff would override a loaded asset
                                if (image.empty() || StartsWith(image, "$") || IsFile(*raw_ / "images" / files::FromUtf8(image + ".iwi")))
                                    continue;
                                if (!IsFile(*raw_ / "images" / files::FromUtf8(image + ".dds")))
                                    return std::nullopt;
                                dds.insert(image);
                            }
                        }
                }
                catch (const std::exception&)
                {
                    return std::nullopt;
                }
                return std::vector<std::string>(dds.begin(), dds.end());
            }

            std::optional<fs::path> raw_;
            std::map<std::string, std::set<std::string>> loaded_;
            std::map<std::string, std::map<std::string, std::string>> table_;
        };

        double FloatOr0(const std::string& text)
        {
            double v = 0;
            if (text.empty())
                return 0;
            if (!py::ParseFloat(text, v))
                throw StepError("ValueError", "could not convert string to float: " + py::StringRepr(text));
            return v;
        }

        double FlameStreamRange(const weapons::InfoFields& fields)
        {
            return std::max(FloatOr0(Field(fields, "flameVar_streamFuelLength")), FloatOr0(Field(fields, "flameVar_streamFlameLength")));
        }

        weapons::InfoFields ReadFlameTable(const fs::path& path)
        {
            return weapons::ReadInfo(path, "FLAMETABLEFILE");
        }
    } // namespace

    Json StageWeaponRuntime(const WeaponRuntimeRequest& request)
    {
        const auto& project = request.content;
        const auto& ipak = request.ipak;
        const auto& bo2 = request.bo2Root;
        const auto& loaded = request.loaded;
        const auto& convertedFx = request.convertedFx;
        StageWeaponEvents(project);

        static const pyre::Regex variantModelSlot("^(gunModel|worldModel)([2-9]|1[0-6])$");
        const auto t6Schema = FieldSchema(extract::ResourceRoot() / "vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/Weapon/WeaponFields.h");
        const std::map<std::string, std::string> t6Types(t6Schema.begin(), t6Schema.end());
        const auto typeOf = [&](const std::string& key) { const auto it = t6Types.find(key); return it == t6Types.end() ? std::string() : it->second; };
        const auto stageReport = mod::Json(project / "weapons.stage.json");
        const auto visuals = mod::Json(project / "weapons.visuals.json");
        std::map<std::string, Json> models;
        for (const auto& m : stageReport["models"]["models"])
            models[m["waw"].get<std::string>()] = m;
        std::set<std::string> missingModels, missingAnims, missingGraphs;
        for (const auto& m : stageReport["models"]["missing"])
            missingModels.insert(m.get<std::string>());
        const auto& unsupportedModels = stageReport["models"]["unsupported"];
        const auto& failedMaterials = visuals["failed_materials"];
        for (const auto& a : stageReport["missing_animations"])
            missingAnims.insert(a.get<std::string>());
        for (const auto& g : stageReport["missing_accuracy_graphs"])
            missingGraphs.insert(files::Utf8(files::FromUtf8(g.get<std::string>()).filename()));
        std::set<std::string> reserved;
        for (const auto& n : request.reserved)
            reserved.insert(Lower(n));
        if (const auto weapons = loaded.find("weapon"); weapons != loaded.end())
            for (const auto& n : weapons->second)
                reserved.insert(Lower(n));
        Bo2Equivalents equivalents(bo2, loaded);

        Json excluded = Json::object(), notes = Json::array(), table = Json::object(), replacements = Json::object();
        std::vector<std::pair<std::string, weapons::InfoFields>> fieldsByWeapon;
        const auto fieldsOf = [&](const std::string& name) -> weapons::InfoFields* {
            for (auto& [n, f] : fieldsByWeapon)
                if (n == name)
                    return &f;
            return nullptr;
        };
        for (const auto& entry : stageReport["weapons"])
        {
            const auto name = entry["name"].get<std::string>();
            if (ENGINE_WEAPONS.count(Lower(name)))
            {
                // both engines' null weapon; BO2 always provides its own
                table[name] = name;
                continue;
            }
            table[name] = reserved.count(Lower(name)) ? WAW_WEAPON_PREFIX + name : name;
            if (table[name] != name)
                notes.push_back("WEAPON_RENAMED " + name + " -> " + table[name].get<std::string>() +
                                ": BO2 has a weapon of that name; scripts reach it through level.waw2bo2_weapons");
            auto fields = weapons::ReadInfo(project / "weapons" / files::FromUtf8(name));
            if (BO2_PRIMARY_FRAGS.count(Lower(name)) && Field(fields, "weaponType") == "grenade" && Field(fields, "offhandClass") == "Frag Grenade" &&
                Field(fields, "offhandSlot") == "Lethal grenade")
            {
                table[name] = replacements[name] = "frag_grenade_zm";
                notes.push_back("BO2_PRIMARY_FRAG " + name + " -> frag_grenade_zm: ordinary lethal replacement; special grenades retain their source weapons");
                continue;
            }
            Json reasons = Json::array();
            if (reserved.count(Lower(table[name].get<std::string>())))
                reasons.push_back("BO2 already has weapons named " + name + " and " + table[name].get<std::string>());
            std::vector<std::string> keys;
            for (const auto& [key, _] : fields)
                keys.push_back(key);
            for (const auto& key : keys)
            {
                const auto value = Field(fields, key);
                std::optional<std::string> kind;
                std::string waw = value;
                for (const auto& [k, prefix] : PREFIXES)
                    if (StartsWith(value, prefix))
                    {
                        kind = k;
                        waw = value.substr(prefix.size());
                        break;
                    }
                if (kind == "xmodel")
                {
                    if (models.count(waw))
                    {
                        std::vector<std::string> bad;
                        for (const auto& m : models[waw]["materials"])
                            if (failedMaterials.contains(m.get<std::string>()))
                                bad.push_back(m.get<std::string>());
                        if (!bad.empty())
                        {
                            std::string joined;
                            for (const auto& b : bad)
                                joined += (joined.empty() ? "" : ", ") + b;
                            reasons.push_back(key + " " + waw + ": materials not converted: " + joined);
                        }
                    }
                    else if (unsupportedModels.contains(waw))
                        reasons.push_back(key + " " + waw + ": " + unsupportedModels[waw].get<std::string>());
                    else if (missingModels.count(waw))
                    {
                        auto found = equivalents.Find("xmodel", waw);
                        if (found)
                        {
                            try
                            {
                                equivalents.Stage(*found, project);
                            }
                            catch (const std::exception& exc)
                            {
                                notes.push_back(name + ": BO2 equivalent " + found->name + " for " + waw + " unusable: " + exc.what());
                                found.reset();
                            }
                        }
                        if (found)
                        {
                            fields[key] = found->name;
                            notes.push_back(name + ": " + found->Describe("xmodel", waw, key + " model absent from WaW"));
                        }
                        else if (variantModelSlot.Search(key))
                        {
                            fields[key] = "";
                            notes.push_back("WAW_ASSET_ABSENT " + name + ": " + key + " " + waw +
                                            " cleared (no WaW model, no BO2 equivalent; WaW showed its default model)");
                        }
                        else
                            reasons.push_back(key + " " + waw + ": model absent from WaW and no BO2 equivalent");
                    }
                }
                else if (kind == "material" && failedMaterials.contains(waw))
                {
                    fields[key] = "";
                    notes.push_back("UNSUPPORTED_MATERIAL " + name + ": " + key + " " + waw + " cleared: " + failedMaterials[waw][0].get<std::string>());
                }
                else if (kind == "xanim" && missingAnims.count(waw))
                {
                    fields[key] = "";
                    notes.push_back("WAW_ASSET_ABSENT " + name + ": " + key + " animation " + waw + " cleared");
                }
                else if (kind == "accuracy" && missingGraphs.count(waw))
                {
                    fields[key] = "";
                    notes.push_back("WAW_ASSET_ABSENT " + name + ": " + key + " accuracy graph " + waw + " cleared");
                }
                else if (StartsWith(value, "waw/") && typeOf(key) == "CSPFT_FX" && !convertedFx.count(value.substr(4)))
                {
                    fields[key] = "";
                    notes.push_back("UNSUPPORTED_FX " + name + ": " + key + " " + value.substr(4) + " not converted, cleared");
                }
            }
            if (!reasons.empty())
                excluded[name] = reasons;
            fieldsByWeapon.emplace_back(name, fields);
        }
        // a weapon is unusable without its alternate (and the alternate's own)
        for (bool changed = true; changed;)
        {
            changed = false;
            for (const auto& [name, fields] : fieldsByWeapon)
            {
                const auto alt = Field(fields, "altWeapon");
                const bool carried = !alt.empty() && (ENGINE_WEAPONS.count(Lower(alt)) || replacements.contains(alt) || (fieldsOf(alt) && !excluded.contains(alt)));
                if (!excluded.contains(name) && !alt.empty() && !carried)
                {
                    excluded[name] = Json::array({"alternate weapon " + alt + " is not carried"});
                    changed = true;
                }
            }
        }
        std::vector<std::string> runtime;
        for (const auto& [n, _] : fieldsByWeapon)
            if (!excluded.contains(n))
                runtime.push_back(n);
        std::sort(runtime.begin(), runtime.end());
        // content_source is on mod.ff's asset search path: an uncarried WaW weapon
        // file would still be found when a BO2 weapon of the same name is loaded
        const auto parked = project / "weapons_not_carried";
        for (const auto& entry : stageReport["weapons"])
        {
            const auto name = entry["name"].get<std::string>();
            const auto file = project / "weapons" / files::FromUtf8(name);
            if (std::find(runtime.begin(), runtime.end(), name) == runtime.end() && IsFile(file))
            {
                fs::create_directories(parked);
                fs::rename(file, parked / files::FromUtf8(name));
            }
        }
        std::set<std::string> images, anims, effects;
        const auto& materialImages = visuals["material_images"];
        const auto addImages = [&](const std::string& material)
        {
            if (materialImages.contains(material))
                for (const auto& i : materialImages[material])
                    images.insert(i.get<std::string>());
        };
        for (const auto& name : runtime)
        {
            auto& fields = *fieldsOf(name);
            if (Field(fields, "weaponType") == "gas")
            {
                double reach = 0;
                for (const auto key : weapons::FlameTableKeys)
                    if (!Field(fields, std::string(key)).empty())
                        reach = std::max(reach, FlameStreamRange(ReadFlameTable(project / "weapons" / files::FromUtf8(Field(fields, std::string(key))))));
                if (reach > 0)
                    for (const char* key : {"maxRange", "maxDamageRange", "minDamageRange"})
                    {
                        const auto previous = FloatOr0(Field(fields, key).empty() ? "0" : Field(fields, key));
                        if (previous <= 0 || previous > reach)
                        {
                            char text[64];
                            std::snprintf(text, sizeof text, "%.9g", reach);
                            fields[key] = text;
                        }
                    }
            }
            // An absent string or anim-name field stays a NULL pointer, which T6
            // dereferences: the linker turns "" into a valid empty string.
            for (const auto& [key, kind] : t6Schema)
                if (POINTER_STRING_FIELDS.count(kind) && !fields.Has(key))
                    fields[key] = "";
            // weapons.separate_alt_ammo: an alternate keeping its own ammo
            if (Field(fields, "inventoryType") == "altmode")
                if (const auto* primary = fieldsOf(Field(fields, "altWeapon")); primary && Field(*primary, "ammoName") != Field(fields, "ammoName"))
                    fields["ignoreAttachments"] = "1";
            if (fields.Has("altWeapon") && table.contains(Field(fields, "altWeapon")))
                fields["altWeapon"] = table[Field(fields, "altWeapon")].get<std::string>();
            const auto target = table[name].get<std::string>();
            if (target != name)
                fs::remove(project / "weapons" / files::FromUtf8(name));
            files::WriteText(project / "weapons" / files::FromUtf8(target), weapons::WriteInfo(fields));
            for (const auto& [key, value] : fields)
            {
                if (StartsWith(value, "waw_xanim/"))
                    anims.insert(value);
                else if (StartsWith(value, "waw/") && typeOf(key) == "CSPFT_FX")
                    effects.insert(value);
                else if (StartsWith(value, "waw_material/"))
                    addImages(value);
                else if (StartsWith(value, "waw_xmodel/"))
                    for (const auto& material : models.at(value.substr(std::string("waw_xmodel/").size()))["materials"])
                        addImages(weapons::OutputName("material", material.get<std::string>()));
            }
        }
        std::set<std::string> flameMaterials, usedTables;
        for (const auto& n : runtime)
            for (const auto key : weapons::FlameTableKeys)
                usedTables.insert(Field(*fieldsOf(n), std::string(key)));
        for (const auto& entry : stageReport.value("flame_tables", Json::array()))
        {
            if (!usedTables.count(entry["name"].get<std::string>()))
                continue;
            const auto fields = ReadFlameTable(project / files::FromUtf8(entry["file"].get<std::string>()));
            for (const auto key : weapons::FlameMaterialKeys)
                if (!Field(fields, std::string(key)).empty())
                {
                    flameMaterials.insert(Field(fields, std::string(key)));
                    addImages(Field(fields, std::string(key)));
                }
        }
        Json lines = Json::array();
        if (!images.empty())
        {
            lines.push_back(">level.ipak_read," + ipak);
            lines.push_back(">ipak," + ipak);
        }
        for (const auto& n : images)
            lines.push_back("image," + n);
        // The baseline linker cannot follow nested tables. These assets must be
        // present before the native relink fills the weapon's table pointers.
        for (const auto& n : flameMaterials)
            lines.push_back("material," + n);
        for (const auto& n : anims)
            lines.push_back("xanim," + n);
        // converted effects live in the map zone; mod.ff only references them
        for (const auto& n : effects)
            lines.push_back("fx,," + n);
        for (const auto& n : runtime)
            lines.push_back("weapon," + table[n].get<std::string>());
        std::set<std::string> replaced;
        for (const auto& [_, v] : replacements.items())
            replaced.insert(v.get<std::string>());
        for (const auto& n : replaced)
            lines.push_back("weapon," + n);
        // every WaW weapon: its BO2 name, or "" when mod.ff does not carry it
        Json finalTable = Json::object();
        for (const auto& [w, b] : table.items())
            finalTable[w] = std::find(runtime.begin(), runtime.end(), w) != runtime.end() || replacements.contains(w) || ENGINE_WEAPONS.count(Lower(w)) ? b : Json("");
        Json result{{"status", "runtime_zone_lines_not_native_validated"},
                    {"ipak", images.empty() ? Json() : Json(ipak)},
                    {"weapons", runtime},
                    {"table", finalTable},
                    {"replacements", replacements},
                    {"excluded", excluded},
                    {"notes", notes},
                    {"zone_lines", lines},
                    {"images", images.size()},
                    {"xanims", anims.size()},
                    {"fx_references", std::vector<std::string>(effects.begin(), effects.end())}};
        files::WriteText(project / "weapons.runtime.json", py::JsonDumps(result, 2) + "\n");
        return result;
    }

    WeaponRuntimeFinish FinishWeaponRuntime(const WeaponFinishRequest& request)
    {
        const auto& projectRoot = request.projectRoot;
        const auto& fxTable = request.fxTable;
        const auto& weaponTable = request.weapons;
        const auto& wawMap = request.wawMap;
        // stage_map_tables: the map's own string tables as waw/<path> (the first root holding a path wins)
        std::map<std::string, std::string> tables;
        for (const auto& root : request.mapTableRoots)
        {
            if (!mod::IsDir(root))
                continue;
            for (const auto& path : files::GlobSuffix(root, ".csv"))
            {
                const auto rel = mod::Generic(path.lexically_relative(root));
                if (StartsWith(rel, "zone_source/") || StartsWith(rel, "waw/") || tables.count(Lower(rel)))
                    continue;
                const auto staged = "waw/" + rel;
                const auto target = projectRoot / files::FromUtf8(staged);
                fs::create_directories(target.parent_path());
                fs::copy_file(path, target, fs::copy_options::overwrite_existing);
                fs::last_write_time(target, fs::last_write_time(path));
                tables[Lower(rel)] = staged;
            }
        }
        if (!tables.empty())
        {
            std::set<std::string> staged;
            for (const auto& [_, v] : tables)
                staged.insert(v);
            std::string lines;
            for (const auto& v : staged)
                lines += "stringtable," + v + "\n";
            std::string existing;
            files::ReadBytes(projectRoot / "mod_extra.zone", existing);
            files::WriteBytes(projectRoot / "mod_extra.zone", existing + mod::TextBytes(lines));
        }
        // bolted_loop_effects: converted effects that loop forever
        std::set<std::string> bolted;
        for (const auto& [_, name] : fxTable)
        {
            const auto path = projectRoot / "fx" / files::FromUtf8(name + ".w2bfx.json");
            if (!IsFile(path))
                continue;
            const auto definition = mod::Json(path);
            const auto looping = definition.value("elemDefCountLooping", Json(0));
            const auto life = definition.value("msecLoopingLife", Json(0));
            if (looping.is_number() && looping.get<double>() != 0 && life.is_number() && life.get<double>() >= 2147483647)
                bolted.insert(name);
        }
        // source_zombie_var_overrides: WaW table column one, source roots in order
        std::map<std::string, std::string> overrides;
        for (const auto& root : request.tableRoots)
        {
            const auto source = root / "mp/zombiemode.csv";
            if (!IsFile(source))
                continue;
            auto text = mod::Bytes(source);
            if (StartsWith(text, "\xEF\xBB\xBF"))
                text = text.substr(3);
            for (const auto& row : ReadCsv(text))
                if (row.size() > 1 && !row[0].empty() && !row[1].empty())
                    overrides[row[0]] = row[1];
            break;
        }
        gsc::port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
        files::WriteText(projectRoot / "maps/mp/waw/_waw2bo2_assets.gsc", gsc::port::AssetsSource(fxTable, wawMap, weaponTable, bolted, overrides, tables));
        // entities.map_wall_buys
        std::vector<std::string> notes;
        const auto entitiesJson = projectRoot / "BSP" / "entities.json";
        if (fs::exists(entitiesJson))
        {
            mapents::MapEntities kept;
            for (auto& ent : mapents::ReadMapEntities(entitiesJson))
            {
                if (ent.Get("targetname") == "weapon_upgrade")
                {
                    const auto* weapon = ent.Find("zombie_weapon_upgrade");
                    if (weapon && weaponTable.count(*weapon) && weaponTable.at(*weapon).empty())
                    {
                        notes.push_back("UNSUPPORTED_WALLBUY " + *weapon + " at " + std::string(ent.Get("origin", "None")) + ": weapon not carried in mod.ff; trigger removed");
                        continue;
                    }
                    ent.Set("targetname", "waw_weapon_upgrade");
                }
                kept.push_back(std::move(ent));
            }
            mapents::WriteMapEntities(entitiesJson, kept);
        }
        std::vector<std::string> tableKeys;
        for (const auto& [k, _] : tables)
            tableKeys.push_back(k);
        return {std::move(tableKeys), std::move(notes)};
    }
} // namespace w2b::pipeline
