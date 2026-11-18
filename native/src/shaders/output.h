#pragma once
// Where shader binding writes the programs, technique sets and proofs it
// produces, and reads them back. A folder output writes them to disk as it
// goes; a memory output keeps them until the caller writes the files it
// keeps, so per-material work never round-trips through scratch folders.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace w2b::shaders
{
    class Output
    {
    public:
        virtual ~Output() = default;
        // writes `data` at `relative` (text gets Windows line endings, as WriteText)
        virtual void Write(const std::filesystem::path& relative, std::string_view data, bool text) = 0;
        // the bytes this output holds at `relative`
        virtual std::optional<std::string> Read(const std::filesystem::path& relative) const = 0;
    };

    class FolderOutput final : public Output
    {
    public:
        explicit FolderOutput(std::filesystem::path root) : root_(std::move(root)) {}
        void Write(const std::filesystem::path& relative, std::string_view data, bool text) override;
        std::optional<std::string> Read(const std::filesystem::path& relative) const override;
        const std::filesystem::path& Root() const
        {
            return root_;
        }

    private:
        std::filesystem::path root_;
    };

    class MemoryOutput final : public Output
    {
    public:
        void Write(const std::filesystem::path& relative, std::string_view data, bool text) override;
        std::optional<std::string> Read(const std::filesystem::path& relative) const override;
        // (posix relative path, bytes), in the case-insensitive path order a
        // folder listing sorts them
        std::vector<std::pair<std::string, std::string>> Files() const;

    private:
        std::map<std::string, std::pair<std::string, std::string>> files_; // lower-case key -> (path, bytes)
    };
} // namespace w2b::shaders
