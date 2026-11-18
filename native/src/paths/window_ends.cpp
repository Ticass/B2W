#include "paths/paths.h"

#include <algorithm>
#include <cstdint>

namespace w2b::paths
{
    namespace
    {
        using Bytes = std::vector<std::uint8_t>;

        unsigned HexDigit(char ch)
        {
            if (ch >= '0' && ch <= '9')
                return static_cast<unsigned>(ch - '0');
            if (ch >= 'a' && ch <= 'f')
                return static_cast<unsigned>(ch - 'a' + 10);
            if (ch >= 'A' && ch <= 'F')
                return static_cast<unsigned>(ch - 'A' + 10);
            throw Error("invalid pathVis hex");
        }

        Bytes Decode(std::string_view hex)
        {
            Bytes result;
            result.reserve(hex.size() / 2);
            for (std::size_t i = 0; i < hex.size();)
            {
                if (std::string_view(" \t\n\r\v\f").find(hex[i]) != hex.npos)
                {
                    ++i;
                    continue;
                }
                if (i + 1 == hex.size())
                    throw Error("invalid pathVis hex");
                result.push_back(static_cast<std::uint8_t>((HexDigit(hex[i]) << 4) | HexDigit(hex[i + 1])));
                i += 2;
            }
            return result;
        }

        std::string Encode(const Bytes& bytes)
        {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string hex(bytes.size() * 2, '\0');
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                hex[i * 2] = digits[bytes[i] >> 4];
                hex[i * 2 + 1] = digits[bytes[i] & 15];
            }
            return hex;
        }

        unsigned ReadByte(const Bytes& data, std::size_t offset)
        {
            return offset < data.size() ? data[offset] : 0;
        }

        unsigned ReadBits(const Bytes& data, std::size_t start, unsigned length)
        {
            const auto offset = start / 8;
            const auto word = ReadByte(data, offset) | (ReadByte(data, offset + 1) << 8);
            return (word >> (start % 8)) & ((1u << length) - 1);
        }

        void SetBits(Bytes& data, std::size_t start, unsigned bits)
        {
            const auto shifted = bits << (start % 8);
            if (shifted == 0)
                return;
            data.at(start / 8) |= static_cast<std::uint8_t>(shifted);
            if ((shifted >> 8) != 0)
                data.at(start / 8 + 1) |= static_cast<std::uint8_t>(shifted >> 8);
        }

        unsigned Visible(const Bytes& data, std::size_t count, std::size_t a, std::size_t b)
        {
            return a == b ? 1 : ReadBits(data, std::min(a, b) * count + std::max(a, b), 1);
        }

        void GrowVisibility(Json& graph, std::size_t count, const std::vector<std::size_t>& hubs)
        {
            const auto vis = graph.find("pathVis");
            if (hubs.empty() || vis == graph.end() || vis->is_null() || vis->get_ref<const std::string&>().empty())
                return;
            const auto total = count + hubs.size();
            if (total > MaxNodes)
                throw Error(std::to_string(total) + " path nodes exceed the uint16 node index range");
            const auto old = Decode(vis->get_ref<const std::string&>());
            Bytes output((total * (total - 1) + 7) / 8);
            for (std::size_t lo = 0; lo < count; ++lo)
            {
                // The old row is still contiguous. Copy in blocks of eight
                // bits even when its old and new bit offsets differ.
                for (std::size_t hi = lo + 1; hi < count; hi += 8)
                {
                    const auto length = static_cast<unsigned>(std::min<std::size_t>(8, count - hi));
                    SetBits(output, lo * total + hi, ReadBits(old, lo * count + hi, length));
                }
                for (std::size_t i = 0; i < hubs.size(); ++i)
                    SetBits(output, lo * total + count + i, Visible(old, count, lo, hubs[i]));
            }
            for (std::size_t lo = 0; lo < hubs.size(); ++lo)
                for (std::size_t hi = lo + 1; hi < hubs.size(); ++hi)
                    SetBits(output, (count + lo) * total + count + hi, Visible(old, count, hubs[lo], hubs[hi]));
            graph["visBytes"] = output.size();
            graph["pathVis"] = Encode(output);
        }
    }

    WindowHubs SplitWindowEnds(Json& graph, const std::vector<std::size_t>& begins)
    {
        auto& nodes = graph.at("nodes");
        const auto count = nodes.size();
        std::map<std::string, std::size_t, std::less<>> targets;
        for (std::size_t i = 0; i < count; ++i)
            if (auto name = nodes.at(i).value("targetname", std::string{}); !name.empty())
                targets[std::move(name)] = i;
        WindowHubs names;
        std::vector<std::size_t> hubs;
        for (const auto index : begins)
        {
            auto& begin = nodes.at(index);
            const auto hub = targets.find(begin.value("target", std::string{}));
            if (hub == targets.end())
                continue;
            auto end = nodes.at(hub->second);
            const auto name = hub->first + "_waw2bo2_window";
            end["targetname"] = name;
            end["target"] = "";
            const auto oldLinks = end.find("links");
            const Json flags = oldLinks != end.end() && !oldLinks->empty() ? oldLinks->at(0).at("flags") : Json(ClearanceLinkFlags);
            end["links"] = Json::array({{{"node", hub->second}, {"dist", 0.0}, {"disconnectCount", 0}, {"negotiationLink", 0}, {"flags", flags}}});
            if (const auto links = begin.find("links"); links != begin.end())
                for (auto& link : *links)
                {
                    const auto negotiation = link.value("negotiationLink", Json(0));
                    if (link.at("node") == hub->second && negotiation != 0 && negotiation != false && !negotiation.is_null())
                        link["node"] = nodes.size();
                }
            begin["target"] = name;
            nodes.push_back(std::move(end));
            hubs.push_back(hub->second);
            names[name] = hub->first;
        }
        const auto total = nodes.size();
        GrowVisibility(graph, count, hubs);
        if (graph.contains("nodeCount"))
            graph["nodeCount"] = total;
        return names;
    }
}
