// WaW's compiled menus as objects (see waw_menus.h).
#include "oat/waw_menus.h"

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

        Json Floats(const float* values, const int count)
        {
            Json out = Json::array();
            for (int i = 0; i < count; i++)
                out.push_back(values[i]);
            return out;
        }

        Json Rect(const T4::rectDef_s& rect)
        {
            return {{"x", rect.x}, {"y", rect.y}, {"w", rect.w}, {"h", rect.h}, {"horzAlign", rect.horzAlign}, {"vertAlign", rect.vertAlign}};
        }

        Json Window(const T4::windowDef_t& window)
        {
            return {{"name", Text(window.name)},
                    {"rect", Rect(window.rect)},
                    {"group", Text(window.group)},
                    {"style", window.style},
                    {"border", window.border},
                    {"ownerDraw", window.ownerDraw},
                    {"ownerDrawFlags", window.ownerDrawFlags},
                    {"borderSize", window.borderSize},
                    {"staticFlags", static_cast<unsigned>(window.staticFlags)},
                    {"dynamicFlags", static_cast<unsigned>(window.dynamicFlags[0])},
                    {"foreColor", Floats(window.foreColor, 4)},
                    {"backColor", Floats(window.backColor, 4)},
                    {"borderColor", Floats(window.borderColor, 4)},
                    {"outlineColor", Floats(window.outlineColor, 4)},
                    {"background", window.background ? Text(window.background->info.name) : Json()}};
        }

        // the expression as its compiled token list: operators as numbers, operands as values
        Json Statement(const T4::statement_s& statement)
        {
            if (statement.numEntries <= 0 || !statement.entries)
                return nullptr;
            Json out = Json::array();
            for (int i = 0; i < statement.numEntries; i++)
            {
                const auto* entry = statement.entries[i];
                if (!entry)
                    continue;
                if (entry->type == T4::EET_OPERATOR)
                {
                    out.push_back({{"op", static_cast<int>(entry->data.op)}});
                    continue;
                }
                const auto& operand = entry->data.operand;
                if (operand.dataType == T4::VAL_INT)
                    out.push_back({{"int", operand.internals.intVal}});
                else if (operand.dataType == T4::VAL_FLOAT)
                    out.push_back({{"float", operand.internals.floatVal}});
                else
                    out.push_back({{"string", Text(operand.internals.string)}});
            }
            return out;
        }

        // execKey handlers: the key's code and its script
        Json Keys(const T4::ItemKeyHandler* handler)
        {
            Json out = Json::array();
            for (; handler; handler = handler->next)
                out.push_back({{"key", handler->key}, {"action", Text(handler->action)}});
            return out;
        }

        Json Item(const T4::itemDef_s& item)
        {
            return {{"window", Window(item.window)},
                    {"itemFlags", item.itemFlags},
                    {"dataType", item.dataType},
                    {"mouseEnterText", Text(item.mouseEnterText)},
                    {"mouseExitText", Text(item.mouseExitText)},
                    {"mouseEnter", Text(item.mouseEnter)},
                    {"mouseExit", Text(item.mouseExit)},
                    {"action", Text(item.action)},
                    {"onAccept", Text(item.onAccept)},
                    {"onFocus", Text(item.onFocus)},
                    {"leaveFocus", Text(item.leaveFocus)},
                    {"onKey", Keys(item.onKey)},
                    {"focusSound", item.focusSound && item.focusSound->aliasName ? Text(item.focusSound->aliasName) : Json()},
                    {"type", item.type},
                    {"alignment", item.alignment},
                    {"font", item.fontEnum},
                    {"textAlignMode", item.textAlignMode},
                    {"textAlignX", item.textalignx},
                    {"textAlignY", item.textaligny},
                    {"textScale", item.textscale},
                    {"textStyle", item.textStyle},
                    {"text", Text(item.text)},
                    {"dvar", Text(item.dvar)},
                    {"dvarTest", Text(item.dvarTest)},
                    {"enableDvar", Text(item.enableDvar)},
                    {"dvarFlags", item.dvarFlags},
                    {"special", item.special},
                    {"visibleExp", Statement(item.visibleExp)},
                    {"textExp", Statement(item.textExp)},
                    {"materialExp", Statement(item.materialExp)},
                    {"rectXExp", Statement(item.rectXExp)},
                    {"rectYExp", Statement(item.rectYExp)},
                    {"rectWExp", Statement(item.rectWExp)},
                    {"rectHExp", Statement(item.rectHExp)},
                    {"forecolorAExp", Statement(item.forecolorAExp)}};
        }
    } // namespace

    std::string ReadMenus(const WawZone& zone)
    {
        const std::lock_guard guard(StateLock());
        const CapturedOutput quiet;
        auto out = nlohmann::ordered_json::object();
        for (const auto* asset : Loaded(zone.fastfile, zone.searchPaths).Get().m_pools.PoolAssets<T4::AssetMenu>())
        {
            if (asset->IsReference())
                continue;
            const auto& menu = *asset->Asset();
            auto items = nlohmann::ordered_json::array();
            for (int i = 0; i < menu.itemCount && menu.items; i++)
                if (menu.items[i])
                    items.push_back(Item(*menu.items[i]));
            out[asset->m_name] = {{"window", Window(menu.window)},
                                  {"fullScreen", menu.fullScreen},
                                  {"visibleExp", Statement(menu.visibleExp)},
                                  {"rectXExp", Statement(menu.rectXExp)},
                                  {"rectYExp", Statement(menu.rectYExp)},
                                  {"onOpen", Text(menu.onOpen)},
                                  {"onClose", Text(menu.onClose)},
                                  {"onESC", Text(menu.onESC)},
                                  {"onKey", Keys(menu.onKey)},
                                  {"blurRadius", menu.blurRadius},
                                  {"focusColor", Floats(menu.focusColor, 4)},
                                  {"soundName", Text(menu.soundName)},
                                  {"items", std::move(items)}};
        }
        return out.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
} // namespace w2b::oat
