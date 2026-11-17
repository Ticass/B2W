#pragma once
// Native dialogs and shell actions for the launcher.

#include <windows.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace w2b::ui::shell
{
    using Filters = std::vector<std::pair<std::wstring, std::wstring>>; // label, "*.ff;*.png"
    std::optional<std::string> OpenFile(HWND owner, const std::wstring& title, const Filters& filters, const std::string& initialFolder = "");
    std::optional<std::string> SaveFile(HWND owner, const std::wstring& title, const Filters& filters, const std::wstring& fileName,
                                        const std::wstring& extension);
    std::optional<std::string> ChooseFolder(HWND owner, const std::wstring& title, const std::string& initialFolder = "");
    void Info(HWND owner, const std::string& title, const std::string& text);
    void Error(HWND owner, const std::string& title, const std::string& text);
    bool AskYesNo(HWND owner, const std::string& title, const std::string& text);
    void Open(const std::string& pathOrUrl);
    std::wstring Wide(const std::string& utf8);
} // namespace w2b::ui::shell
