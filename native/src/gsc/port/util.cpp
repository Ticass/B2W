#include "util.h"

#include "core/unicode.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace w2b::gsc::port
{
    void ThrowIndexError(const long long index, const size_t size)
    {
        if (const char* stack = std::getenv("W2B_STACK"); stack && std::string(stack) == "1")
        {
            void* frames[48];
            const auto count = CaptureStackBackTrace(1, 48, frames, nullptr);
            const auto process = GetCurrentProcess();
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
            SymInitialize(process, nullptr, TRUE);
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            for (USHORT f = 0; f < count; f++)
            {
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 511;
                DWORD64 displacement = 0;
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof line;
                DWORD lineDisplacement = 0;
                const auto address = reinterpret_cast<DWORD64>(frames[f]);
                const bool named = SymFromAddr(process, address, &displacement, symbol);
                const bool lined = SymGetLineFromAddr64(process, address, &lineDisplacement, &line);
                std::fprintf(stderr, "  at %s %s:%lu\n", named ? symbol->Name : "?", lined ? line.FileName : "?", lined ? line.LineNumber : 0UL);
            }
        }
        throw std::out_of_range("list index out of range (index " + std::to_string(index) + ", size " + std::to_string(size) + ")");
    }

    std::string EmitRange(const Tokens& tokens, const long long a, const long long b)
    {
        const auto [lo, hi] = Slice(tokens.size(), a, b);
        std::string out;
        for (size_t i = lo; i < hi; i++)
        {
            out += tokens[i].pre;
            out += tokens[i].text;
        }
        return out;
    }

    std::string JoinLow(const Tokens& tokens, const long long a, const long long b)
    {
        const auto [lo, hi] = Slice(tokens.size(), a, b);
        std::string out;
        for (size_t i = lo; i < hi; i++)
            out += tokens[i].Low();
        return out;
    }

    std::string JoinText(const Tokens& tokens, const long long a, const long long b)
    {
        const auto [lo, hi] = Slice(tokens.size(), a, b);
        std::string out;
        for (size_t i = lo; i < hi; i++)
            out += tokens[i].text;
        return out;
    }

    namespace
    {
        std::string Trim(const std::string_view text, const bool left, const bool right)
        {
            auto s = unicode::Decode(text);
            size_t a = 0, b = s.size();
            if (left)
                while (a < b && unicode::IsSpace(s[a]))
                    a++;
            if (right)
                while (b > a && unicode::IsSpace(s[b - 1]))
                    b--;
            return unicode::Encode(std::u32string_view(s).substr(a, b - a));
        }
    } // namespace

    std::string PyStrip(const std::string_view text)
    {
        return Trim(text, true, true);
    }

    std::string PyLStrip(const std::string_view text)
    {
        return Trim(text, true, false);
    }

    std::string PyRStrip(const std::string_view text)
    {
        return Trim(text, false, true);
    }

    std::string StripChars(std::string_view text, const std::string_view chars)
    {
        while (!text.empty() && chars.find(text.front()) != std::string_view::npos)
            text.remove_prefix(1);
        while (!text.empty() && chars.find(text.back()) != std::string_view::npos)
            text.remove_suffix(1);
        return std::string(text);
    }

    std::string RStripChars(std::string_view text, const std::string_view chars)
    {
        while (!text.empty() && chars.find(text.back()) != std::string_view::npos)
            text.remove_suffix(1);
        return std::string(text);
    }

    bool WordsEqual(const Tokens& tokens, const long long a, const std::vector<std::string>& words)
    {
        const auto [lo, hi] = Slice(tokens.size(), a, a + static_cast<long long>(words.size()));
        if (hi - lo != words.size())
            return false;
        for (size_t i = 0; i < words.size(); i++)
            if (tokens[lo + i].Low() != words[i])
                return false;
        return true;
    }

    long long PathStart(const Tokens& tokens, const long long dot)
    {
        long long j = dot, depth = 0;
        while (j > 0)
        {
            const auto& prev = At(tokens, j - 1);
            if (depth == 0 && prev.text != "."
                && !((prev.kind == Kind::Ident || prev.text == "]" || prev.text == ")") && (At(tokens, j).text == "." || At(tokens, j).text == "[")))
                break;
            if (prev.text == "]" || prev.text == ")")
                depth++;
            else if (prev.text == "[" || prev.text == "(")
                depth--;
            j--;
        }
        return j;
    }

    std::string Replace(std::string text, const std::string_view from, const std::string_view to)
    {
        if (from.empty())
            return text;
        size_t at = 0;
        while ((at = text.find(from, at)) != std::string::npos)
        {
            text.replace(at, from.size(), to);
            at += to.size();
        }
        return text;
    }

    std::string Join(const std::vector<std::string>& parts, const std::string_view sep)
    {
        std::string out;
        for (size_t i = 0; i < parts.size(); i++)
        {
            if (i)
                out += sep;
            out += parts[i];
        }
        return out;
    }
} // namespace w2b::gsc::port
