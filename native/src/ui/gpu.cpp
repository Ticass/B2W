// Direct3D 11 for the launcher (see gpu.h).
#include "gpu.h"

#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

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

    Texture Gpu::Dds(const std::filesystem::path& path)
    {
        const auto key = path.wstring();
        if (const auto found = dds.find(key); found != dds.end())
            return found->second;
        Texture out;
        std::ifstream stream(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (data.size() >= 128 && data.compare(0, 4, "DDS ") == 0)
        {
            const auto u32 = [&](const size_t at) { return static_cast<UINT>(static_cast<uint8_t>(data[at]) | static_cast<uint8_t>(data[at + 1]) << 8 | static_cast<uint8_t>(data[at + 2]) << 16 | static_cast<uint8_t>(data[at + 3]) << 24); };
            const UINT height = u32(12), width = u32(16), flags = u32(80), bits = u32(88);
            const std::string fourcc = data.substr(84, 4);
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            UINT block = 0;
            if (flags & 4)
            {
                if (fourcc == "DXT1")
                    format = DXGI_FORMAT_BC1_UNORM, block = 8;
                else if (fourcc == "DXT3")
                    format = DXGI_FORMAT_BC2_UNORM, block = 16;
                else if (fourcc == "DXT5")
                    format = DXGI_FORMAT_BC3_UNORM, block = 16;
            }
            else if (bits == 32 && u32(92) == 0x00FF0000 && u32(100) == 0x000000FF)
                format = DXGI_FORMAT_B8G8R8A8_UNORM;
            const UINT pitch = block ? std::max(1u, (width + 3) / 4) * block : width * 4;
            const size_t bytes = static_cast<size_t>(pitch) * (block ? std::max(1u, (height + 3) / 4) : height);
            if (format != DXGI_FORMAT_UNKNOWN && data.size() >= 128 + bytes)
            {
                D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, format, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
                D3D11_SUBRESOURCE_DATA init{data.data() + 128, pitch, 0};
                ID3D11Texture2D* texture = nullptr;
                if (SUCCEEDED(device->CreateTexture2D(&desc, &init, &texture)))
                {
                    device->CreateShaderResourceView(texture, nullptr, &out.view);
                    texture->Release();
                    out.width = static_cast<int>(width);
                    out.height = static_cast<int>(height);
                }
            }
        }
        dds[key] = out;
        return out;
    }

    namespace
    {
        // the orthographic globe of menupreviews.Globe, per pixel: lifted game
        // texture (x 1.9 + 8), lit by the view-facing normal (0.35 + 0.65 nz)
        constexpr char GLOBE_HLSL[] = R"(
cbuffer Globe : register(b0) { float4 rect; float4 params; float4 display; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    float2 c = float2(id & 1, id >> 1);
    float2 px = rect.xy + c * rect.z;
    V o;
    o.pos = float4(px.x / display.x * 2 - 1, 1 - px.y / display.y * 2, 0, 1);
    o.uv = c;
    return o;
}
Texture2D map : register(t0);
SamplerState wrap : register(s0);
float4 ps(V i) : SV_Target
{
    float size = rect.z, radius = size / 2 - 2;
    float2 q = i.uv * size;
    float nx = (q.x - size / 2) / radius, ny = (size / 2 - q.y) / radius;
    float d = nx * nx + ny * ny;
    if (d >= 1) discard;
    float nz = sqrt(1 - d);
    float lat = asin(clamp(ny * params.y + nz * params.z, -1, 1));
    float dlon = degrees(atan2(nx, nz * params.y - ny * params.z));
    float2 uv = float2(frac((params.x + dlon + 180) / 360), (90 - degrees(lat)) / 180);
    float3 color = min(1, map.SampleLevel(wrap, uv, 0).rgb * 1.9 + 8.0 / 255);
    return float4(color * (0.35 + 0.65 * nz), 1);
}
)";
    } // namespace

    bool Gpu::CreateGlobe()
    {
        if (globeReady || globeFailed)
            return globeReady;
        ID3DBlob *vsCode = nullptr, *psCode = nullptr, *errors = nullptr;
        if (FAILED(D3DCompile(GLOBE_HLSL, sizeof GLOBE_HLSL - 1, "globe", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsCode, &errors))
            || FAILED(D3DCompile(GLOBE_HLSL, sizeof GLOBE_HLSL - 1, "globe", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psCode, &errors)))
        {
            if (errors)
                errors->Release();
            if (vsCode)
                vsCode->Release();
            globeFailed = true;
            return false;
        }
        device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &globeVs);
        device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &globePs);
        vsCode->Release();
        psCode->Release();
        D3D11_BUFFER_DESC buffer{48, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
        device->CreateBuffer(&buffer, nullptr, &globeConstants);
        D3D11_SAMPLER_DESC sampler{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP};
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sampler, &wrapSampler);
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                                 D3D11_COLOR_WRITE_ENABLE_ALL};
        device->CreateBlendState(&blend, &alphaBlend);
        globeReady = globeVs && globePs && globeConstants && wrapSampler && alphaBlend;
        globeFailed = !globeReady;
        return globeReady;
    }

    void Gpu::NewFrame()
    {
        globeCalls.clear();
    }

    void Gpu::GlobeCallback(const ImDrawList*, const ImDrawCmd* command)
    {
        const auto* call = static_cast<const GlobeCall*>(command->UserCallbackData);
        auto* gpu = call->gpu;
        auto* context = gpu->context;
        const ImDrawData* data = ImGui::GetDrawData();
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(gpu->globeConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;
        float* constants = static_cast<float*>(mapped.pData);
        constants[0] = call->rect[0] - data->DisplayPos.x;
        constants[1] = call->rect[1] - data->DisplayPos.y;
        constants[2] = call->rect[2];
        constants[3] = 0;
        std::memcpy(constants + 4, call->params, sizeof call->params);
        constants[8] = data->DisplaySize.x;
        constants[9] = data->DisplaySize.y;
        constants[10] = constants[11] = 0;
        context->Unmap(gpu->globeConstants, 0);
        const D3D11_RECT clip{static_cast<LONG>(command->ClipRect.x - data->DisplayPos.x), static_cast<LONG>(command->ClipRect.y - data->DisplayPos.y),
                              static_cast<LONG>(command->ClipRect.z - data->DisplayPos.x), static_cast<LONG>(command->ClipRect.w - data->DisplayPos.y)};
        context->RSSetScissorRects(1, &clip);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(gpu->globeVs, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &gpu->globeConstants);
        context->PSSetShader(gpu->globePs, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &gpu->globeConstants);
        context->PSSetShaderResources(0, 1, &call->view);
        context->PSSetSamplers(0, 1, &gpu->wrapSampler);
        const float factor[4] = {0, 0, 0, 0};
        context->OMSetBlendState(gpu->alphaBlend, factor, 0xffffffff);
        context->Draw(4, 0);
    }

    void Gpu::DrawGlobe(ImDrawList* list, const Texture& texture, const float x, const float y, const float size, const float centerLon, const float tiltDegrees)
    {
        if (!texture || !CreateGlobe())
        {
            list->AddCircleFilled(ImVec2(x + size / 2, y + size / 2), size / 2 - 2, IM_COL32(24, 30, 38, 255), 96);
            return;
        }
        const float tilt = tiltDegrees * 3.14159265f / 180.0f;
        auto call = std::make_unique<GlobeCall>(GlobeCall{this, texture.view, {x, y, size, 0}, {centerLon, std::cos(tilt), std::sin(tilt), 0}});
        list->AddCallback(GlobeCallback, call.get());
        list->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
        globeCalls.push_back(std::move(call));
    }

    void Gpu::ForgetImages()
    {
        for (auto& [_, texture] : dds)
            if (texture.view)
                texture.view->Release();
        dds.clear();
        for (auto& [_, cached] : images)
            if (cached.texture.view)
                cached.texture.view->Release();
        images.clear();
    }

    void Gpu::Destroy()
    {
        ForgetImages();
        globeCalls.clear();
        for (IUnknown* resource : std::initializer_list<IUnknown*>{globeVs, globePs, globeConstants, wrapSampler, alphaBlend})
            if (resource)
                resource->Release();
        globeVs = nullptr, globePs = nullptr, globeConstants = nullptr, wrapSampler = nullptr, alphaBlend = nullptr;
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
