#include "link_check.h"

#include "core/encoding.h"
#include "core/files.h"
#include "gsc/ast.h"
#include "util.h"

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_map>

namespace w2b::gsc::port
{
    namespace fs = std::filesystem;

    namespace
    {
        struct Loaded
        {
            std::string path; // lower case, no extension
            std::vector<Token> tokens;
            Ast ast;
            std::vector<std::string> includes;
            std::map<std::string, int> functions; // lower-case name -> parameters
            std::string error;
        };

        // "#include maps\x;" -> maps\x
        std::optional<std::string> IncludePath(const std::string& directive)
        {
            constexpr std::string_view word = "#include";
            if (directive.size() < word.size() || Lower(directive.substr(0, word.size())) != word)
                return std::nullopt;
            std::string path;
            for (const char c : directive.substr(word.size()))
                if (c != ' ' && c != '\t' && c != ';')
                    path += c == '/' ? '\\' : c;
            return Lower(path);
        }

        class Side
        {
        public:
            Side(const fs::path& root, const std::string& extension, const T6Api& api)
                : api_(api), client_(extension == ".csc")
            {
                auto files = files::GlobSuffix(root, extension);
                std::sort(files.begin(), files.end());
                for (const auto& file : files)
                {
                    Loaded script;
                    auto relative = files::Utf8(file.lexically_relative(root));
                    relative.resize(relative.size() - extension.size());
                    std::replace(relative.begin(), relative.end(), '/', '\\');
                    script.path = Lower(relative);
                    try
                    {
                        std::string bytes;
                        files::ReadBytes(file, bytes);
                        script.tokens = Tokenize(encoding::UniversalNewlines(encoding::Utf8SigReplace(bytes)));
                        script.ast = ParseAst(script.tokens, Dialect::Bo2);
                    }
                    catch (const GscSyntaxError& error)
                    {
                        script.error = error.what();
                        script.ast = Ast{};
                    }
                    if (script.error.empty())
                        for (const auto id : script.ast[script.ast.root].kids)
                        {
                            const auto& n = script.ast[id];
                            if (n.kind == NodeKind::Directive)
                            {
                                if (auto include = IncludePath(script.tokens[n.token].text))
                                    script.includes.push_back(std::move(*include));
                            }
                            else if (n.kind == NodeKind::Function)
                            {
                                const auto name = script.tokens[n.begin].Low();
                                if (!script.functions.emplace(name, static_cast<int>(n.kids.size()) - 1).second)
                                    duplicates_.push_back({script.path, n.begin, name});
                            }
                        }
                    index_.emplace(script.path, scripts_.size());
                    scripts_.push_back(std::move(script));
                }
            }

            void Check(const std::vector<std::string>& prefixes, std::vector<std::string>& problems) const
            {
                for (const auto& script : scripts_)
                {
                    if (!prefixes.empty() && std::none_of(prefixes.begin(), prefixes.end(), [&](const std::string& p) { return script.path.starts_with(p); }))
                        continue;
                    if (!script.error.empty())
                    {
                        problems.push_back(Name(script.path) + ": " + script.error);
                        continue;
                    }
                    for (const auto& include : script.includes)
                        if (!Exists(include))
                            problems.push_back(Name(script.path) + ": #include " + include + " does not exist");
                    for (const auto& [path, token, name] : duplicates_)
                        if (path == script.path)
                            problems.push_back(Name(path) + ":" + std::to_string(script.tokens[token].line) + ": function " + name + " is defined twice");
                    for (const auto function : script.ast.Functions())
                        CheckFunction(script, function, problems);
                }
            }

        private:
            struct Duplicate
            {
                std::string path;
                uint32_t token;
                std::string name;
            };

            std::string Name(const std::string& path) const
            {
                return path + (client_ ? ".csc" : ".gsc");
            }

            bool Exists(const std::string& path) const
            {
                return index_.count(path) || api_.scriptIndex.count(path);
            }

            std::optional<int> Parameters(const std::string& path, const std::string& name) const
            {
                if (const auto own = index_.find(path); own != index_.end())
                {
                    const auto& script = scripts_[own->second];
                    if (!script.error.empty())
                        return 64; // reported once, for the script
                    const auto f = script.functions.find(name);
                    return f == script.functions.end() ? std::nullopt : std::optional<int>(f->second);
                }
                const auto stock = api_.scriptIndex.find(path);
                if (stock == api_.scriptIndex.end())
                    return std::nullopt;
                const auto& functions = api_.scripts[stock->second].second;
                const auto f = functions.find(name);
                return f == functions.end() ? std::nullopt : std::optional<int>(f->second);
            }

            std::optional<Range> Builtin(const std::string& name, const std::optional<bool> method) const
            {
                if (!client_)
                    return api_.BuiltinRange(name, method);
                std::optional<Range> out;
                const auto find = [&](const std::unordered_map<std::string, Range>& table)
                {
                    const auto it = table.find(name);
                    if (it == table.end())
                        return;
                    out = out ? Range{std::min(out->lo, it->second.lo), std::max(out->hi, it->second.hi)} : it->second;
                };
                if (!method || !*method)
                    find(api_.clientBuiltins);
                if (!method || *method)
                    find(api_.clientMethods);
                return out;
            }

            void CheckFunction(const Loaded& script, const NodeId function, std::vector<std::string>& problems) const
            {
                const auto& ast = script.ast;
                const auto where = [&](const uint32_t token)
                {
                    return Name(script.path) + ":" + std::to_string(script.tokens[token].line) + " (function " + script.tokens[ast[function].token].text + "): ";
                };
                std::vector<NodeId> stack{function};
                while (!stack.empty())
                {
                    const auto id = stack.back();
                    stack.pop_back();
                    const auto& n = ast[id];
                    // T6 compiles "/# ... #/" out of the scripts it loads
                    if (n.kind == NodeKind::DevBlock)
                        continue;
                    stack.insert(stack.end(), n.kids.begin(), n.kids.end());
                    const bool call = n.kind == NodeKind::Call && ast[n.kids[0]].kids.empty();
                    if (!call && n.kind != NodeKind::FunctionRef)
                        continue;
                    const auto& target = call ? ast[n.kids[0]] : n;
                    const auto& text = script.tokens[target.token].text;
                    const auto name = Lower(text);
                    const int arguments = call ? static_cast<int>(n.kids.size()) - (n.method ? 2 : 1) : 0;
                    std::optional<int> parameters;
                    if (target.qualifier)
                    {
                        auto path = script.tokens[target.qualifier - 1].Low();
                        std::replace(path.begin(), path.end(), '/', '\\');
                        parameters = Parameters(path, name);
                        if (!parameters)
                        {
                            problems.push_back(where(target.token) + path + "::" + text + (Exists(path) ? " is not defined there" : ": that script does not exist"));
                            continue;
                        }
                    }
                    else
                    {
                        parameters = Parameters(script.path, name);
                        for (const auto& include : script.includes)
                            if (!parameters)
                                parameters = Parameters(include, name);
                    }
                    if (parameters)
                    {
                        if (arguments > *parameters)
                            problems.push_back(where(target.token) + text + " is called with " + std::to_string(arguments) + " arguments, it takes "
                                               + std::to_string(*parameters));
                        continue;
                    }
                    const auto method = call ? std::optional<bool>(n.method) : std::nullopt;
                    const auto range = Builtin(name, method);
                    if (!range)
                    {
                        const auto other = call ? Builtin(name, !n.method) : std::nullopt;
                        problems.push_back(where(target.token) + text + " is neither defined, included nor a T6 " + (client_ ? "client " : "") + "builtin"
                                           + (other ? n.method ? " method (it is a builtin function)" : " function (it is a builtin method)" : ""));
                    }
                    else if (call && (arguments < range->lo || arguments > range->hi))
                        problems.push_back(where(target.token) + "builtin " + text + " is called with " + std::to_string(arguments) + " arguments, T6 takes "
                                           + std::to_string(range->lo) + "-" + std::to_string(range->hi));
                }
            }

            const T6Api& api_;
            bool client_;
            std::vector<Loaded> scripts_;
            std::unordered_map<std::string, size_t> index_;
            std::vector<Duplicate> duplicates_;
        };
    } // namespace

    std::vector<std::string> LinkCheck(const fs::path& root, const T6Api& api, const std::vector<std::string>& prefixes)
    {
        std::vector<std::string> problems;
        Side(root, ".gsc", api).Check(prefixes, problems);
        Side(root, ".csc", api).Check(prefixes, problems);
        return problems;
    }
} // namespace w2b::gsc::port
