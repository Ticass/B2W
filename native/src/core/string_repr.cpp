#include "core/text.h"

#include "core/encoding.h"
#include "core/unicode.h"

#include <algorithm>
#include <iterator>

namespace w2b::util
{
    namespace
    {
        struct Range { char32_t first, last; };
        constexpr Range printable[] = {
#include "core/printable_ranges.inc"
        };

        bool IsPrintable(char32_t point)
        {
            if (point < 0x80)
                return point >= 0x20 && point < 0x7f;
            const auto range = std::lower_bound(std::begin(printable), std::end(printable), point,
                [](const Range& r, char32_t value) { return r.last < value; });
            return range != std::end(printable) && range->first <= point;
        }

        void AppendEscape(std::string& text, char32_t value)
        {
            const int digits = value < 0x100 ? 2 : value < 0x10000 ? 4 : 8;
            text += '\\';
            text += digits == 2 ? 'x' : digits == 4 ? 'u' : 'U';
            for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
                text += "0123456789abcdef"[(value >> shift) & 0xf];
        }
    }

    std::string StringRepr(std::string_view value)
    {
        const char quote = value.find('\'') != value.npos && value.find('"') == value.npos ? '"' : '\'';
        std::string result(1, quote);
        result.reserve(value.size() + 2);
        for (const auto point : unicode::Decode(value))
        {
            if (point == static_cast<char32_t>(quote) || point == '\\')
                result += '\\';
            switch (point)
            {
            case '\t': result += "\\t"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            default:
                if (IsPrintable(point))
                    encoding::AppendUtf8(result, point);
                else
                    AppendEscape(result, point);
            }
        }
        result += quote;
        return result;
    }
}
