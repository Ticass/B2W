// ZIP writer (see zipwrite.h).
#include "zipwrite.h"

#include <zlib.h>

#include <ctime>
#include <stdexcept>

namespace w2b::zip
{
    namespace
    {
        void Put16(std::string& b, const unsigned v)
        {
            b += static_cast<char>(v & 0xFF);
            b += static_cast<char>(v >> 8 & 0xFF);
        }

        void Put32(std::string& b, const unsigned v)
        {
            Put16(b, v & 0xFFFF);
            Put16(b, v >> 16);
        }

        std::string Deflate(const std::string_view data)
        {
            z_stream stream{};
            if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                throw std::runtime_error("deflate init failed");
            std::string out(deflateBound(&stream, static_cast<uLong>(data.size())), '\0');
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
            stream.avail_in = static_cast<uInt>(data.size());
            stream.next_out = reinterpret_cast<Bytef*>(out.data());
            stream.avail_out = static_cast<uInt>(out.size());
            const int result = deflate(&stream, Z_FINISH);
            out.resize(stream.total_out);
            deflateEnd(&stream);
            if (result != Z_STREAM_END)
                throw std::runtime_error("deflate failed");
            return out;
        }
    } // namespace

    Writer::Writer(const std::filesystem::path& path) : out(path, std::ios::binary | std::ios::trunc)
    {
        if (!out)
            throw std::runtime_error("cannot write the archive");
    }

    Writer::~Writer()
    {
        try
        {
            Close();
        }
        catch (...)
        {
        }
    }

    void Writer::Add(const std::string& name, const std::string_view data)
    {
        const auto packed = Deflate(data);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        Central entry{name,
                      static_cast<unsigned>(crc32(0, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()))),
                      static_cast<unsigned>(packed.size()),
                      static_cast<unsigned>(data.size()),
                      offset,
                      static_cast<unsigned short>(local.tm_hour << 11 | local.tm_min << 5 | local.tm_sec / 2),
                      static_cast<unsigned short>((local.tm_year - 80) << 9 | (local.tm_mon + 1) << 5 | local.tm_mday)};
        std::string header;
        Put32(header, 0x04034b50);
        Put16(header, 20);
        Put16(header, 0x800); // UTF-8 names
        Put16(header, 8);
        Put16(header, entry.time);
        Put16(header, entry.date);
        Put32(header, entry.crc);
        Put32(header, entry.compressed);
        Put32(header, entry.size);
        Put16(header, static_cast<unsigned>(name.size()));
        Put16(header, 0);
        header += name;
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.write(packed.data(), static_cast<std::streamsize>(packed.size()));
        offset += static_cast<unsigned>(header.size() + packed.size());
        entries.push_back(entry);
    }

    void Writer::Close()
    {
        if (closed)
            return;
        closed = true;
        std::string directory;
        for (const auto& e : entries)
        {
            Put32(directory, 0x02014b50);
            Put16(directory, 20);
            Put16(directory, 20);
            Put16(directory, 0x800);
            Put16(directory, 8);
            Put16(directory, e.time);
            Put16(directory, e.date);
            Put32(directory, e.crc);
            Put32(directory, e.compressed);
            Put32(directory, e.size);
            Put16(directory, static_cast<unsigned>(e.name.size()));
            Put16(directory, 0);
            Put16(directory, 0);
            Put16(directory, 0);
            Put16(directory, 0);
            Put32(directory, 0);
            Put32(directory, e.offset);
            directory += e.name;
        }
        std::string end;
        Put32(end, 0x06054b50);
        Put16(end, 0);
        Put16(end, 0);
        Put16(end, static_cast<unsigned>(entries.size()));
        Put16(end, static_cast<unsigned>(entries.size()));
        Put32(end, static_cast<unsigned>(directory.size()));
        Put32(end, offset);
        Put16(end, 0);
        out.write(directory.data(), static_cast<std::streamsize>(directory.size()));
        out.write(end.data(), static_cast<std::streamsize>(end.size()));
        out.close();
    }
} // namespace w2b::zip
