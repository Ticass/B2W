#pragma once
// gscport.PortReport, with entries in insertion order.

#include "core/text.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace w2b::gsc::port
{
    // collections.Counter / dict of lists: insertion-ordered
    template<typename V> struct OrderedMap
    {
        std::vector<std::string> order;
        std::unordered_map<std::string, V> values;

        V& operator[](const std::string& key)
        {
            const auto it = values.find(key);
            if (it != values.end())
                return it->second;
            order.push_back(key);
            return values[key];
        }
        bool Has(const std::string& key) const
        {
            return values.count(key) != 0;
        }
        const V* Find(const std::string& key) const
        {
            const auto it = values.find(key);
            return it == values.end() ? nullptr : &it->second;
        }
    };

    struct PortReport
    {
        std::string mapMain;
        std::vector<std::string> ported, coreOverrides, extracted;
        OrderedMap<std::vector<std::string>> unsupported;
        OrderedMap<long long> rewrites; // report.rewrites[k] += n creates k even for n == 0
        std::vector<std::string> fx;
        OrderedMap<std::vector<std::string>> animtrees;
        std::vector<std::string> errors;
        bool characters = false;
        util::Json weaponRegistration = util::Json::object();
        std::vector<std::string> wawHudMaterials;
        OrderedMap<std::string> nativeActorTrees;
        std::vector<std::string> clientListenerStrings;

        void Unsupported(const std::string& key, const std::string& where)
        {
            unsupported[key].push_back(where);
        }

        util::Json ToJson() const;
    };
} // namespace w2b::gsc::port
