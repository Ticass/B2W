#pragma once
// Read-only zip archives (WaW .iwd), as Python's zipfile reads them.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace w2b::zip
{
    struct Entry
    {
        std::string name; // ZipInfo.filename, UTF-8
        uint64_t localOffset = 0;
        uint64_t compressedSize = 0;
        uint64_t size = 0;
        uint32_t crc = 0;
        uint16_t method = 0;
        bool IsDir() const
        {
            return !name.empty() && name.back() == '/';
        }
    };

    class Archive
    {
    public:
        explicit Archive(const std::filesystem::path& path);
        ~Archive();
        Archive(const Archive&) = delete;
        Archive& operator=(const Archive&) = delete;

        const std::vector<Entry>& Entries() const
        {
            return m_entries;
        }

        // ZipFile.read(name): stored or deflated, CRC checked
        std::string Read(const Entry& entry);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        std::vector<Entry> m_entries;
    };
} // namespace w2b::zip
