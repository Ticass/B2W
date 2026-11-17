#include "unicode.h"

#include "encoding.h"

#include <windows.h>

namespace w2b::unicode
{
    std::u32string Decode(const std::string_view utf8)
    {
        std::u32string out;
        out.reserve(utf8.size());
        for (size_t i = 0; i < utf8.size();)
        {
            const auto c = static_cast<unsigned char>(utf8[i]);
            char32_t cp = c;
            int extra = 0;
            if (c >= 0xF0)
            {
                cp = c & 0x07;
                extra = 3;
            }
            else if (c >= 0xE0)
            {
                cp = c & 0x0F;
                extra = 2;
            }
            else if (c >= 0xC0)
            {
                cp = c & 0x1F;
                extra = 1;
            }
            for (int k = 1; k <= extra && i + static_cast<size_t>(k) < utf8.size(); k++)
                cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + static_cast<size_t>(k)]) & 0x3F);
            out += cp;
            i += static_cast<size_t>(extra) + 1;
        }
        return out;
    }

    std::string Encode(const std::u32string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (const auto c : text)
            encoding::AppendUtf8(out, static_cast<unsigned>(c));
        return out;
    }

    bool IsSpace(const char32_t c)
    {
        if (c < 0x80)
            return c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F);
        return c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F
               || c == 0x205F || c == 0x3000;
    }

    namespace
    {
        // Unicode character type, independent of the C runtime locale
        WORD Type(const char32_t c)
        {
            if (c > 0xFFFF)
                return 0;
            const wchar_t w = static_cast<wchar_t>(c);
            WORD type = 0;
            GetStringTypeW(CT_CTYPE1, &w, 1, &type);
            return type;
        }
    } // namespace

    bool IsDigit(const char32_t c)
    {
        if (c < 0x80)
            return c >= '0' && c <= '9';
        return (Type(c) & C1_DIGIT) != 0;
    }

    bool IsWord(const char32_t c)
    {
        if (c < 0x80)
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        return (Type(c) & (C1_ALPHA | C1_DIGIT)) != 0;
    }
} // namespace w2b::unicode
