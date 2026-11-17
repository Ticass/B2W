// Native dialogs (see shell.h).
#include "shell.h"

#include "core/files.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace w2b::ui::shell
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        std::optional<std::string> Run(HWND owner, const CLSID& kind, const std::wstring& title, const Filters& filters, const std::string& folder,
                                       const FILEOPENDIALOGOPTIONS extra, const std::wstring& fileName = L"", const std::wstring& extension = L"")
        {
            ComPtr<IFileDialog> dialog;
            if (FAILED(CoCreateInstance(kind, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
                return std::nullopt;
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | extra);
            dialog->SetTitle(title.c_str());
            std::vector<COMDLG_FILTERSPEC> specs;
            for (const auto& [label, pattern] : filters)
                specs.push_back({label.c_str(), pattern.c_str()});
            if (!specs.empty())
                dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
            if (!fileName.empty())
                dialog->SetFileName(fileName.c_str());
            if (!extension.empty())
                dialog->SetDefaultExtension(extension.c_str());
            if (!folder.empty())
            {
                ComPtr<IShellItem> item;
                if (SUCCEEDED(SHCreateItemFromParsingName(Wide(folder).c_str(), nullptr, IID_PPV_ARGS(&item))))
                    dialog->SetFolder(item.Get());
            }
            if (FAILED(dialog->Show(owner)))
                return std::nullopt;
            ComPtr<IShellItem> result;
            PWSTR path = nullptr;
            if (FAILED(dialog->GetResult(&result)) || FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                return std::nullopt;
            std::string out = files::Utf8(std::filesystem::path(path));
            CoTaskMemFree(path);
            return out;
        }
    } // namespace

    std::wstring Wide(const std::string& utf8)
    {
        return files::FromUtf8(utf8).wstring();
    }

    std::optional<std::string> OpenFile(HWND owner, const std::wstring& title, const Filters& filters, const std::string& initialFolder)
    {
        return Run(owner, CLSID_FileOpenDialog, title, filters, initialFolder, FOS_FILEMUSTEXIST);
    }

    std::optional<std::string> SaveFile(HWND owner, const std::wstring& title, const Filters& filters, const std::wstring& fileName, const std::wstring& extension)
    {
        return Run(owner, CLSID_FileSaveDialog, title, filters, "", FOS_OVERWRITEPROMPT, fileName, extension);
    }

    std::optional<std::string> ChooseFolder(HWND owner, const std::wstring& title, const std::string& initialFolder)
    {
        return Run(owner, CLSID_FileOpenDialog, title, {}, initialFolder, FOS_PICKFOLDERS);
    }

    void Info(HWND owner, const std::string& title, const std::string& text)
    {
        MessageBoxW(owner, Wide(text).c_str(), Wide(title).c_str(), MB_OK | MB_ICONINFORMATION);
    }

    void Error(HWND owner, const std::string& title, const std::string& text)
    {
        MessageBoxW(owner, Wide(text).c_str(), Wide(title).c_str(), MB_OK | MB_ICONERROR);
    }

    bool AskYesNo(HWND owner, const std::string& title, const std::string& text)
    {
        return MessageBoxW(owner, Wide(text).c_str(), Wide(title).c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES;
    }

    void Open(const std::string& pathOrUrl)
    {
        ShellExecuteW(nullptr, L"open", Wide(pathOrUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
} // namespace w2b::ui::shell
