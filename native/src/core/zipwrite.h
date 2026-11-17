#pragma once
// A ZIP writer (deflated entries, as zipfile.ZIP_DEFLATED writes them) for
// small archives such as the launcher's diagnostics bundle. No ZIP64.

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace w2b::zip
{
    class Writer
    {
    public:
        explicit Writer(const std::filesystem::path& path);
        ~Writer();
        // ZipFile.writestr(name, data)
        void Add(const std::string& name, std::string_view data);
        void Close();

    private:
        struct Central
        {
            std::string name;
            unsigned crc = 0, compressed = 0, size = 0, offset = 0;
            unsigned short time = 0, date = 0;
        };
        std::ofstream out;
        std::vector<Central> entries;
        unsigned offset = 0;
        bool closed = false;
    };
} // namespace w2b::zip
