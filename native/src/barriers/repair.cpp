#include "barriers/xanim.h"

#include "core/files.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>

namespace w2b::barriers
{
    namespace
    {
        constexpr std::uint16_t RepairFps = 30;
        using Quaternion = std::array<double, 4>;
        using PackedQuaternion = std::array<std::int16_t, 3>;

        struct Repair
        {
            XAnimHeader header;
            std::array<Vec3, 2> root;
            std::uint8_t flip;
            std::string_view bone;
            std::array<Quaternion, 2> rotation;
            std::array<Vec3, 2> offset;
            std::string_view tail;
        };

        struct Timing
        {
            double lift, turn, hold, push;
            explicit Timing(const Json& source)
                : lift(source.at("lift").get<double>()), turn(source.at("turn").get<double>()),
                  hold(source.at("hold").get<double>()), push(source.at("push").get<double>()) {}
            double Settle() const { return std::max(lift, turn) + hold; }
            double Duration() const { return Settle() + push; }
        };

        void SkipIndices(Reader& input, std::size_t count, std::uint16_t frames)
        {
            if (count < frames)
                input.Take(count * (frames <= 256 ? 1 : 2));
        }

        std::array<Vec3, 2> ReadTranslation(Reader& input, std::uint16_t frames)
        {
            const auto count = input.Read<std::uint16_t>();
            if (count == 0)
                return {};
            if (count == 1)
            {
                const auto value = ReadVector(input);
                return {value, value};
            }
            SkipIndices(input, count, frames);
            const TransQuantization track{input.Read<std::uint8_t>() != 0, ReadVector(input), ReadVector(input)};
            const auto first = track.Key(input);
            input.Take(static_cast<std::size_t>(count - 2) * 3 * (track.small ? 1 : 2));
            return {first, track.Key(input)};
        }

        Quaternion Decode(const PackedQuaternion& encoded)
        {
            const auto x = encoded[0] / 32767.0, y = encoded[1] / 32767.0, z = encoded[2] / 32767.0;
            return {x, y, z, std::sqrt(std::max(0.0, 1 - x * x - y * y - z * z))};
        }

        std::array<Quaternion, 2> ReadRotation(Reader& input, std::uint16_t frames)
        {
            const auto count = input.Read<std::uint16_t>();
            if (!count)
                throw Error("board animation: the board bone has no rotation track");
            if (count > 1)
                SkipIndices(input, count, frames);
            const auto first = Decode(input.Read<PackedQuaternion>());
            if (count == 1)
                return {first, first};
            input.Take(static_cast<std::size_t>(count - 2) * sizeof(PackedQuaternion));
            return {first, Decode(input.Read<PackedQuaternion>())};
        }

        Repair ReadRepair(std::string_view raw)
        {
            Reader input(raw);
            const auto header = ReadHeader(input);
            if (header.bones != 1 || !(header.flags & 2) || (header.flags & 1))
                throw Error("board animation: expected one bone, root motion, not looping");
            if (input.Read<std::uint16_t>() != 0)
                throw Error("board animation: root rotation is not supported");
            const auto root = ReadTranslation(input, header.frames);
            const auto flip = input.Read<std::uint8_t>(), half = input.Read<std::uint8_t>();
            const auto end = input.Remaining().find('\0');
            if (end == std::string_view::npos)
                throw Error("board animation: missing bone name");
            const auto bone = input.Take(end + 1).substr(0, end);
            if (half & 1)
                throw Error("board animation: simple (z-only) bone rotation is not supported");
            const auto rotation = ReadRotation(input, header.frames);
            const auto offset = ReadTranslation(input, header.frames);
            return {header, root, flip, bone, rotation, offset, input.Remaining()};
        }

        template<std::size_t Size>
        std::array<double, Size> Lerp(const std::array<double, Size>& a, const std::array<double, Size>& b, double fraction)
        {
            std::array<double, Size> result{};
            for (std::size_t i = 0; i < Size; ++i)
                result[i] = a[i] + (b[i] - a[i]) * fraction;
            return result;
        }

        Quaternion Slerp(const Quaternion& a, Quaternion b, double fraction)
        {
            Quaternion products{};
            for (std::size_t i = 0; i < products.size(); ++i)
                products[i] = a[i] * b[i];
            auto dot = Sum(products);
            if (dot < 0)
            {
                for (auto& value : b)
                    value = -value;
                dot = -dot;
            }
            auto result = Quaternion{};
            if (dot > 0.9995)
                result = Lerp(a, b, fraction);
            else
            {
                const auto theta = std::acos(dot);
                const auto s0 = std::sin((1 - fraction) * theta) / std::sin(theta);
                const auto s1 = std::sin(fraction * theta) / std::sin(theta);
                for (std::size_t i = 0; i < result.size(); ++i)
                    result[i] = s0 * a[i] + s1 * b[i];
            }
            for (std::size_t i = 0; i < products.size(); ++i)
                products[i] = result[i] * result[i];
            const auto length = std::sqrt(Sum(products));
            for (auto& value : result)
                value /= length;
            if (result[3] < 0)
                for (auto& value : result)
                    value = -value;
            return result;
        }

        template<typename T>
        void Append(std::string& out, const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            out.append(reinterpret_cast<const char*>(&value), sizeof(value));
        }

        template<typename T>
        T Quantize(double value)
        {
            if (!std::isfinite(value))
                throw Error("invalid barricade animation key");
            const auto floor = std::floor(value);
            const auto fraction = value - floor;
            const auto rounded = floor + (fraction > 0.5 || (fraction == 0.5 && std::fmod(floor, 2) != 0));
            if (rounded < std::numeric_limits<T>::lowest() || rounded > std::numeric_limits<T>::max())
                throw Error("barricade animation key exceeds its storage range");
            return static_cast<T>(rounded);
        }

        void WriteTranslation(std::string& out, const std::vector<Vec3>& values)
        {
            Vec3 mins = values.front(), maxs = values.front(), ranges{};
            for (const auto& key : values)
                for (std::size_t i = 0; i < mins.size(); ++i)
                {
                    mins[i] = std::min(mins[i], key[i]);
                    maxs[i] = std::max(maxs[i], key[i]);
                }
            for (std::size_t i = 0; i < mins.size(); ++i)
                ranges[i] = maxs[i] - mins[i];
            Append(out, static_cast<std::uint16_t>(values.size()));
            out += '\0'; // unsigned 16-bit components
            for (const auto value : mins)
                Append(out, static_cast<float>(value));
            for (const auto range : ranges)
                Append(out, static_cast<float>(range));
            for (const auto& key : values)
                for (std::size_t i = 0; i < mins.size(); ++i)
                    Append(out, ranges[i] != 0 ? Quantize<std::uint16_t>((key[i] - mins[i]) / ranges[i] * 65535) : std::uint16_t{});
        }

        double Fraction(double elapsed, double duration)
        {
            return duration > 0 ? std::min(1.0, elapsed / duration) : 1.0;
        }

        double RoundedSeconds(double duration)
        {
            char text[64];
            const auto [end, error] = std::to_chars(text, text + sizeof(text), duration, std::chars_format::fixed, 3);
            double result{};
            if (error != std::errc{} || !py::ParseFloat(std::string_view(text, end), result))
                throw Error("invalid barricade repair duration");
            return result;
        }
    }

    std::string RetimeRepair(std::string_view raw, const Json& timing)
    {
        const auto stock = ReadRepair(raw);
        const auto [start, rest] = stock.root;
        const auto [q0, q1] = stock.rotation;
        const auto [o0, o1] = stock.offset;
        const Timing durations(timing);
        const auto total = durations.Duration();
        const auto settle = durations.Settle();
        if (!std::isfinite(total))
            throw Error("invalid barricade repair duration");
        const auto frameCount = total <= 0 ? 2 : std::max<unsigned>(2, Quantize<std::uint16_t>(total * RepairFps) + 1u);
        if (frameCount > std::numeric_limits<std::uint16_t>::max())
            throw Error("barricade repair exceeds the animation frame limit");
        const auto frames = static_cast<std::uint16_t>(frameCount);
        std::vector<Vec3> roots, offsets;
        std::vector<PackedQuaternion> rotations;
        roots.reserve(frames);
        offsets.reserve(frames);
        rotations.reserve(frames);
        for (unsigned i = 0; i < frames; ++i)
        {
            const auto elapsed = i + 1 < frames ? static_cast<double>(i) / RepairFps : total;
            const auto turn = Fraction(elapsed, durations.turn);
            if (elapsed <= settle)
                roots.push_back({start[0], start[1], start[2] + (rest[2] - start[2]) * Fraction(elapsed, durations.lift)});
            else
                roots.push_back(Lerp(Vec3{start[0], start[1], rest[2]}, rest, Fraction(elapsed - settle, durations.push)));
            const auto q = Slerp(q0, q1, turn);
            rotations.push_back({Quantize<std::int16_t>(q[0] * 32767), Quantize<std::int16_t>(q[1] * 32767),
                                 Quantize<std::int16_t>(q[2] * 32767)});
            offsets.push_back(Lerp(o0, o1, turn));
        }
        std::string out;
        Append(out, XAnimHeader{stock.header.version, frames, 1, 2, stock.header.kind, RepairFps});
        Append(out, std::uint16_t{}); // empty root rotation
        WriteTranslation(out, roots);
        Append(out, stock.flip);
        out += '\0';
        out += stock.bone;
        out += '\0';
        Append(out, frames);
        for (const auto& rotation : rotations)
            Append(out, rotation);
        WriteTranslation(out, offsets);
        out += stock.tail;
        return out;
    }

    Json StageRepairs(std::string_view asset, const Path& bo2, const Json& timing, const Path& destination)
    {
        const auto fields = py::Split(asset, "\\");
        std::map<std::string_view, std::string_view, std::less<>> pairs;
        for (std::size_t i = 1; i + 1 < fields.size(); i += 2)
            pairs[fields[i]] = fields[i + 1];
        auto written = Json::object();
        for (int i = 1; i < 64; ++i)
        {
            const auto anim = pairs.find("boardAnim" + std::to_string(i));
            if (anim == pairs.end() || anim->second.empty())
                continue;
            const auto name = files::FromUtf8(anim->second);
            files::WriteBytes(destination / "xanim" / name, RetimeRepair(ReadBytes(bo2 / "raw/xanim" / name), timing));
            written[anim->second] = RoundedSeconds(Timing(timing).Duration());
        }
        return written;
    }
}
