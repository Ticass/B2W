// Developer diagnostics (see dev_tools.h).
#include "cli/dev_tools.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/text.h"
#include "extract/cache.h"
#include "gsc/ast.h"
#include "gsc/port/link_check.h"
#include "gsc/port/vm_rules.h"
#include "launcher/settings.h"
#include "pipeline/knowledge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <stdexcept>

namespace w2b::cli
{
    namespace
    {
        namespace fs = std::filesystem;

        void Print(const std::string& line)
        {
            std::printf("%s\n", line.c_str());
        }

        // a little-endian value at `offset`, bounds checked
        template<typename T> T At(const std::string& data, const std::uint64_t offset)
        {
            if (offset + sizeof(T) > data.size())
                throw std::runtime_error(std::format("read past the end of the file at 0x{:x}", offset));
            T value;
            std::memcpy(&value, data.data() + offset, sizeof(T));
            return value;
        }

        std::string ReadFile(const fs::path& path)
        {
            std::string data;
            if (!files::ReadBytes(path, data))
                throw std::runtime_error("cannot read " + files::Utf8(path));
            return data;
        }

        // ---- minidumps ----------------------------------------------------

        struct Module
        {
            std::uint64_t base, size;
            std::string name;
        };

        class Minidump
        {
        public:
            explicit Minidump(std::string data) : data_(std::move(data))
            {
                if (At<std::uint32_t>(data_, 0) != 0x504D444D) // "MDMP"
                    throw std::runtime_error("not a minidump");
                const auto count = At<std::uint32_t>(data_, 8), directory = At<std::uint32_t>(data_, 12);
                for (std::uint32_t i = 0; i < count; i++)
                {
                    const auto entry = directory + i * 12ull;
                    streams_.try_emplace(At<std::uint32_t>(data_, entry), At<std::uint32_t>(data_, entry + 8));
                }
                if (const auto list = Stream(4))
                    for (std::uint32_t i = 0, n = At<std::uint32_t>(data_, *list); i < n; i++)
                    {
                        const auto entry = *list + 4 + i * 108ull;
                        modules_.push_back({At<std::uint64_t>(data_, entry), At<std::uint32_t>(data_, entry + 8), Name(At<std::uint32_t>(data_, entry + 20))});
                    }
            }

            std::optional<std::uint32_t> Stream(const std::uint32_t type) const
            {
                const auto found = streams_.find(type);
                return found == streams_.end() ? std::nullopt : std::optional(found->second);
            }

            const std::string& Data() const
            {
                return data_;
            }

            // "module+0xOFFSET" of an address inside a loaded module
            std::optional<std::string> Where(const std::uint64_t address) const
            {
                for (const auto& module : modules_)
                    if (module.base <= address && address < module.base + module.size)
                        return std::format("{}+0x{:X}", module.name.substr(module.name.find_last_of('\\') + 1), address - module.base);
                return std::nullopt;
            }

            // dumped process memory: the memory list (stream 5) or memory64 list (stream 9)
            std::optional<std::string> Read(const std::uint64_t address, const std::uint64_t length) const
            {
                if (const auto list = Stream(5))
                    for (std::uint32_t i = 0, n = At<std::uint32_t>(data_, *list); i < n; i++)
                    {
                        const auto entry = *list + 4 + i * 16ull;
                        const auto start = At<std::uint64_t>(data_, entry), size = std::uint64_t{At<std::uint32_t>(data_, entry + 8)};
                        if (start <= address && address + length <= start + size)
                            return data_.substr(At<std::uint32_t>(data_, entry + 12) + (address - start), length);
                    }
                if (const auto list = Stream(9))
                {
                    const auto n = At<std::uint64_t>(data_, *list);
                    auto offset = At<std::uint64_t>(data_, *list + 8);
                    for (std::uint64_t i = 0; i < n; i++)
                    {
                        const auto start = At<std::uint64_t>(data_, *list + 16 + i * 16), size = At<std::uint64_t>(data_, *list + 24 + i * 16);
                        if (start <= address && address + length <= start + size)
                            return data_.substr(offset + (address - start), length);
                        offset += size;
                    }
                }
                return std::nullopt;
            }

        private:
            std::string Name(const std::uint32_t rva) const
            {
                const auto bytes = At<std::uint32_t>(data_, rva);
                std::wstring text(bytes / 2, L'\0');
                if (rva + 4ull + bytes > data_.size())
                    throw std::runtime_error("module name past the end of the dump");
                std::memcpy(text.data(), data_.data() + rva + 4, bytes);
                return files::Utf8(fs::path(text));
            }

            std::string data_;
            std::map<std::uint32_t, std::uint32_t> streams_; // type -> rva, first of each type
            std::vector<Module> modules_;
        };

        bool SystemModule(std::string name)
        {
            std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const char* prefix : {"ntdll", "kernel", "ucrt", "msvcp", "vcruntime", "win32u", "user32", "kernelbase"})
                if (name.starts_with(prefix))
                    return true;
            return false;
        }

        // the exception of a 32-bit process (Plutonium t6zm): x86 CONTEXT offsets
        void SummarizeDump(const fs::path& path)
        {
            const Minidump dump(ReadFile(path));
            const auto& d = dump.Data();
            Print(files::Utf8(path.filename()));
            const auto exception = dump.Stream(6);
            if (!exception)
            {
                Print("  no exception stream");
                return;
            }
            const auto record = *exception + 8;
            const auto code = At<std::uint32_t>(d, record), parameters = At<std::uint32_t>(d, record + 24);
            const auto address = At<std::uint64_t>(d, record + 16);
            std::string params;
            for (std::uint32_t i = 0; i < std::min(parameters, 15u); i++)
                params += std::format("{}0x{:x}", i ? " " : "", At<std::uint64_t>(d, record + 32 + i * 8ull));
            Print(std::format("  thread {} code 0x{:X} at 0x{:X} = {}; params [{}]", At<std::uint32_t>(d, *exception), code, address,
                              dump.Where(address).value_or("?"), params));
            const auto context = At<std::uint32_t>(d, record + 152 + 4);
            const auto reg = [&](const std::uint32_t offset) { return At<std::uint32_t>(d, context + offset); };
            const auto eip = reg(0xB8), esp = reg(0xC4);
            Print(std::format("  eip 0x{:X} ({}) esp 0x{:X} ebp 0x{:X}", eip, dump.Where(eip).value_or("?"), esp, reg(0xB4)));
            Print(std::format("  eax 0x{:X} ebx 0x{:X} ecx 0x{:X} edx 0x{:X} esi 0x{:X} edi 0x{:X}", reg(0xB0), reg(0xA4), reg(0xAC), reg(0xA8), reg(0xA0),
                              reg(0x9C)));
            const auto stack = dump.Read(esp, 0x800);
            if (!stack)
            {
                Print("  stack memory not in dump");
                return;
            }
            for (std::uint64_t i = 0; i + 4 <= stack->size(); i += 4)
            {
                const auto value = At<std::uint32_t>(*stack, i);
                if (const auto where = dump.Where(value); where && !SystemModule(*where))
                    Print(std::format("  [esp+0x{:X}] 0x{:X} {}", i, value, *where));
            }
        }

        // ---- ipak ---------------------------------------------------------

        // one entry walked as t6zm's streamer reads it: fixed 0x40000 windows
        // from the entry's first 0x8000-aligned chunk; empty when it reads fine
        std::string CheckEntry(const std::string& f, const std::uint64_t start, const std::uint64_t size)
        {
            constexpr std::uint64_t WINDOW = 0x40000;
            auto window = start & ~0x7fffull;
            const auto span = ((start + size + 0x7fff) & ~0x7fffull) - window;
            auto offset = start - window;
            auto remaining = offset + size;
            std::uint64_t expect = 0;
            for (std::uint64_t w = 0; w < (span + WINDOW - 1) / WINDOW; w++)
            {
                std::uint64_t limit;
                if (w)
                {
                    remaining -= WINDOW;
                    window += WINDOW;
                    limit = std::min(WINDOW, remaining);
                    offset = 0;
                }
                else
                    limit = std::min(WINDOW - offset, size);
                for (std::uint64_t block = 0;;)
                {
                    const auto header = window + offset + block;
                    const auto control = At<std::uint32_t>(f, header);
                    const auto count = control >> 24;
                    std::uint64_t end = block + 128;
                    bool positioned = false;
                    for (std::uint32_t c = 0; c < count && c < 31; c++)
                    {
                        const auto command = At<std::uint32_t>(f, header + 4 + c * 4ull);
                        end += command & 0xffffff;
                        positioned |= (command >> 24) == 0 || (command >> 24) == 1;
                    }
                    if (offset + end > WINDOW)
                        return std::format("block @{:x} overruns window by {}", header, offset + end - WINDOW);
                    if (count && positioned && (control & 0xffffff) < expect)
                        return std::format("block @{:x} offset {:x} < {:x}", header, control & 0xffffff, expect);
                    const auto next = (end + 127) & ~127ull;
                    if (next >= limit)
                        break;
                    block = next;
                }
            }
            return {};
        }
    } // namespace

    int KnowledgeReport(const Arguments& args)
    {
        namespace k = pipeline::knowledge;
        std::vector<fs::path> ledgers, bundles, reports;
        std::optional<fs::path> write;
        bool record = false;
        std::size_t top = 40;
        for (std::size_t i = 0; i < args.size(); i++)
        {
            const auto value = [&]
            {
                if (i + 1 >= args.size())
                    throw std::invalid_argument(args[i] + " needs a value");
                return args[++i];
            };
            if (args[i] == "--ledger")
                ledgers.push_back(files::FromUtf8(value()));
            else if (args[i] == "--bundle")
                bundles.push_back(files::FromUtf8(value()));
            else if (args[i] == "--report")
                reports.push_back(files::FromUtf8(value()));
            else if (args[i] == "--write")
                write = files::FromUtf8(value());
            else if (args[i] == "--top")
                top = std::stoul(value());
            else if (args[i] == "--record")
                record = true;
            else
                throw std::invalid_argument("unknown argument " + args[i]);
        }
        if (ledgers.empty())
            ledgers.push_back(files::FromUtf8(launcher::Settings().work) / "knowledge");
        std::vector<util::Json> extra;
        for (const auto& bundle : bundles)
            if (auto obs = k::FromBundle(bundle))
                extra.push_back(std::move(*obs));
        for (const auto& path : reports)
        {
            const auto report = util::Json::parse(files::ReadText(path));
            auto project = report.value("project", std::string());
            extra.push_back(k::Observation(&report, project.empty() ? files::Utf8(path.parent_path().filename()) : project, project, "", "staged", std::nullopt, ""));
        }
        if (record)
            for (const auto& obs : extra)
                k::Record(ledgers.front(), obs);
        auto seen = k::Load(ledgers);
        for (auto& obs : extra)
        {
            const auto map = obs.at("map");
            if (const auto same = std::ranges::find_if(seen, [&](const util::Json& o) { return o.at("map") == map; }); same != seen.end())
                *same = std::move(obs);
            else
                seen.push_back(std::move(obs));
        }
        const auto summary = k::Aggregate(seen);
        Print(std::format("{} maps: {}", summary["maps"].get<std::size_t>(), summary["outcomes"].dump()));
        for (const auto& [step, maps] : summary["failed_steps"].items())
        {
            std::string names;
            for (const auto& m : maps)
                names += (names.empty() ? "" : ", ") + m.get<std::string>();
            Print("failed at " + step + ": " + names);
        }
        Print(std::format("\n{:>4} {:>5}  gap", "maps", "uses"));
        for (std::size_t i = 0; i < std::min(top, summary["gaps"].size()); i++)
        {
            const auto& gap = summary["gaps"][i];
            Print(std::format("{:>4} {:>5}  {:<14} {}", gap["maps"].size(), gap["uses"].get<long long>(), gap["kind"].get<std::string>(),
                              gap["symbol"].get<std::string>()));
        }
        if (!summary["stage_errors"].empty())
        {
            Print("\nstage errors:");
            std::size_t shown = 0;
            for (const auto& [kind, maps] : summary["stage_errors"].items())
                if (shown++ < top)
                    Print(std::format("{:>4}  {}", maps.size(), kind));
        }
        if (write)
            files::WriteText(*write, util::JsonDumps(summary, 1) + "\n");
        return 0;
    }

    int IpakCheck(const Arguments& args)
    {
        if (args.size() != 1)
            throw std::invalid_argument("usage: waw2bo2n ipak-check <file.ipak>");
        const auto f = ReadFile(files::FromUtf8(args[0]));
        std::map<std::uint32_t, std::array<std::uint32_t, 3>> sections; // type -> offset, size, count
        for (std::uint32_t i = 0, n = At<std::uint32_t>(f, 12); i < n; i++)
            sections[At<std::uint32_t>(f, 16 + 16ull * i)] = {At<std::uint32_t>(f, 20 + 16ull * i), At<std::uint32_t>(f, 24 + 16ull * i),
                                                              At<std::uint32_t>(f, 28 + 16ull * i)};
        if (!sections.contains(1) || !sections.contains(2))
            throw std::runtime_error("no index or data section");
        const auto [index, unused, count] = sections[1];
        const auto data = sections[2][0];
        std::size_t bad = 0;
        for (std::uint32_t i = 0; i < count; i++)
        {
            const auto entry = index + 16ull * i;
            const auto name = At<std::uint32_t>(f, entry + 4);
            if (const auto error = CheckEntry(f, data + std::uint64_t{At<std::uint32_t>(f, entry + 8)}, At<std::uint32_t>(f, entry + 12)); !error.empty())
            {
                bad++;
                Print(std::format("0x{:x} {}", name, error));
            }
        }
        Print(std::format("{} entries, {} bad", count, bad));
        return bad ? 1 : 0;
    }

    int CrashSummary(const Arguments& args)
    {
        if (args.empty())
            throw std::invalid_argument("usage: waw2bo2n crash-summary <dump.dmp>...");
        for (const auto& path : args)
            SummarizeDump(files::FromUtf8(path));
        return 0;
    }

    int GscCheck(const Arguments& args)
    {
        auto options = args;
        const auto dialect = std::erase(options, std::string("--bo2")) ? gsc::Dialect::Bo2 : gsc::Dialect::Waw;
        if (options.size() != 1 && (options.size() != 3 || options[1] != "--dump"))
            return 2;
        const auto root = files::FromUtf8(options[0]);
        std::vector<fs::path> scripts;
        if (fs::is_directory(root))
        {
            scripts = files::GlobSuffix(root, ".gsc");
            const auto client = files::GlobSuffix(root, ".csc");
            scripts.insert(scripts.end(), client.begin(), client.end());
            std::sort(scripts.begin(), scripts.end());
        }
        else
            scripts.push_back(root);
        std::string dump;
        int rejected = 0;
        gsc::port::VmCounts counts;
        for (const auto& path : scripts)
        {
            const auto name = files::Utf8(fs::is_directory(root) ? path.lexically_relative(root) : path);
            dump += "== " + name + "\n";
            try
            {
                const auto tokens = gsc::Tokenize(encoding::UniversalNewlines(encoding::Utf8SigReplace(ReadFile(path))));
                dump += gsc::DumpAst(gsc::ParseAst(tokens, dialect), tokens);
                // the translator's VM rules, and that their result still parses
                auto ported = tokens;
                counts.Add(gsc::port::ApplyVmRules(ported, {"runtime"}));
                gsc::ParseAst(gsc::Tokenize(gsc::Emit(ported)), dialect);
            }
            catch (const gsc::GscSyntaxError& error)
            {
                rejected++;
                dump += std::string("!! ") + error.what() + "\n";
                Print(name + ": " + error.what());
            }
        }
        Print(std::format("{} scripts, {} rejected; {} of {} loops may never end, {} switches, {} vectors, {} logic values, {} array and {} string comparisons",
                          scripts.size(), rejected, counts.watched, counts.loops, counts.switches, counts.vectors, counts.logic, counts.arrays,
                          counts.comparisons));
        if (options.size() == 3)
            files::WriteBytes(files::FromUtf8(options[2]), dump);
        return rejected ? 1 : 0;
    }

    int GscLink(const Arguments& args)
    {
        if (args.empty())
            return 2;
        const auto compat = extract::ResourceRoot() / "src/waw2bo2/compat";
        const auto api = gsc::port::T6Api::Load(compat / "t6_script_api.json", fs::path());
        const auto problems = gsc::port::LinkCheck(files::FromUtf8(args[0]), api, {args.begin() + 1, args.end()});
        for (const auto& problem : problems)
            Print(problem);
        Print(std::format("{} problems", problems.size()));
        return problems.empty() ? 0 : 1;
    }

    int GscRules(const Arguments& args)
    {
        if (args.size() != 3)
            return 2;
        auto tokens = gsc::Tokenize(encoding::UniversalNewlines(encoding::Utf8SigReplace(ReadFile(files::FromUtf8(args[0])))));
        const auto counts = gsc::port::ApplyVmRules(tokens, {args[2]});
        files::WriteBytes(files::FromUtf8(args[1]), gsc::Emit(tokens));
        Print(std::format("{} changes", counts.Changes()));
        return 0;
    }
} // namespace w2b::cli
