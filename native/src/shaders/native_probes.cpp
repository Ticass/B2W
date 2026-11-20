// Native T6 probe readers adapted to the original WaW RGBA
// cube images (see runtime.h).
#include "runtime.h"

#include "core/files.h"
#include "core/hash.h"
#include "core/profile.h"
#include "shaders/techsets.h"
#include "core/regex.h"
#include "d3d.h"
#include "pipeline/finish.h"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace w2b::shaders
{
    namespace fs = std::filesystem;
    using pipeline::StepError;

    namespace
    {
        std::string Str(fs::path p)
        {
            return files::Utf8(p.make_preferred());
        }

        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }

        std::string Bytes(const fs::path& p)
        {
            std::string data;
            if (!files::ReadBytes(p, data))
                throw StepError("FileNotFoundError", "[Errno 2] No such file or directory: '" + Str(p) + "'");
            return data;
        }

        util::Json Json(const fs::path& p)
        {
            return util::Json::parse(files::ReadText(p));
        }

        void MakeParent(const fs::path& p)
        {
            fs::create_directories(p.parent_path());
        }

        bool StartsWith(std::string_view s, std::string_view p)
        {
            return s.substr(0, p.size()) == p;
        }

        uint32_t Word(const std::string& d, const size_t at)
        {
            uint32_t v;
            std::memcpy(&v, d.data() + at, 4);
            return v;
        }

        void AppendWord(std::string& d, const uint32_t v)
        {
            d.append(reinterpret_cast<const char*>(&v), 4);
        }

        // DXBC's MD5 with its container-specific final block
        std::string Checksum(const std::string& data)
        {
            const auto size = data.size();
            const auto end = size / 64 * 64;
            std::string tail = data.substr(end) + '\x80';
            std::string blocks;
            if (tail.size() > 56)
            {
                tail.resize(64, '\0');
                blocks = data.substr(0, end) + tail;
                tail.clear();
            }
            else
                blocks = data.substr(0, end);
            const auto bits = static_cast<uint32_t>(size * 8);
            AppendWord(blocks, bits);
            tail.resize(56, '\0');
            blocks += tail;
            AppendWord(blocks, (bits >> 2) | 1);
            std::array<uint32_t, 4> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
            static const int shifts[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
            uint32_t constants[64];
            for (int i = 0; i < 64; i++)
                constants[i] = static_cast<uint32_t>(static_cast<unsigned long long>(std::fabs(std::sin(i + 1.0)) * 4294967296.0));
            for (size_t offset = 0; offset < blocks.size(); offset += 64)
            {
                uint32_t words[16];
                std::memcpy(words, blocks.data() + offset, 64);
                auto [a, b, c, d] = state;
                for (int i = 0; i < 64; i++)
                {
                    uint32_t f;
                    int g;
                    if (i < 16)
                        f = (b & c) | (~b & d), g = i;
                    else if (i < 32)
                        f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
                    else if (i < 48)
                        f = b ^ c ^ d, g = (3 * i + 5) % 16;
                    else
                        f = c ^ (b | ~d), g = (7 * i) % 16;
                    uint32_t value = a + f + constants[i] + words[g];
                    value = (value << shifts[i]) | (value >> (32 - shifts[i]));
                    a = d, d = c, c = b, b = b + value;
                }
                state[0] += a, state[1] += b, state[2] += c, state[3] += d;
            }
            std::string out;
            for (const auto v : state)
                AppendWord(out, v);
            return out;
        }

        std::vector<std::pair<std::string, std::string>> Chunks(const std::string& code)
        {
            if (code.substr(0, 4) != "DXBC" || code.size() < 32)
                throw StepError("ValueError", "native probe adapter requires DXBC");
            std::vector<std::pair<std::string, std::string>> result;
            const auto count = Word(code, 28);
            for (uint32_t i = 0; i < count; i++)
            {
                const auto offset = Word(code, 32 + 4 * i);
                const auto size = Word(code, offset + 4);
                result.emplace_back(code.substr(offset, 4), code.substr(offset + 8, size));
            }
            return result;
        }

        // insert a unit conversion after native, full-register cube samples:
        // three elementwise instructions, no new registers or constants
        std::string Adapt(const std::string& code, const uint32_t texture)
        {
            auto chunks = Chunks(code);
            int changed = 0;
            for (auto& [tag, body] : chunks)
            {
                if (tag != "SHDR" && tag != "SHEX")
                    continue;
                std::vector<uint32_t> words(body.size() / 4);
                std::memcpy(words.data(), body.data(), words.size() * 4);
                std::vector<uint32_t> output(words.begin(), words.begin() + 2);
                size_t i = 2;
                while (i < words.size())
                {
                    const auto token = words[i];
                    size_t length = (token >> 24) & 127;
                    if ((token & 2047) == 53)
                        length = words[i + 1]; // custom data
                    if (!length || i + length > words.size())
                        throw StepError("ValueError", "invalid DXBC instruction");
                    const std::vector<uint32_t> inst(words.begin() + static_cast<long long>(i), words.begin() + static_cast<long long>(i + length));
                    output.insert(output.end(), inst.begin(), inst.end());
                    // SAMPLE, SAMPLE_L, SAMPLE_D and SAMPLE_B; skip opcode extensions
                    const auto op = token & 2047;
                    if (op == 69 || op == 72 || op == 73 || op == 74)
                    {
                        size_t at = 1;
                        auto extended = token;
                        while (extended & 0x80000000u)
                            extended = inst[at], at++;
                        if (inst.size() > at + 5 && ((inst[at + 4] >> 12) & 255) == 7 && inst[at + 5] == texture)
                        {
                            if (inst[at] != 0x001000f2 || ((inst[at + 2] >> 20) & 3) != 1)
                                throw StepError("ValueError", "unsupported native cube sample operands");
                            const auto reg = inst[at + 1];
                            // sample.rgb = (sample.rgb * sample.a)^2; sample.a = 1
                            output.insert(output.end(), {0x07000038, 0x00100072, reg, 0x00100e46, reg, 0x00100ff6, reg});
                            output.insert(output.end(), {0x07000038, 0x00100072, reg, 0x00100e46, reg, 0x00100e46, reg});
                            output.insert(output.end(), {0x05000036, 0x00100082, reg, 0x00004001, 0x3f800000});
                            changed++;
                        }
                    }
                    i += length;
                }
                output[1] = static_cast<uint32_t>(output.size());
                body.assign(reinterpret_cast<const char*>(output.data()), output.size() * 4);
            }
            if (!changed)
                throw StepError("ValueError", "native cube sample not found");
            const auto offset = static_cast<uint32_t>(32 + 4 * chunks.size());
            std::vector<uint32_t> offsets;
            std::string payload;
            for (const auto& [tag, body] : chunks)
            {
                offsets.push_back(offset + static_cast<uint32_t>(payload.size()));
                payload += tag;
                AppendWord(payload, static_cast<uint32_t>(body.size()));
                payload += body;
            }
            std::string result = "DXBC" + std::string(16, '\0');
            AppendWord(result, 1);
            AppendWord(result, offset + static_cast<uint32_t>(payload.size()));
            AppendWord(result, static_cast<uint32_t>(chunks.size()));
            for (const auto o : offsets)
                AppendWord(result, o);
            result += payload;
            const auto sum = Checksum(result.substr(20));
            result.replace(4, 16, sum);
            return result;
        }

        // shaderruntime.resource_names: texture slot -> name
        std::map<long long, std::string> ResourceNames(const std::string& assembly)
        {
            static const regex::Regex resource("^//\\s+(\\w+)\\s+texture\\s+\\w+\\s+(\\w+)\\s+t?(\\d+)\\s+\\d+", regex::MULTILINE);
            std::map<long long, std::string> out;
            for (const auto& m : resource.FindAll(assembly))
                out[std::stoll(m[2])] = m[0];
            return out;
        }

        std::optional<fs::path> FirstFile(std::initializer_list<fs::path> candidates)
        {
            for (const auto& c : candidates)
                if (IsFile(c))
                    return c;
            return std::nullopt;
        }

        // what the caches keep across materials: parsed technique
        // sets, probe slots per shader file and adapted programs
        struct ProbeCache
        {
            std::map<std::string, T6TechniqueSet> techniques;
            std::map<std::string, std::pair<std::string, std::vector<long long>>> shaders;
            std::map<std::string, std::string> adapted;
        };

        // a file the binding output holds, else the stock one: (cache key, bytes)
        std::optional<std::pair<std::string, std::string>> OutputOrStock(const Output& output, const fs::path& stock, const fs::path& relative)
        {
            if (auto held = output.Read(relative))
                return std::pair{"output:" + hash::Sha256Hex(*held), std::move(*held)};
            const auto path = stock / relative;
            if (!IsFile(path))
                return std::nullopt;
            return std::pair{Str(path), std::string()};
        }

        int BindProbesWith(materials::T6Material& material, Output& output, const fs::path& stock, ProbeCache& cache)
        {
            const auto name = material.techniqueSet;
            const auto file = OutputOrStock(output, stock, fs::path("techniquesets") / files::FromUtf8(name + ".json"));
            if (!file)
                return 0;
            const auto& fileKey = file->first;
            if (!cache.techniques.count(fileKey))
            {
                const profile::Scope scope("probes: read technique set");
                cache.techniques[fileKey] = file->second.empty() ? ReadT6TechniqueSet(stock / "techniquesets" / files::FromUtf8(name + ".json")) : T6TechniqueSetFromJson(util::Json::parse(file->second));
            }
            auto data = cache.techniques[fileKey];
            int count = 0;
            for (auto& slot : data.techniques)
            {
                if (!slot)
                    continue;
                for (auto& p : slot.technique->passes)
                {
                    const auto shader = p.pixelShader;
                    if (StartsWith(shader, "waw/runtime_") || StartsWith(shader, "waw/native_probe_"))
                        continue;
                    const auto program = fs::path("shader_bin") / files::FromUtf8("ps_" + shader + ".cso");
                    const auto held = OutputOrStock(output, stock, program);
                    if (!held)
                        continue;
                    const auto& shaderKey = held->first;
                    if (!cache.shaders.count(shaderKey))
                    {
                        auto bytes = held->second.empty() ? Bytes(stock / program) : held->second;
                        std::vector<long long> found;
                        std::string assembly;
                        {
                            const profile::Scope scope("probes: disassemble");
                            assembly = shaders::Disassemble(bytes);
                        }
                        const profile::Scope scope("probes: resource names");
                        for (const auto& [slot, n] : ResourceNames(assembly))
                            if (n == "reflectionProbeSampler")
                                found.push_back(slot);
                        cache.shaders[shaderKey] = {std::move(bytes), std::move(found)};
                    }
                    const auto& [bytes, slots] = cache.shaders[shaderKey];
                    if (slots.empty())
                        continue;
                    if (slots.size() != 1)
                        throw StepError("ValueError", "ambiguous native reflection probe resource");
                    const auto adaptKey = shaderKey + "|" + std::to_string(slots[0]);
                    if (!cache.adapted.count(adaptKey))
                        cache.adapted[adaptKey] = Adapt(bytes, static_cast<uint32_t>(slots[0]));
                    const auto& code = cache.adapted[adaptKey];
                    const auto adapted = "waw/native_probe_" + hash::Sha256Hex(code).substr(0, 20);
                    output.Write(fs::path("shader_bin") / files::FromUtf8("ps_" + adapted + ".cso"), code, false);
                    p.pixelShader = adapted;
                    count++;
                }
            }
            if (count)
            {
                const auto identity = "waw/runtime_" + hash::Sha256Hex(util::JsonDumpsSorted(ToJson(data))).substr(0, 16);
                data.name = identity;
                size_t i = 0;
                for (auto& slot : data.techniques)
                {
                    if (slot)
                        slot.technique->name = identity + "_" + std::to_string(i);
                    i++;
                }
                output.Write(fs::path("techniquesets") / files::FromUtf8(identity + ".json"), util::JsonDumpsDefault(ToJson(data)), true);
                material.techniqueSet = identity;
            }
            return count;
        }

    } // namespace

    int BindNativeProbes(materials::T6Material& material, Output& output, const fs::path& stock)
    {
        static std::mutex lock;
        static ProbeCache cache;
        const std::lock_guard guard(lock);
        return BindProbesWith(material, output, stock, cache);
    }
} // namespace w2b::shaders
