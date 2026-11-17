#include "zip.h"

#include "encoding.h"
#include "files.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace w2b::zip
{
    namespace fs = std::filesystem;

    struct Archive::Impl
    {
        std::ifstream stream;
        std::string path;
        std::mutex lock;
    };

    namespace
    {
        template<typename T> T Get(const std::string& data, const size_t at)
        {
            if (at + sizeof(T) > data.size())
                throw std::runtime_error("truncated zip structure");
            T value{};
            std::memcpy(&value, data.data() + at, sizeof(T));
            return value;
        }
    } // namespace

    Archive::Archive(const fs::path& path)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->path = files::Utf8(path);
        m_impl->stream.open(path, std::ios::binary);
        if (!m_impl->stream)
            throw std::runtime_error("cannot open " + m_impl->path);
        m_impl->stream.seekg(0, std::ios::end);
        const auto size = static_cast<uint64_t>(m_impl->stream.tellg());
        // end of central directory: within the last 64 KiB + 22 bytes
        const uint64_t tail = std::min<uint64_t>(size, 65557);
        std::string end(static_cast<size_t>(tail), '\0');
        m_impl->stream.seekg(static_cast<std::streamoff>(size - tail));
        m_impl->stream.read(end.data(), static_cast<std::streamsize>(tail));
        const auto at = end.rfind(std::string("PK\x05\x06", 4));
        if (at == std::string::npos)
            throw std::runtime_error("File is not a zip file: " + m_impl->path);
        uint64_t count = Get<uint16_t>(end, at + 10);
        uint64_t directorySize = Get<uint32_t>(end, at + 12);
        uint64_t directoryOffset = Get<uint32_t>(end, at + 16);
        // ZIP64 end of central directory locator just before
        if (at >= 20 && end.compare(at - 20, 4, std::string("PK\x06\x07", 4)) == 0)
        {
            const auto record = Get<uint64_t>(end, at - 20 + 8);
            std::string z64(56, '\0');
            m_impl->stream.seekg(static_cast<std::streamoff>(record));
            m_impl->stream.read(z64.data(), 56);
            count = Get<uint64_t>(z64, 32);
            directorySize = Get<uint64_t>(z64, 40);
            directoryOffset = Get<uint64_t>(z64, 48);
        }
        std::string directory(static_cast<size_t>(directorySize), '\0');
        m_impl->stream.seekg(static_cast<std::streamoff>(directoryOffset));
        m_impl->stream.read(directory.data(), static_cast<std::streamsize>(directorySize));
        size_t pos = 0;
        for (uint64_t i = 0; i < count; i++)
        {
            if (Get<uint32_t>(directory, pos) != 0x02014b50)
                throw std::runtime_error("Bad magic number for central directory: " + m_impl->path);
            Entry entry;
            const auto flags = Get<uint16_t>(directory, pos + 8);
            entry.method = Get<uint16_t>(directory, pos + 10);
            entry.crc = Get<uint32_t>(directory, pos + 16);
            entry.compressedSize = Get<uint32_t>(directory, pos + 20);
            entry.size = Get<uint32_t>(directory, pos + 24);
            const auto nameLength = Get<uint16_t>(directory, pos + 28);
            const auto extraLength = Get<uint16_t>(directory, pos + 30);
            const auto commentLength = Get<uint16_t>(directory, pos + 32);
            entry.localOffset = Get<uint32_t>(directory, pos + 42);
            const auto rawName = directory.substr(pos + 46, nameLength);
            entry.name = (flags & 0x800) ? rawName : encoding::Cp437ToUtf8(rawName);
            // zipfile turns os.sep into "/" on Windows
            for (auto& c : entry.name)
                if (c == '\\')
                    c = '/';
            // ZIP64 extra field for 0xFFFFFFFF sizes/offset
            size_t extra = pos + 46 + nameLength;
            const size_t extraEnd = extra + extraLength;
            while (extra + 4 <= extraEnd)
            {
                const auto id = Get<uint16_t>(directory, extra);
                const auto length = Get<uint16_t>(directory, extra + 2);
                if (id == 1)
                {
                    size_t field = extra + 4;
                    if (entry.size == 0xFFFFFFFF)
                        entry.size = Get<uint64_t>(directory, (field += 8) - 8);
                    if (entry.compressedSize == 0xFFFFFFFF)
                        entry.compressedSize = Get<uint64_t>(directory, (field += 8) - 8);
                    if (entry.localOffset == 0xFFFFFFFF)
                        entry.localOffset = Get<uint64_t>(directory, (field += 8) - 8);
                }
                extra += 4 + length;
            }
            m_entries.push_back(std::move(entry));
            pos += 46 + nameLength + extraLength + commentLength;
        }
    }

    Archive::~Archive() = default;

    std::string Archive::Read(const Entry& entry)
    {
        std::string header(30, '\0');
        std::string data;
        {
            const std::lock_guard guard(m_impl->lock);
            m_impl->stream.clear();
            m_impl->stream.seekg(static_cast<std::streamoff>(entry.localOffset));
            m_impl->stream.read(header.data(), 30);
            if (Get<uint32_t>(header, 0) != 0x04034b50)
                throw std::runtime_error("Bad magic number for file header: " + m_impl->path);
            const auto skip = static_cast<uint64_t>(Get<uint16_t>(header, 26)) + Get<uint16_t>(header, 28);
            data.resize(static_cast<size_t>(entry.compressedSize));
            m_impl->stream.seekg(static_cast<std::streamoff>(entry.localOffset + 30 + skip));
            m_impl->stream.read(data.data(), static_cast<std::streamsize>(entry.compressedSize));
        }
        std::string out;
        if (entry.method == 0)
            out = std::move(data);
        else if (entry.method == 8)
        {
            out.resize(static_cast<size_t>(entry.size));
            z_stream z{};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
                throw std::runtime_error("inflateInit failed");
            z.next_in = reinterpret_cast<Bytef*>(data.data());
            z.avail_in = static_cast<uInt>(data.size());
            z.next_out = reinterpret_cast<Bytef*>(out.data());
            z.avail_out = static_cast<uInt>(out.size());
            const int result = inflate(&z, Z_FINISH);
            inflateEnd(&z);
            if (result != Z_STREAM_END && !(result == Z_BUF_ERROR && z.avail_out == 0))
                throw std::runtime_error("Error -3 while decompressing data: " + entry.name);
        }
        else
            throw std::runtime_error("That compression method is not supported: " + entry.name);
        if (crc32(0, reinterpret_cast<const Bytef*>(out.data()), static_cast<uInt>(out.size())) != entry.crc)
            throw std::runtime_error("Bad CRC-32 for file '" + entry.name + "'");
        return out;
    }
} // namespace w2b::zip
