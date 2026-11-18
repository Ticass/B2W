// Shared helpers of the bridge stage (see bridge_support.h).
#include "bridge_support.h"

#include "extract/stockwaw.h"

#include <windows.h>

#include <atomic>

namespace w2b::pipeline::bridge
{
    TemporaryFolder::TemporaryFolder()
    {
        static std::atomic<unsigned long long> serial{0};
        const auto temporary = fs::temp_directory_path();
        const auto prefix = "w2b_bridge_" + std::to_string(GetCurrentProcessId()) + "_";
        do
            folder_ = temporary / (prefix + std::to_string(serial++));
        while (!fs::create_directory(folder_));
    }

    TemporaryFolder::~TemporaryFolder()
    {
        std::error_code e;
        fs::remove_all(folder_, e);
    }

    Json Call(Json (*step)(const fs::path&), const Json& request)
    {
        const TemporaryFolder folder;
        const auto path = folder.Path() / "request.json";
        files::WriteText(path, py::JsonDumpsDefault(request));
        // WAW2BO2_CAPTURE=<folder> keeps every request, to replay a step alone
        if (const wchar_t* capture = _wgetenv(L"WAW2BO2_CAPTURE"); capture && *capture)
        {
            static std::atomic<unsigned> captured{0};
            std::error_code e;
            fs::create_directories(capture, e);
            fs::copy_file(path, fs::path(capture) / ("request_" + std::to_string(captured++) + ".json"), fs::copy_options::overwrite_existing, e);
        }
        return step(path);
    }

    Json StockRequest(const extract::StockWaw* stock)
    {
        if (!stock)
            return nullptr;
        return Json{{"waw_root", Str(stock->wawRoot)}, {"unlinker", Str(stock->unlinker)}, {"work", Str(stock->work)},
                    {"prepared", stock->PreparedJson()}};
    }

    void AdoptPrepared(extract::StockWaw* stock, const Json& result)
    {
        if (stock && result.is_object() && result.contains("stock_prepared") && result["stock_prepared"].is_object())
            stock->ApplyPrepared(result["stock_prepared"]);
    }
}
