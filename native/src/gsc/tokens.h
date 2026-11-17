#pragma once
// Lossless GSC/CSC tokenizer (port of gsc.py): every token keeps the
// whitespace/comments before it, so Emit(tokens) reproduces the source.

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace w2b::gsc
{
    enum class Kind
    {
        Ident,
        Path,
        String,
        Number,
        Punct,
        Directive,
        Eof
    };

    const char* KindName(Kind kind); // "ident", "path", ... as gsc.py names them

    struct Token
    {
        Kind kind = Kind::Eof;
        std::string text; // UTF-8
        std::string pre;  // whitespace/comments before the token
        int line = 0;

        std::string Low() const; // text.lower()
    };

    class GscSyntaxError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // gsc.tokenize (source is UTF-8 text, as Python decoded it)
    std::vector<Token> Tokenize(std::string_view source);

    // gsc.emit
    std::string Emit(const std::vector<Token>& tokens);

    // str.lower() for identifiers and paths
    std::string Lower(std::string_view text);

    // repr(str) as Python prints it in messages
    std::string Repr(std::string_view text);
} // namespace w2b::gsc
