#pragma once
// gsc.parse / gsc.references / gsc.repair_block_comments.

#include "tokens.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace w2b::gsc
{
    struct Function
    {
        std::string name;
        std::vector<std::string> params;
        size_t start = 0;     // index of the name token
        size_t bodyOpen = 0;  // index of '{'
        size_t bodyClose = 0; // index of matching '}'
        std::optional<std::string> animtree;
    };

    struct Script
    {
        std::string path; // e.g. maps\_access_bunker (no extension)
        std::vector<Token> tokens;
        std::vector<std::string> includes;
        std::vector<std::pair<std::string, Function>> functions; // lower-case name -> Function, definition order
        std::optional<std::string> animtree;
        std::optional<std::string> error; // load_scripts: the syntax error of an unparsable file

        const Function* Find(const std::string& lowName) const;
    };

    // index of the token closing tokens[i] (open_/close_ punctuation)
    size_t Match(const std::vector<Token>& tokens, size_t i, std::string_view open, std::string_view close);

    Script Parse(std::string_view source, const std::string& path);

    struct Ref
    {
        size_t index = 0;
        std::string name;
        std::optional<std::string> qualifier;
        std::optional<size_t> qualIndex;
        bool method = false;
        bool pointer = false;
    };

    std::vector<Ref> References(const std::vector<Token>& tokens, size_t start = 0, std::optional<size_t> end = std::nullopt);

    // repair_block_comments(text) -> (text, count)
    std::pair<std::string, int> RepairBlockComments(std::string_view text);
} // namespace w2b::gsc
