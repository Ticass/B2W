#include "barriers/barriers.h"

#include "core/pyre.h"
#include "core/unicode.h"

namespace w2b::barriers
{
    Json RepairTiming(std::string_view script)
    {
        static const pyre::Regex start(R"(^replace_chunk\s*\()", pyre::MULTILINE | pyre::IGNORECASE);
        static const pyre::Regex next(R"(^[A-Za-z_]\w*\s*\([^;{]*\)\s*$\s*\{)", pyre::MULTILINE);
        static const pyre::Regex comments(R"(//[^\n]*)");
        static const pyre::Regex lift(R"(MoveTo\s*\(\s*only_z\s*,\s*(\d*\.?\d+)\s*\)\s*;\s*\w+\s+RotateTo\s*\([^;]*,\s*(\d*\.?\d+)\s*\)\s*;[^;]*;\s*wait\s*\(\s*(\d*\.?\d+)\s*\))",
                                      pyre::IGNORECASE);
        static const pyre::Regex push(R"(MoveTo\s*\(\s*\w+\.og_origin\s*,\s*(\d*\.?\d+)\s*\))", pyre::IGNORECASE);
        const auto begin = start.Search(script);
        if (!begin)
            return nullptr;
        const auto decoded = unicode::Decode(script);
        const auto rest = unicode::Encode(std::u32string_view(decoded).substr(static_cast<std::size_t>(begin->End())));
        const auto end = next.Search(rest);
        const auto section = end
            ? unicode::Encode(std::u32string_view(unicode::Decode(rest)).substr(0, static_cast<std::size_t>(end->Start())))
            : rest;
        const auto body = comments.Sub("", section);
        const auto times = lift.Search(body);
        const auto moves = push.FindAll(body);
        if (!times || moves.empty())
            return nullptr;
        const auto number = [](const std::string& value) {
            double seconds{};
            if (!py::ParseFloat(value, seconds))
                throw Error("invalid barricade timing: " + value);
            return seconds;
        };
        return {{"lift", number((*times)[1])}, {"turn", number((*times)[2])}, {"hold", number((*times)[3])},
                {"push", number(moves.back().front())}};
    }
}
