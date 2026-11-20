#include "d3d.h"

#include "core/cache.h"
#include "core/text.h"

#include <filesystem>

#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <mutex>

namespace w2b::shaders
{
    namespace
    {
        // Strictness + IEEE strictness, as shaders.COMPILE_FLAGS.
        constexpr UINT COMPILE_FLAGS = (1u << 11) | (1u << 13);

        struct Compiler
        {
            pD3DCompile compile = nullptr;
            pD3DDisassemble disassemble = nullptr;
        };

        const Compiler& Load()
        {
            static Compiler compiler;
            static std::once_flag once;
            std::call_once(once,
                           []
                           {
                               // the system copy, which is what ctypes.WinDLL finds for the converter
                               const auto module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                               if (!module)
                                   throw ShaderError("d3dcompiler_47.dll is missing");
                               compiler.compile = reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile"));
                               compiler.disassemble = reinterpret_cast<pD3DDisassemble>(GetProcAddress(module, "D3DDisassemble"));
                           });
            return compiler;
        }

        std::string BlobBytes(ID3DBlob* blob)
        {
            std::string data(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
            blob->Release();
            return data;
        }

        std::filesystem::path SystemCompilerPath()
        {
            wchar_t system[MAX_PATH];
            const UINT length = GetSystemDirectoryW(system, MAX_PATH);
            return std::filesystem::path(std::wstring(system, length)) / L"d3dcompiler_47.dll";
        }

        std::string Hex(const HRESULT hr)
        {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "%#x", static_cast<unsigned>(hr));
            return buffer;
        }
    } // namespace

    std::string Disassemble(const std::string_view bytecode)
    {
        static const std::string compiler = cache::FileIdentity(SystemCompilerPath());
        const auto key = cache::Key({"disassemble", compiler, bytecode});
        if (auto cached = cache::Load("sm3_disassembly", key))
            return std::move(*cached);
        ID3DBlob* blob = nullptr;
        const auto hr = Load().disassemble(bytecode.data(), bytecode.size(), 0, nullptr, &blob);
        if (FAILED(hr))
            throw ShaderError("D3DDisassemble failed: " + Hex(hr));
        std::string text(util::RStrip(BlobBytes(blob), std::string_view("\0", 1)));
        cache::Store("sm3_disassembly", key, text);
        return text;
    }

    std::string CompileHlsl(const std::string_view source, const std::string_view profile)
    {
        if (profile != "ps_5_0" && profile != "vs_5_0" && profile != "ps_3_0" && profile != "vs_3_0")
            throw ShaderError("unsupported target profile " + std::string(profile));
        // D3DCompile is ~20 ms a program and a pure function of these: reuse
        // results across builds (only successes are stored)
        static const std::string compiler = cache::FileIdentity(SystemCompilerPath());
        const auto key = cache::Key({"hlsl", profile, std::to_string(COMPILE_FLAGS), compiler, source});
        if (auto cached = cache::Load("hlsl_compile", key))
            return std::move(*cached);
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const std::string target(profile);
        const auto hr =
            Load().compile(source.data(), source.size(), "waw_translated.hlsl", nullptr, nullptr, "main", target.c_str(), COMPILE_FLAGS, 0, &code, &errors);
        const std::string diagnostics = errors ? BlobBytes(errors) : std::string();
        if (FAILED(hr))
        {
            if (code)
                code->Release();
            throw ShaderError("D3DCompile failed: " + diagnostics);
        }
        auto bytes = BlobBytes(code);
        cache::Store("hlsl_compile", key, bytes);
        return bytes;
    }
} // namespace w2b::shaders
