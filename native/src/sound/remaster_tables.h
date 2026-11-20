#pragma once
// The audioremaster tables the sound mix measures from the stock BO2 and WaW
// banks and the bank writer applies (sound/bank.cpp): the stock mix of each
// role and volume class, the WaW bus -> BO2 group table, the stock WaW
// quantiles the classes are transferred from, and the role lists.

#include "core/text.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace w2b::sound
{
    // alias columns -> values, in the stock column order
    using AliasMix = std::vector<std::pair<std::string, std::string>>;

    struct StockEmitter // stock_emitters: BO2's 3D ambient emitter placement
    {
        AliasMix columns;
        long long rows = 0;
    };

    struct RoleProfile // the most common stock mix of a role (music, ambience, announcer, stinger:<name>)
    {
        AliasMix mix;
        std::optional<double> volume;
        long long rows = 0;
        std::map<std::string, StockEmitter> emitters; // ambience only: looping / nonlooping
    };

    struct GroupLevel // stock_group_levels: median stock VolMax of a class
    {
        double volume = 0;
        long long rows = 0;
    };

    struct StockFalloff
    {
        std::string dryMaxCurve, wetMaxCurve;
        double area = 0;
        std::vector<std::string> falloffCurves;
    };

    struct StockClass // stock_classes: a VolumeGroup|PanType class of the stock BO2 aliases
    {
        long long rows = 0;
        AliasMix routing; // Bus, DuckGroup
        std::map<std::string, std::vector<double>> quantiles;
        std::optional<StockFalloff> falloff;
    };

    struct StockProfiles
    {
        std::map<std::string, RoleProfile> roles;
        std::map<std::string, GroupLevel> groups;
        std::map<std::string, StockClass> classes;
        std::map<std::string, AliasMix> twins; // stock alias -> its placement columns
    };

    struct BusVote // a WaW bus's stock BO2 group, learned from the engine sounds both games name
    {
        std::string group;
        long long twins = 0, of = 0;
    };

    struct SoundClasses // measure_sound_classes: the WaW bus -> BO2 group table of this build
    {
        std::map<std::string, std::string> busGroups;
        std::map<std::string, std::string> weaponDependencyGroup;
        std::map<std::string, BusVote> learned;
        std::map<std::string, std::pair<std::optional<std::string>, BusVote>> differs; // declared group, vote
        util::Json ToJson() const;
    };

    struct ReferenceQuantiles // a stock WaW column's percentiles in one class
    {
        std::vector<double> q;
        long long samples = 0;
    };

    struct RemasterTables
    {
        StockProfiles profiles;
        SoundClasses classes;
        std::map<std::string, std::string> twins; // WaW alias -> the stock alias it plays for
        std::map<std::string, std::map<std::string, ReferenceQuantiles>> reference; // class -> column; empty without stock WaW
        std::set<std::string> music, ambience;
        std::map<std::string, std::string> stingers; // WaW alias -> stinger state
        util::Json announcerEvents;                    // content_source/announcer_events.json
        std::string announcerPrefix;
    };

} // namespace w2b::sound
