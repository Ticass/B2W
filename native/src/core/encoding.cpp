#include "encoding.h"

namespace w2b::encoding
{
    namespace
    {
        // 0x80..0x9F; 0 marks the five bytes cp1252 leaves undefined
        constexpr unsigned short CP1252_HIGH[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                                    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                                    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

        constexpr unsigned short CP437_HIGH[128] = {
            0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
            0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
            0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
            0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
            0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
            0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
            0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
            0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};
    } // namespace

    void AppendUtf8(std::string& out, const unsigned codepoint)
    {
        if (codepoint < 0x80)
            out += static_cast<char>(codepoint);
        else if (codepoint < 0x800)
        {
            out += static_cast<char>(0xC0 | (codepoint >> 6));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else if (codepoint < 0x10000)
        {
            out += static_cast<char>(0xE0 | (codepoint >> 12));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (codepoint >> 18));
            out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
    }

    std::string Cp1252ToUtf8(const std::string_view bytes)
    {
        std::string out;
        out.reserve(bytes.size());
        for (size_t i = 0; i < bytes.size(); i++)
        {
            const auto c = static_cast<unsigned char>(bytes[i]);
            if (c < 0x80)
                out += static_cast<char>(c);
            else if (c < 0xA0)
            {
                const unsigned mapped = CP1252_HIGH[c - 0x80];
                if (!mapped)
                    throw DecodeError("'charmap' codec can't decode byte 0x" + std::string(1, "0123456789abcdef"[c >> 4]) +
                                      std::string(1, "0123456789abcdef"[c & 0xF]) + " in position " + std::to_string(i));
                AppendUtf8(out, mapped);
            }
            else
                AppendUtf8(out, c); // 0xA0..0xFF are Latin-1
        }
        return out;
    }

    std::string Utf8Replace(const std::string_view bytes)
    {
        std::string out;
        out.reserve(bytes.size());
        const size_t n = bytes.size();
        size_t i = 0;
        const auto at = [&](const size_t k) { return static_cast<unsigned char>(bytes[k]); };
        while (i < n)
        {
            const unsigned char c = at(i);
            if (c < 0x80)
            {
                out += static_cast<char>(c);
                i++;
                continue;
            }
            // valid second-byte ranges per lead byte (Unicode table 3-7)
            int need = 0;
            unsigned char lo = 0x80, hi = 0xBF;
            if (c >= 0xC2 && c <= 0xDF)
                need = 1;
            else if (c == 0xE0)
            {
                need = 2;
                lo = 0xA0;
            }
            else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF)
                need = 2;
            else if (c == 0xED)
            {
                need = 2;
                hi = 0x9F;
            }
            else if (c == 0xF0)
            {
                need = 3;
                lo = 0x90;
            }
            else if (c >= 0xF1 && c <= 0xF3)
                need = 3;
            else if (c == 0xF4)
            {
                need = 3;
                hi = 0x8F;
            }
            else
            {
                AppendUtf8(out, 0xFFFD);
                i++;
                continue;
            }
            size_t k = 1;
            bool valid = true;
            for (; k <= static_cast<size_t>(need); k++)
            {
                if (i + k >= n)
                {
                    valid = false;
                    break;
                }
                const unsigned char b = at(i + k);
                const unsigned char rangeLo = k == 1 ? lo : 0x80, rangeHi = k == 1 ? hi : 0xBF;
                if (b < rangeLo || b > rangeHi)
                {
                    valid = false;
                    break;
                }
            }
            if (!valid)
            {
                // the maximal subpart (lead + its valid continuation bytes) -> one U+FFFD
                AppendUtf8(out, 0xFFFD);
                i += k;
                continue;
            }
            out.append(bytes.substr(i, static_cast<size_t>(need) + 1));
            i += static_cast<size_t>(need) + 1;
        }
        return out;
    }

    std::string Utf8SigReplace(const std::string_view bytes)
    {
        if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF")
            return Utf8Replace(bytes.substr(3));
        return Utf8Replace(bytes);
    }

    std::string UniversalNewlines(const std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); i++)
        {
            if (text[i] == '\r')
            {
                out += '\n';
                if (i + 1 < text.size() && text[i + 1] == '\n')
                    i++;
            }
            else
                out += text[i];
        }
        return out;
    }

    std::string WawText(const std::string_view bytes)
    {
        // data.decode("utf-8") succeeds exactly when the replacing decoder changes nothing
        std::string text;
        if (Utf8Replace(bytes) == bytes)
            text = std::string(bytes);
        else
        {
            // cp1252 with errors="replace": undefined bytes become U+FFFD
            for (const char ch : bytes)
            {
                const auto c = static_cast<unsigned char>(ch);
                if (c >= 0x80 && c < 0xA0)
                {
                    try
                    {
                        text += Cp1252ToUtf8(std::string_view(&ch, 1));
                    }
                    catch (const DecodeError&)
                    {
                        AppendUtf8(text, 0xFFFD);
                    }
                }
                else if (c >= 0xA0)
                    AppendUtf8(text, c);
                else
                    text += ch;
            }
        }
        return UniversalNewlines(text);
    }

    std::string Cp437ToUtf8(const std::string_view bytes)
    {
        std::string out;
        for (const char ch : bytes)
        {
            const auto c = static_cast<unsigned char>(ch);
            AppendUtf8(out, c < 0x80 ? c : CP437_HIGH[c - 0x80]);
        }
        return out;
    }
} // namespace w2b::encoding
