// WaW's impact table and destructibles as JSON (see waw_tables.h).
#include "oat/waw_tables.h"

#include "oat/loaded_zones.h"
#include "oat/oat_state.h"

#include "Game/T4/T4.h"

#include <nlohmann/json.hpp>

namespace w2b::oat
{
    namespace
    {
        using Json = nlohmann::ordered_json; // this library's own flavour (JSON_DIAGNOSTICS)

        Json Text(const char* text)
        {
            return text ? Json(std::string(text)) : Json();
        }

        // "," marks an asset of another zone
        std::string Name(const char* name)
        {
            return !name ? std::string() : name[0] == ',' ? std::string(name + 1) : std::string(name);
        }

        template<typename T> Json Asset(const T* asset)
        {
            return asset ? Json(Name(asset->name)) : Json();
        }

        Json Sound(const T4::SndAliasCustom& sound)
        {
            return sound.name && sound.name->soundName ? Text(sound.name->soundName) : Json();
        }

        Json Dump(const T4::DestructibleDef& def, const Zone& zone)
        {
            auto pieces = Json::array();
            for (int i = 0; i < def.numPieces && def.pieces; i++)
            {
                const auto& piece = def.pieces[i];
                auto models = Json::array();
                for (const auto& model : piece.dmodels)
                    models.push_back({{"model", Asset(model.model)},
                                      {"breakHealth", model.breakHealth},
                                      {"flags", model.flags},
                                      {"breakEffect", Asset(model.breakEffect)},
                                      {"breakSound", Sound(model.breakSound)},
                                      {"breakNotify", Text(model.breakNotify)},
                                      {"loopSound", Text(model.loopSound)},
                                      {"spawnModel", Json::array({Asset(model.spawnModel[0]), Asset(model.spawnModel[1]), Asset(model.spawnModel[2])})}});
                pieces.push_back({{"models", std::move(models)},
                                  {"attachBone", Text(piece.attachBone)},
                                  {"parentPiece", piece.parentPiece},
                                  {"parentDamagePercent", piece.parentDamagePercent},
                                  {"bulletDamageScale", piece.bulletDamageScale},
                                  {"explosiveDamageScale", piece.explosiveDamageScale},
                                  {"entityDamageTransfer", piece.entityDamageTransfer},
                                  {"physConstraints", Asset(piece.physConstraints)},
                                  {"health", piece.health},
                                  {"damageSound", Sound(piece.damageSound)},
                                  {"burnEffect", Asset(piece.burnEffect)},
                                  {"burnSound", Sound(piece.burnSound)},
                                  {"enableLabel", Text(zone.m_script_strings.CValue(piece.enableLabel))}});
            }
            return {{"baseModel", Asset(def.baseModel)}, {"clientOnly", def.clientOnly}, {"pieces", std::move(pieces)}};
        }
    } // namespace

    std::string ReadImpactTable(const WawZone& zone)
    {
        const std::lock_guard guard(StateLock());
        const CapturedOutput quiet;
        Json out;
        for (const auto* asset : Loaded(zone.fastfile, zone.searchPaths).Get().m_pools.PoolAssets<T4::AssetImpactFx>())
        {
            const auto* table = asset->Asset();
            if (asset->IsReference() || !table->table || (table->name && *table->name)) // the game looks the one named "" up
                continue;
            out = Json::array();
            for (int type = 0; type < 16; type++)
            {
                auto row = Json::array();
                for (const auto* effect : table->table[type].nonflesh)
                    row.push_back(effect ? Name(effect->name) : std::string());
                for (const auto* effect : table->table[type].flesh)
                    row.push_back(effect ? Name(effect->name) : std::string());
                out.push_back(std::move(row));
            }
        }
        return out.dump();
    }

    std::string AssetNames(const WawZone& zone)
    {
        const std::lock_guard guard(StateLock());
        const CapturedOutput quiet;
        auto out = Json::object();
        const auto& loaded = Loaded(zone.fastfile, zone.searchPaths).Get();
        for (const auto& asset : loaded.m_pools)
            if (!asset->IsReference())
                if (const auto type = IGame::GetGameById(loaded.m_game_id)->GetAssetTypeName(asset->m_type))
                    out[*type].push_back(asset->m_name);
        return out.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    std::string ReadDestructibles(const WawZone& zone)
    {
        const std::lock_guard guard(StateLock());
        const CapturedOutput quiet;
        auto out = Json::object();
        const auto& loaded = Loaded(zone.fastfile, zone.searchPaths).Get();
        for (const auto* asset : loaded.m_pools.PoolAssets<T4::AssetDestructibleDef>())
            if (!asset->IsReference())
                out[asset->m_name] = Dump(*asset->Asset(), loaded);
        return out.dump(1, ' ', false, Json::error_handler_t::replace);
    }
} // namespace w2b::oat
