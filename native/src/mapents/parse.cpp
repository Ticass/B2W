#include "mapents/entities.h"

#include "core/unicode.h"

#include <algorithm>

namespace w2b::mapents
{
    namespace
    {
        std::size_t SkipSpace(std::string_view text, std::size_t pos)
        {
            while (pos < text.size())
            {
                const auto byte = static_cast<unsigned char>(text[pos]);
                if (byte < 0x80)
                {
                    if (!py::IsSpace(text[pos]))
                        break;
                    ++pos;
                    continue;
                }
                std::size_t end = pos + 1;
                const auto utf8Limit = std::min(pos + 4, text.size());
                while (end < utf8Limit && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
                    ++end;
                const auto codepoint = unicode::Decode(text.substr(pos, end - pos));
                if (codepoint.size() != 1 || !unicode::IsSpace(codepoint.front()))
                    break;
                pos = end;
            }
            return pos;
        }

        Json ParseBlock(std::string_view text)
        {
            auto entity = Json::object();
            auto keyStart = text.find('"');
            while (keyStart != text.npos)
            {
                const auto keyEnd = text.find('"', keyStart + 1);
                if (keyEnd == text.npos)
                    break;
                const auto valueStart = SkipSpace(text, keyEnd + 1);
                if (valueStart == keyEnd + 1 || valueStart == text.size() || text[valueStart] != '"')
                {
                    keyStart = keyEnd;
                    continue;
                }
                const auto valueEnd = text.find('"', valueStart + 1);
                if (valueEnd == text.npos)
                    break;
                entity[text.substr(keyStart + 1, keyEnd - keyStart - 1)] = text.substr(valueStart + 1, valueEnd - valueStart - 1);
                keyStart = text.find('"', valueEnd + 1);
            }
            return entity;
        }
    }

    Json Parse(std::string_view text)
    {
        auto entities = Json::array();
        auto brace = text.find('{');
        while (brace != text.npos)
        {
            const auto end = text.find_first_of("{}", brace + 1);
            if (end == text.npos)
                break;
            if (text[end] == '{')
            {
                brace = end;
                continue;
            }
            entities.push_back(ParseBlock(text.substr(brace + 1, end - brace - 1)));
            brace = text.find('{', end + 1);
        }
        return entities;
    }
}
