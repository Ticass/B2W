// Disassembly analysis (see runtime_analysis.h).
#include "runtime_analysis.h"

#include "core/hash.h"
#include "d3d.h"
#include "core/regex.h"
#include "gsc/tokens.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace w2b::shaders::runtime
{
    namespace
    {
        std::mutex lock;

        // results per assembly text, cached
        template <typename T> using Cache = std::unordered_map<std::string, T>;

        template <typename T> T& Cached(Cache<T>& cache, const std::string& assembly, const std::function<T()>& make)
        {
            {
                const std::lock_guard guard(lock);
                if (const auto it = cache.find(assembly); it != cache.end())
                    return it->second;
            }
            auto value = make();
            const std::lock_guard guard(lock);
            return cache.emplace(assembly, std::move(value)).first->second;
        }

        int Int(const std::string& s)
        {
            return std::stoi(s);
        }

        std::string Translate(std::string mask)
        {
            for (auto& c : mask)
                c = c == 'r' ? 'x' : c == 'g' ? 'y' : c == 'b' ? 'z' : c == 'a' ? 'w' : c;
            return mask;
        }

        std::string Strip(std::string_view s)
        {
            return std::string(util::Strip(s));
        }
    } // namespace

    const Ordered<Register>& Registers(const std::string& assembly)
    {
        static Cache<Ordered<Register>> cache;
        return Cached<Ordered<Register>>(cache, assembly, [&]
                                         {
                                             static const regex::Regex pattern("^//\\s+(\\w+)\\s+([cs])(\\d+)\\s+(\\d+)\\s*$", regex::MULTILINE);
                                             Ordered<Register> result;
                                             for (const auto& m : pattern.FindAll(assembly))
                                                 for (int row = 0; row < Int(m[3]); row++)
                                                     result.Set(m[1] + std::to_string(Int(m[2]) + row), Register{m[0], row});
                                             return result;
                                         });
    }

    std::map<std::string, std::pair<int, std::string>> Signature(const std::string& assembly, const std::string& kind)
    {
        static Cache<std::map<std::string, std::pair<int, std::string>>> cache;
        return Cached<std::map<std::string, std::pair<int, std::string>>>(
            cache, kind + '\0' + assembly,
            [&]
            {
                const regex::Regex block("// " + kind + " signature:(.*?)(?:// (?:Input|Output) signature:|\\n(?:ps|vs)_\\d)", regex::DOTALL);
                const auto match = block.Search(assembly);
                if (!match)
                    throw ShaderError("no native " + kind + " signature");
                static const regex::Regex row("^//\\s+(\\w+)\\s+(\\d+)\\s+([xyzw]+)\\s+\\d+\\s+\\w+\\s+(float|uint|int)", regex::MULTILINE);
                std::map<std::string, std::pair<int, std::string>> result;
                for (const auto& m : row.FindAll((*match)[1]))
                {
                    int width = 0;
                    for (const char c : m[2])
                        width = std::max(width, static_cast<int>(std::string("xyzw").find(c)) + 1);
                    auto key = m[0] + m[1];
                    std::transform(key.begin(), key.end(), key.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
                    result[key] = {width, m[3]};
                }
                return result;
            });
    }

    const std::map<std::string, Field>& Buffers(const std::string& assembly)
    {
        static Cache<std::map<std::string, Field>> cache;
        return Cached<std::map<std::string, Field>>(cache, assembly, [&]
                                                    {
                                                        // the SDK compiler prints "cb3", the Windows 8.1 one a bare slot
                                                        static const regex::Regex resource("^//\\s+(\\w+)\\s+cbuffer\\s+NA\\s+NA\\s+(?:cb)?(\\d+)\\s+\\d+", regex::MULTILINE);
                                                        static const regex::Regex cbuffer("// cbuffer (\\w+)\\s*\\n// \\{(.*?)// \\}", regex::DOTALL);
                                                        static const regex::Regex variable(
                                                            "//\\s+(?:float\\d?(?:x\\d)?|int\\d?|uint\\d?)\\s+(\\w+)(?:\\[(\\d+)\\])?;\\s*// Offset:\\s*(\\d+)\\s+Size:\\s*(\\d+)");
                                                        std::map<std::string, int> resources;
                                                        for (const auto& m : resource.FindAll(assembly))
                                                            resources[m[0]] = Int(m[1]);
                                                        std::map<std::string, Field> result;
                                                        for (const auto& m : cbuffer.FindAll(assembly))
                                                        {
                                                            if (!resources.count(m[0]))
                                                                continue;
                                                            for (const auto& v : variable.FindAll(m[1]))
                                                                result[v[0]] = Field{resources[m[0]], Int(v[2]), Int(v[3])};
                                                        }
                                                        return result;
                                                    });
    }

    std::vector<std::pair<int, Resource>> ResourceNames(const std::string& assembly)
    {
        using Resources = std::vector<std::pair<int, Resource>>;
        static Cache<Resources> cache;
        return Cached<Resources>(cache, assembly, [&]
                                 {
                                     static const regex::Regex pattern("^//\\s+(\\w+)\\s+texture\\s+\\w+\\s+(\\w+)\\s+t?(\\d+)\\s+\\d+", regex::MULTILINE);
                                     Resources out;
                                     for (const auto& m : pattern.FindAll(assembly))
                                     {
                                         const int slot = Int(m[2]);
                                         const auto it = std::find_if(out.begin(), out.end(), [&](const auto& e) { return e.first == slot; });
                                         if (it != out.end())
                                             it->second = Resource{m[0], m[1]};
                                         else
                                             out.emplace_back(slot, Resource{m[0], m[1]});
                                     }
                                     return out;
                                 });
    }

    std::set<std::pair<std::string, char>> TaintedOutputs(const std::string& assembly, const std::set<std::string>& sources)
    {
        static const regex::Regex reg("^(r\\d+|v\\d+|c\\d+|oC\\d+|o\\d+|oDepth)(?:\\.([xyzwrgba]{1,4}))?$");
        static const regex::Regex absCall("^abs\\((.*)\\)$");
        static const std::set<std::string> elementwise{"mov", "add", "sub", "mul", "mad", "min", "max", "slt", "sge", "frc", "abs", "cmp", "lrp", "dsx", "dsy"};
        std::map<std::string, std::set<char>> taint;
        for (const auto rawLine : util::SplitLines(assembly))
        {
            std::string line(rawLine);
            if (const auto at = line.find("//"); at != std::string::npos)
                line.resize(at);
            line = Strip(line);
            const auto space = line.find(' ');
            std::string op = line.substr(0, space);
            const std::string tail = space == std::string::npos ? std::string() : line.substr(space + 1);
            op = op.substr(0, op.find('_'));
            if (tail.empty() || op == "def" || op == "defi" || op.rfind("dcl", 0) == 0 || op == "vs" || op == "ps")
                continue;
            std::vector<std::string> args;
            for (const auto part : util::Split(tail, ","))
                args.push_back(Strip(part));
            const auto dst = reg.FullMatch(args[0]);
            if (!dst)
                continue;
            const auto mask = Translate(dst->Group(2).value_or("xyzw"));
            const auto reads = [&](std::string token, const char* channel)
            {
                while (!token.empty() && token[0] == '-')
                    token.erase(0, 1);
                token = absCall.Sub("\\1", token);
                for (size_t at; (at = token.find("_abs")) != std::string::npos;)
                    token.erase(at, 4);
                const auto m = reg.FullMatch(token);
                if (!m)
                    return false;
                const auto r = (*m)[1];
                auto sw = Translate(m->Group(2).value_or("xyzw"));
                sw += std::string(4 - sw.size(), sw.back());
                if (sources.count(r))
                    return true;
                std::string comps;
                if (!channel)
                    comps = sw.substr(0, op == "dp3" || op == "nrm" ? 3 : op == "dp2add" ? 2 : 4);
                else
                    comps = std::string(1, sw[std::string("xyzw").find(*channel)]);
                const auto it = taint.find(r);
                return it != taint.end() && std::any_of(comps.begin(), comps.end(), [&](const char c) { return it->second.count(c) > 0; });
            };
            std::set<char> fresh;
            for (const char ch : mask)
            {
                bool hit = false;
                for (size_t i = 1; i < args.size() && !hit; i++)
                {
                    if (elementwise.count(op))
                        hit = reads(args[i], &ch);
                    else if (op == "rcp" || op == "rsq" || op == "exp" || op == "log" || op == "pow")
                    {
                        static const char x = 'x';
                        hit = reads(args[i], &x);
                    }
                    else
                        hit = reads(args[i], nullptr);
                }
                if (hit)
                    fresh.insert(ch);
            }
            auto& current = taint[(*dst)[1]];
            for (const char ch : mask)
                current.erase(ch);
            current.insert(fresh.begin(), fresh.end());
        }
        std::set<std::pair<std::string, char>> out;
        for (const auto& [r, comps] : taint)
            if (r.rfind("o", 0) == 0)
                for (const char c : comps)
                    out.insert({r, c});
        return out;
    }

    bool WritesComponent(const std::string& assembly, const std::string& reg, const char component)
    {
        for (const auto rawLine : util::SplitLines(assembly))
        {
            const std::string line(rawLine);
            // line.split(None, 1)
            size_t i = 0;
            while (i < line.size() && util::IsSpace(line[i]))
                i++;
            size_t j = i;
            while (j < line.size() && !util::IsSpace(line[j]))
                j++;
            const auto first = line.substr(i, j - i);
            while (j < line.size() && util::IsSpace(line[j]))
                j++;
            if (first.empty() || j >= line.size())
                continue;
            if (first.rfind("//", 0) == 0 || first.rfind("dcl", 0) == 0 || first.rfind("def", 0) == 0 || first.rfind("vs_", 0) == 0 || first.rfind("ps_", 0) == 0)
                continue;
            auto rest = line.substr(j);
            const auto dest = Strip(rest.substr(0, rest.find(',')));
            const auto dot = dest.find('.');
            const auto name = dest.substr(0, dot);
            const auto mask = dot == std::string::npos ? std::string() : dest.substr(dot + 1);
            if (name == reg && (mask.empty() || mask.find(component) != std::string::npos))
                return true;
        }
        return false;
    }

    std::string RegisterRepr(const Register* reg)
    {
        if (!reg)
            return "None";
        return "(" + gsc::Repr(reg->name) + ", " + std::to_string(reg->row) + ")";
    }
} // namespace w2b::shaders::runtime
