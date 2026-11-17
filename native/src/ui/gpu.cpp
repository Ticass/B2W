// Direct3D 11 for the launcher (see gpu.h).
#include "gpu.h"

#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace w2b::ui
{
    bool Gpu::Create(const HWND window)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &swapChain, &device, &level, &context);
        if (FAILED(hr)) // no usable GPU: WARP keeps the launcher working
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &swapChain, &device, &level, &context);
        if (FAILED(hr))
            return false;
        CreateTarget();
        return true;
    }

    void Gpu::CreateTarget()
    {
        ID3D11Texture2D* back = nullptr;
        swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        device->CreateRenderTargetView(back, nullptr, &target);
        back->Release();
    }

    void Gpu::ReleaseTarget()
    {
        if (target)
            target->Release();
        target = nullptr;
    }

    void Gpu::Resize(const UINT width, const UINT height)
    {
        if (!swapChain || !width || !height)
            return;
        ReleaseTarget();
        swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        CreateTarget();
    }

    void Gpu::BeginFrame(const float clear[4])
    {
        context->OMSetRenderTargets(1, &target, nullptr);
        context->ClearRenderTargetView(target, clear);
    }

    void Gpu::Present(const bool vsync)
    {
        swapChain->Present(vsync ? 1 : 0, 0);
    }

    Texture Gpu::Upload(const launcher::Image& image)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(image.width);
        desc.Height = static_cast<UINT>(image.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{image.rgba.data(), static_cast<UINT>(image.width * 4), 0};
        ID3D11Texture2D* texture = nullptr;
        Texture out;
        if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)))
        {
            device->CreateShaderResourceView(texture, nullptr, &out.view);
            texture->Release();
            out.width = image.width;
            out.height = image.height;
        }
        return out;
    }

    Texture Gpu::Image(const std::filesystem::path& path, std::string* error)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        const auto time = ec ? 0 : std::filesystem::last_write_time(path, ec).time_since_epoch().count();
        auto& cached = images[path.wstring()];
        if (cached.texture.view == nullptr && cached.error.empty() || cached.size != size || cached.time != time)
        {
            if (cached.texture.view)
                cached.texture.view->Release();
            cached = {size, static_cast<long long>(time), {}, {}};
            try
            {
                cached.texture = Upload(launcher::DecodeImage(path));
            }
            catch (const std::exception& e)
            {
                cached.error = e.what();
            }
        }
        if (error)
            *error = cached.error;
        return cached.texture;
    }

    void Gpu::ForgetImages()
    {
        for (auto& [_, cached] : images)
            if (cached.texture.view)
                cached.texture.view->Release();
        images.clear();
    }

    void Gpu::Destroy()
    {
        ForgetImages();
        ReleaseTarget();
        if (swapChain)
            swapChain->Release();
        if (context)
            context->Release();
        if (device)
            device->Release();
        swapChain = nullptr;
        context = nullptr;
        device = nullptr;
    }
} // namespace w2b::ui
