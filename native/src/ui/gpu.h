#pragma once
// The launcher's Direct3D 11 device, swap chain and image textures.

#include "launcher/images.h"

#include <d3d11.h>
#include <filesystem>
#include <map>
#include <string>

namespace w2b::ui
{
    struct Texture
    {
        ID3D11ShaderResourceView* view = nullptr;
        int width = 0, height = 0;
        explicit operator bool() const
        {
            return view != nullptr;
        }
    };

    class Gpu
    {
    public:
        bool Create(HWND window);
        void Destroy();
        void Resize(UINT width, UINT height);
        void Present(bool vsync);
        void BeginFrame(const float clear[4]);
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;

        // a decoded image as a texture, cached by path, size and write time;
        // the error is filled when it cannot be decoded
        Texture Image(const std::filesystem::path& path, std::string* error = nullptr);
        Texture Upload(const launcher::Image& image);
        void ForgetImages();

    private:
        IDXGISwapChain* swapChain = nullptr;
        ID3D11RenderTargetView* target = nullptr;
        void CreateTarget();
        void ReleaseTarget();
        struct Cached
        {
            uintmax_t size = 0;
            long long time = 0;
            Texture texture;
            std::string error;
        };
        std::map<std::wstring, Cached> images;
    };
} // namespace w2b::ui
