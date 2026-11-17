#include "weapons/weapon.h"

#include "core/unicode.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace w2b::weapons
{
    namespace
    {
        using Lookup = std::map<std::string_view, std::string_view, std::less<>>;
        const Lookup dependencyTypes{{"CSPFT_XMODEL", "xmodel"}, {"CSPFT_MATERIAL", "material"}, {"CSPFT_FX", "fx"},
            {"CSPFT_SOUND", "sound"}, {"WFT_ANIM_NAME", "xanim"}, {"CSPFT_TRACER", "tracer"},
            {"CSPFT_PHYS_PRESET", "physpreset"}};
        const Lookup classAnimationTypes{
            {"autorifle", "BO2 rifles use default (M14/M16, 24 rifle weapons)"},
            {"pistol", "BO2 pistols use default (22 of 25 pistol weapons; rest dualwield/ballistic)"},
            {"smg", "BO2 SMGs use default (16 of 25 smg weapons; rest rearclip/g11)"},
            {"mg", "BO2 box-fed MGs use default (hamr, rpd); beltfed only for belt feeds"},
            {"rocketlauncher", "BO2 rocket launchers use default (usrpg)"},
            {"grenade", "BO2 grenades use default (13 grenade weapons incl. frag_grenade_zm)"}};
        const std::set<std::string_view, std::less<>> enumTypes{
            "WFT_WEAPONTYPE", "WFT_WEAPONCLASS", "WFT_OVERLAYRETICLE", "WFT_PENETRATE_TYPE", "WFT_IMPACT_TYPE",
            "WFT_STANCE", "WFT_PROJ_EXPLOSION", "WFT_OFFHAND_CLASS", "WFT_ANIMTYPE", "WFT_ACTIVE_RETICLE_TYPE",
            "WFT_GUIDED_MISSILE_TYPE", "WFT_STICKINESS", "WFT_OVERLAYINTERFACE", "WFT_INVENTORYTYPE",
            "WFT_FIRETYPE", "WFT_AMMOCOUNTER_CLIPTYPE", "WFT_ICONRATIO_HUD", "WFT_ICONRATIO_PICKUP",
            "WFT_ICONRATIO_AMMOCOUNTER", "WFT_ICONRATIO_KILL", "WFT_ICONRATIO_DPAD"};
        const std::set<std::string_view, std::less<>> playerAnimationTypes{
            "none", "default", "other", "sniper", "m203", "hold", "briefcase", "reviver", "radio", "dualwield",
            "remotecontrol", "crossbow", "minigun", "beltfed", "g11", "rearclip", "handleclip", "rearclipsniper",
            "ballisticknife", "singleknife", "nopump", "hatchet", "grimreaper", "zipline", "riotshield", "tablet",
            "turned", "screecher", "staff"};

        std::string Reference(Weapon& weapon, std::string_view kind, const std::string& name)
        {
            auto safe = OutputName(kind, name);
            weapon.AddDependency(kind, name);
            return kind == "fx" || kind == "sound" ? "waw/" + name : safe;
        }

        std::vector<std::string> Words(std::string_view text)
        {
            const auto chars = unicode::Decode(text);
            std::vector<std::string> words;
            for (std::size_t i = 0; i < chars.size();)
            {
                if (unicode::IsSpace(chars[i]))
                {
                    ++i;
                    continue;
                }
                const auto start = i++;
                while (i < chars.size() && !unicode::IsSpace(chars[i]))
                    ++i;
                words.push_back(unicode::Encode(std::u32string_view(chars).substr(start, i - start)));
            }
            return words;
        }

        std::string NotetrackSounds(Weapon& weapon, std::string_view name, std::string_view value)
        {
            std::string rewritten;
            bool first = true;
            for (const auto line : py::SplitLines(value))
            {
                const auto words = Words(line);
                if (words.size() != 2)
                    throw WeaponError(std::string(name) + ": malformed notetrackSoundMap " + py::StringRepr(line));
                if (!first)
                    rewritten += '\n';
                first = false;
                rewritten += words[0] + " " + Reference(weapon, "sound", words[1]);
            }
            return rewritten;
        }

        std::string AmmoCapacity(const Json& source)
        {
            const auto& value = Field(source, "sharedAmmoCap");
            if (value.empty())
                return "0";
            long long number = 0;
            bool overflow = false;
            if (!py::ParseInt(value, number, overflow))
                throw WeaponError("invalid literal for int() with base 10: " + py::StringRepr(value));
            if (!overflow)
                return std::to_string(number);
            // The capacity is only part of the group name. Preserve the full
            // integer even for an authored value outside int64_t.
            const auto stripped = py::Strip(value);
            std::string digits;
            std::ranges::copy_if(stripped, std::back_inserter(digits), py::IsDigit);
            digits.erase(0, digits.find_first_not_of('0'));
            return stripped.starts_with('-') ? "-" + digits : digits;
        }

        void RewriteReference(Weapon& weapon, std::string_view name, std::string_view key, std::string_view type,
                              std::string& value, const Json& source)
        {
            if (value.empty())
                return;
            const auto asset = dependencyTypes.find(type);
            if (type == "WFT_BOUNCE_SOUND")
            {
                OutputName("sound", value);
                weapon.AddDependency("sound", value + "_default");
                weapon.translations[key] = {{"source", value}, {"target", "waw/" + value},
                    {"reason", "T4 surface bounce aliases -> namespaced T6 sound prefix"}};
                value = "waw/" + value;
            }
            if (key == "flameTableFirstPerson" || key == "flameTableThirdPerson")
                value = Reference(weapon, "flametable", value);
            else if (asset != dependencyTypes.end())
                value = Reference(weapon, asset->second, value);
            if (type == "WFT_NOTETRACKSOUNDMAP")
                value = NotetrackSounds(weapon, name, value);
            if (!value.empty() && key == "altWeapon")
                value = Reference(weapon, "weapon", value);
            if (!value.empty() && (key == "aiVsAiAccuracyGraph" || key == "aiVsPlayerAccuracyGraph"))
            {
                weapon.AddDependency(key == "aiVsAiAccuracyGraph" ? "accuracy_aivsai" : "accuracy_aivsplayer", value);
                value = OutputName("accuracy", value);
            }
            if (!value.empty() && (key == "ammoName" || key == "clipName"))
                value = "waw_" + value;
            if (!value.empty() && key == "sharedAmmoCapName")
            {
                auto target = "waw_" + value + "_cap_" + AmmoCapacity(source);
                weapon.translations[key] = {{"source", value}, {"target", target},
                    {"reason", "isolate WaW shared ammo groups by authored capacity from T6 groups"}};
                value = std::move(target);
            }
        }

        Json Unsupported(const std::string& src, const std::string& dst,
                         const std::string& value)
        {
            return {{"value", value}, {"source_type", src.empty() ? Json{} : Json(src)},
                    {"target_type", dst.empty() ? Json{} : Json(dst)}};
        }
    }

    Weapon Convert(std::string_view name, const Json& source, const Schema& t4, const Schema& t6)
    {
        Weapon weapon;
        weapon.name = OutputName("weapon", std::string(name));
        for (const auto& [key, field] : source.items())
        {
            auto value = field.get<std::string>();
            const auto& src = Field(t4, key);
            const bool zoom = key == "adsZoomFov" && src == "CSPFT_FLOAT";
            const std::string_view target = zoom ? std::string_view("adsZoomFov1") : std::string_view(key);
            const auto& dst = Field(t6, target);
            const bool soundName = src == "CSPFT_SOUND" && dst == "CSPFT_STRING";
            const bool streamedMaterial = src == "CSPFT_MATERIAL" && dst == "CSPFT_MATERIAL_STREAM";
            if (dst.empty() || (src != dst && !soundName && !streamedMaterial))
            {
                if (!value.empty() && value != "0" && value != "0.0")
                    weapon.unsupported[key] = Unsupported(src, dst, value);
                continue;
            }
            const auto profile = classAnimationTypes.find(value);
            if (key == "playerAnimType" && profile != classAnimationTypes.end())
            {
                weapon.translations[key] = {{"source", value}, {"target", "default"}, {"reason", profile->second}};
                value = "default";
            }
            else if (value.empty() && enumTypes.contains(dst))
            {
                weapon.translations[key] = {{"source", value}, {"target", nullptr}, {"reason", "unset; T6 default"}};
                continue;
            }
            else if (key == "playerAnimType" && !playerAnimationTypes.contains(value))
            {
                auto omitted = Unsupported(src, dst, value);
                omitted["reason"] = "no measured T6 playerAnimType";
                weapon.unsupported[key] = std::move(omitted);
                continue;
            }
            RewriteReference(weapon, name, key, src, value, source);
            weapon.fields[target] = value;
            if (zoom)
            {
                weapon.fields["adsZoomFov2"] = value;
                weapon.fields["adsZoomFov3"] = value;
                weapon.translations[key] = {{"source", value}, {"target", "adsZoomFov1/2/3"},
                    {"reason", "T6 per-zoom-level ADS FOV; WaW has one"}};
            }
        }
        ApplyWeaponDefaults(weapon, source, t6);
        return weapon;
    }
}
