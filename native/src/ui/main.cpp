// B2W.exe: the native desktop launcher (Win32 + Direct3D 11 + Dear ImGui).
#include "app.h"
#include "theme.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <windows.h>
#include <shellscalingapi.h>

#include <memory>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    w2b::ui::Gpu gpu;
    std::unique_ptr<w2b::ui::App> app;
    UINT pendingWidth = 0, pendingHeight = 0;
    float dpiScale = 1.0f;

    LRESULT CALLBACK WndProc(const HWND window, const UINT message, const WPARAM w, const LPARAM l)
    {
        if (ImGui_ImplWin32_WndProcHandler(window, message, w, l))
            return 1;
        switch (message)
        {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED)
                pendingWidth = LOWORD(l), pendingHeight = HIWORD(l);
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(l);
            info->ptMinTrackSize.x = static_cast<LONG>(1000 * dpiScale);
            info->ptMinTrackSize.y = static_cast<LONG>(720 * dpiScale);
            return 0;
        }
        case WM_CLOSE:
            if (!app || app->RequestClose())
                DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, w, l);
        }
    }
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW windowClass{sizeof windowClass, CS_CLASSDC, WndProc, 0, 0, instance};
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = L"B2WLauncher";
    RegisterClassExW(&windowClass);
    dpiScale = GetDpiForSystem() / 96.0f;
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"WaW → BO2 Mod Tools", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                        static_cast<int>(1160 * dpiScale), static_cast<int>(820 * dpiScale), nullptr, nullptr, instance, nullptr);
    if (!gpu.Create(window))
    {
        MessageBoxW(nullptr, L"Direct3D 11 is not available on this system.", L"WaW → BO2 Mod Tools", MB_ICONERROR);
        return 1;
    }
    dpiScale = GetDpiForWindow(window) / 96.0f;
    ShowWindow(window, show);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    w2b::ui::theme::Apply(dpiScale);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(gpu.device, gpu.context);
    app = std::make_unique<w2b::ui::App>(gpu, window);
    for (int i = 1; i < __argc; i++)
        if (const std::wstring arg = __wargv[i]; arg.starts_with(L"--tab="))
            app->ShowTab(std::string(arg.begin() + 6, arg.end()));

    const float clear[4] = {0.941f, 0.941f, 0.941f, 1.0f};
    bool running = true;
    int quietFrames = 0;
    while (running)
    {
        // Idle launcher: sleep until input. While a build runs, wake for its
        // console lines and the elapsed clock (10 Hz is enough for both).
        if (quietFrames > 2)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, app->NeedsTicks() ? 100 : INFINITE, QS_ALLINPUT);
        MSG msg;
        bool input = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                running = false;
            input = true;
        }
        if (!running)
            break;
        quietFrames = input ? 0 : quietFrames + 1;
        if (pendingWidth && pendingHeight)
        {
            gpu.Resize(pendingWidth, pendingHeight);
            pendingWidth = pendingHeight = 0;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app->Frame();
        ImGui::Render();
        gpu.BeginFrame(clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        gpu.Present(true);
    }
    app.reset();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    gpu.Destroy();
    DestroyWindow(window);
    CoUninitialize();
    return 0;
}
