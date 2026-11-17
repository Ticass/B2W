#pragma once
// The launcher's Direct3D 11 device, swap chain and image textures.

#include "launcher/images.h"

#include <d3d11.h>
#include <imgui.h>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>
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
        // a DDS (DXT1/DXT3/DXT5 or 32-bit BGRA) straight to the GPU, cached; empty if unreadable
        Texture Dds(const std::filesystem::path& path);
        // BO2's menu globe: an orthographic sphere of an equirectangular texture,
        // drawn per pixel by a shader (ImGui draw callback) in a size x size square
        void DrawGlobe(ImDrawList* list, const Texture& texture, float x, float y, float size, float centerLon, float tiltDegrees);
        void NewFrame(); // frees the previous frame's callback data
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
        std::map<std::wstring, Texture> dds;
        struct GlobeCall
        {
            Gpu* gpu;
            ID3D11ShaderResourceView* view;
            float rect[4], params[4];
        };
        std::vector<std::unique_ptr<GlobeCall>> globeCalls;
        ID3D11VertexShader* globeVs = nullptr;
        ID3D11PixelShader* globePs = nullptr;
        ID3D11Buffer* globeConstants = nullptr;
        ID3D11SamplerState* wrapSampler = nullptr;
        ID3D11BlendState* alphaBlend = nullptr;
        bool globeReady = false, globeFailed = false;
        bool CreateGlobe();
        static void GlobeCallback(const ImDrawList* list, const struct ImDrawCmd* command);
    };
} // namespace w2b::ui
