#include "paths/paths.h"

#include "core/files.h"
#include "mapents/entities.h"

#include <cmath>
#include <locale>
#include <numbers>
#include <sstream>

namespace w2b::paths
{
    namespace
    {
        using mapents::Field;
        constexpr double WindowRadius = 48.0;

        struct Candidate
        {
            std::size_t index;
            double x, y, dx, dy;
            bool used = false;
        };

        std::vector<Candidate> Negotiations(const Json& nodes)
        {
            std::map<std::string_view, const Json*, std::less<>> targets;
            for (const auto& node : nodes)
                if (const auto name = Field(node, "targetname"); !name.empty())
                    targets[name] = &node;
            std::vector<Candidate> candidates;
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                const auto& node = nodes.at(i);
                if (node.at("type") != T6NegotiationBegin)
                    continue;
                const auto end = targets.find(Field(node, "target"));
                if (end == targets.end())
                    continue;
                const auto& point = node.at("origin");
                const auto& inside = end->second->at("origin");
                const auto x = point.at(0).get<double>(), y = point.at(1).get<double>();
                candidates.push_back({i, x, y, inside.at(0).get<double>() - x, inside.at(1).get<double>() - y});
            }
            return candidates;
        }

        double Yaw(const Json& goal)
        {
            std::istringstream stream{std::string(Field(goal, "angles", "0 0 0"))};
            stream.imbue(std::locale::classic());
            double pitch{}, yaw{};
            if (!(stream >> pitch >> yaw))
                throw Error("invalid barrier angles: " + std::string(Field(goal, "angles")));
            return yaw * (std::numbers::pi / 180.0);
        }

        Candidate* Closest(std::vector<Candidate>& candidates, const Json& goal)
        {
            const auto origin = mapents::Origin(goal);
            const auto yaw = Yaw(goal);
            const auto forwardX = std::cos(yaw), forwardY = std::sin(yaw);
            double closest = WindowRadius;
            Candidate* best = nullptr;
            for (auto& candidate : candidates)
            {
                if (candidate.used || !(candidate.dx * forwardX + candidate.dy * forwardY > 0))
                    continue;
                const auto distance = std::hypot(candidate.x - origin[0], candidate.y - origin[1]);
                if (distance <= WindowRadius && (!best || distance < closest))
                {
                    best = &candidate;
                    closest = distance;
                }
            }
            return best;
        }

        bool SameOrigin(const Json& entity, const Json& pathOrigin)
        {
            // Entity origins are decimal text, while the dump stores float32
            // positions, so compare each component with the source tolerance.
            std::istringstream source{std::string(Field(entity, "origin", "0 0 0"))};
            source.imbue(std::locale::classic());
            for (const auto& component : pathOrigin)
            {
                double x{};
                source >> std::ws;
                if (source.eof())
                    break;
                if (!(source >> x))
                    throw Error("invalid negotiation entity origin: " + std::string(Field(entity, "origin")));
                if (!(std::abs(x - component.get<double>()) < 0.05))
                    return false;
            }
            return true;
        }

        bool AssignBegin(Json& entities, Json& node, const std::string& window)
        {
            const auto old = Field(node, "targetname");
            for (auto& ent : entities)
                if (Field(ent, "classname") == "node_negotiation_begin" && Field(ent, "targetname") == old &&
                    SameOrigin(ent, node.at("origin")))
                {
                    ent["targetname"] = window;
                    node["targetname"] = window;
                    return true;
                }
            return false;
        }

        std::string DescribeOrigin(const Json& pathOrigin)
        {
            std::string text("[");
            for (const auto& component : pathOrigin)
            {
                if (text.size() != 1)
                    text += ", ";
                text += py::JsonDumps(component, -1);
            }
            return text + ']';
        }

        std::vector<std::size_t> SelectWindows(Json& entities, Json& nodes, Json& notes)
        {
            auto candidates = Negotiations(nodes);
            std::vector<std::size_t> begins;
            for (const auto& goal : entities)
            {
                if (Field(goal, "targetname") != "exterior_goal")
                    continue;
                const auto window = std::string(Field(goal, "target"));
                if (window.empty())
                    continue;
                const auto candidate = Closest(candidates, goal);
                if (!candidate)
                {
                    notes.push_back("UNSUPPORTED_BARRIER_TRAVERSAL " + window + " at " + std::string(Field(goal, "origin", "None")) +
                                    ": no inward negotiation within 48 units; path stays connected while boarded");
                    continue;
                }
                candidate->used = true;
                auto& node = nodes.at(candidate->index);
                if (AssignBegin(entities, node, window))
                    begins.push_back(candidate->index);
                else
                    notes.push_back("UNSUPPORTED_BARRIER_TRAVERSAL " + window + ": node entity at " +
                                    DescribeOrigin(node.at("origin")) + " not found");
            }
            return begins;
        }

        void CopyEndpoint(Json& entities, const Json& node, const WindowHubs& hubs, Json& notes)
        {
            const auto window = Field(node, "targetname"), name = Field(node, "target");
            const auto hub = hubs.find(name);
            const Json* original = nullptr;
            std::size_t count = 0;
            for (const auto& ent : entities)
            {
                if (Field(ent, "classname") != "node_negotiation_end")
                    continue;
                const auto target = ent.find("targetname");
                if (hub == hubs.end() ? target == ent.end() || target->is_null() : Field(ent, "targetname") == hub->second)
                {
                    original = &ent;
                    ++count;
                }
            }
            if (hub == hubs.end() || count != 1)
            {
                notes.push_back("UNSUPPORTED_BARRIER_TRAVERSAL " + std::string(window) + ": " + std::to_string(count) +
                                " end node entities; its own end " + std::string(name) + " has no entity, so GetNode cannot find it");
                return;
            }
            auto end = *original;
            end["targetname"] = name;
            entities.push_back(std::move(end));
            for (auto& ent : entities)
                if (Field(ent, "classname") == "node_negotiation_begin" && Field(ent, "targetname") == window)
                    ent["target"] = name;
        }
    }

    Json LinkWindowTraversals(const Path& bsp)
    {
        const auto entitiesFile = bsp / "entities.json", pathsFile = bsp / "paths.json";
        auto entities = ReadManifest(entitiesFile), graph = ReadManifest(pathsFile);
        auto notes = Json::array();
        auto& entries = entities.at("entities");
        const auto begins = SelectWindows(entries, graph.at("nodes"), notes);
        const auto hubs = SplitWindowEnds(graph, begins);
        for (const auto begin : begins)
            CopyEndpoint(entries, graph.at("nodes").at(begin), hubs, notes);
        const auto entityText = py::JsonDumps(entities, 1) + '\n';
        const auto graphText = py::JsonDumps(graph, -1) + '\n';
        files::WriteText(entitiesFile, entityText);
        files::WriteText(pathsFile, graphText);
        return notes;
    }
}
