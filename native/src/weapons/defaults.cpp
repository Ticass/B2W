#include "weapons/weapon.h"

#include <algorithm>
#include <array>

namespace w2b::weapons
{
    namespace
    {
        void SetField(Weapon& weapon, std::string_view key, const Json& source, std::string_view target,
                      std::string_view reason)
        {
            weapon.fields[key] = target;
            weapon.translations[key] = {{"source", source}, {"target", target}, {"reason", reason}};
        }

        void SetOverlayAndKick(Weapon& weapon, const Json& source, const Schema& t6)
        {
            if (!Field(weapon.fields, "adsOverlayShader").empty() && !source.contains("adsOverlayAlphaScale") &&
                t6.contains("adsOverlayAlphaScale"))
                SetField(weapon, "adsOverlayAlphaScale", nullptr, "1",
                         "T6-only overlay alpha scale; WaW draws the scope overlay opaque");
            constexpr std::array<std::string_view, 8> kick{
                "adsViewKickPitchMin", "adsViewKickPitchMax", "adsViewKickYawMin", "adsViewKickYawMax",
                "hipViewKickPitchMin", "hipViewKickPitchMax", "hipViewKickYawMin", "hipViewKickYawMax"};
            if (std::ranges::any_of(kick, [&](auto key) { return NumericField(source, key) != 0; }))
                for (const std::string_view key : {"adsViewKickCenterDuckedScale", "adsViewKickCenterProneScale"})
                    if (!source.contains(key) && t6.contains(key))
                        SetField(weapon, key, nullptr, "1",
                                 "T6-only stance scale on view-kick recentering; WaW recentres unscaled");
        }

        void SetOffhand(Weapon& weapon, const Json& source)
        {
            if (source.contains("offhandSlot"))
                return;
            const auto& offhand = Field(source, "offhandClass");
            std::string_view slot;
            if (offhand == "Frag Grenade")
                slot = "Lethal grenade";
            if (offhand == "Smoke Grenade" || offhand == "Flash Grenade")
                slot = "Tactical grenade";
            if (!slot.empty())
                SetField(weapon, "offhandSlot", offhand, slot, "T4 offhand class -> T6 required inventory slot");
        }

        void SetProjectileDamage(Weapon& weapon, const Json& source, const Schema& t6)
        {
            const auto& type = Field(source, "weaponType");
            if (type != "projectile" && type != "grenade")
                return;
            std::vector<std::string_view> locations;
            for (const auto& [key, value] : source.items())
                if (key.starts_with("loc") && Field(t6, key) == "CSPFT_FLOAT")
                    locations.push_back(key);
            if (locations.empty() || !std::ranges::all_of(locations, [&](auto key)
                { return NumericField(source, key) == 0; }))
                return;
            for (const auto key : locations)
                weapon.fields[key] = "1";
            weapon.translations["loc*"] = {{"source", "0"}, {"target", "1"},
                {"reason", "WaW ignores hit-location multipliers on projectiles/grenades; T6 applies them"}};
        }

        void SetHeldMelee(Weapon& weapon, const Json& source)
        {
            const auto& animation = Field(source, "meleeAnim");
            if (Field(source, "inventoryType") == "item" && !animation.empty() &&
                animation != Field(source, "idleAnim") && animation != Field(source, "emptyIdleAnim") &&
                NumericField(source, "meleeDamage") > 0 && !source.contains("useAsMelee"))
                SetField(weapon, "useAsMelee", "inventoryType item + meleeAnim", "1",
                         "T4 held item melee swings itself; T6 needs useAsMelee");
        }

        void SetProjectileImpact(Weapon& weapon, const Json& source)
        {
            if (Field(source, "weaponType") != "projectile")
                return;
            const auto& impact = Field(source, "impactType");
            const bool explosion = !Field(source, "projExplosionEffect").empty();
            if (impact == "tank_shell" && !explosion)
                SetField(weapon, "impactType", impact, "none",
                         "T4 projectile without explosion FX; T6 shell impact table adds one");
            if (impact.starts_with("bullet_") && explosion)
                SetField(weapon, "impactType", impact, "none",
                         "projectile carries its authored explosion; suppress additional T6 bullet impact table");
        }
    }

    void ApplyWeaponDefaults(Weapon& weapon, const Json& source, const Schema& t6)
    {
        SetOverlayAndKick(weapon, source, t6);
        SetOffhand(weapon, source);
        SetProjectileDamage(weapon, source, t6);
        SetHeldMelee(weapon, source);
        SetProjectileImpact(weapon, source);
    }
}
