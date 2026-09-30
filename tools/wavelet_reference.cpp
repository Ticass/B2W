// Audit-only x86 oracle: calls the user's unmodified WaW AssetViewer decoder.
// Known AssetViewer version only; entry signature and image base checked.
// Build with MSVC x86 /link /BASE:0x10000000. No game files are modified.
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

struct State {
    uint16_t lookahead, bitOffset;
    uint8_t* input;
    int width, height, channels, stride, level;
    uint8_t initialized;
    uint8_t padding[3];
};
static_assert(sizeof(State) == 32, "build this tool as x86");
std::vector<uint8_t> read(const char* path) {
    FILE* f = nullptr; fopen_s(&f, path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END); auto size = ftell(f); rewind(f);
    std::vector<uint8_t> b(size); fread(b.data(), 1, size, f); fclose(f); return b;
}
int main(int argc, char** argv) {
    if (argc != 4) { puts("wavelet_reference AssetViewer.exe input.iwi output.raw"); return 2; }
    auto exe = read(argv[1]);
    if (exe.size() < sizeof(IMAGE_DOS_HEADER)) return 3;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe.data());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(exe.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.ImageBase != 0x400000) return 4;
    // EXEs are mapped as data by LoadLibraryEx. Copy just this known image's
    // decoder/table sections, without resolving/running any imports.
    auto base = static_cast<uint8_t*>(VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage,
                          MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!base) return 4;
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        for (auto window : {std::pair<uint32_t,uint32_t>{0xa0000, 0xb0000}, {0x140000, 0x160000}}) {
            auto lo = std::max<uint32_t>(window.first, section[i].VirtualAddress);
            auto hi = std::min<uint32_t>(window.second, section[i].VirtualAddress + section[i].SizeOfRawData);
            if (hi > lo) memcpy(base + lo, exe.data() + section[i].PointerToRawData + lo - section[i].VirtualAddress, hi - lo);
        }
    }
    // This executable has no relocation directory. The measured decoder has
    // exactly four absolute references; internal calls are relative. Relocate
    // those references and its four-entry switch table, in this private copy.
    uintptr_t delta = reinterpret_cast<uintptr_t>(base) - 0x400000;
    for (unsigned offset = 0xa17e0; offset < 0xa1f68 - 3; ++offset) {
        uint32_t value; memcpy(&value, base + offset, 4);
        if (value == 0x548398 || value == 0x54c398 || value == 0x550398 || value == 0x4a1f68) {
            value += delta; memcpy(base + offset, &value, 4); offset += 3;
        }
    }
    for (int i = 0; i < 4; ++i) *reinterpret_cast<uint32_t*>(base + 0xa1f68 + 4 * i) += delta;
    const uint8_t sig[] = {0x83, 0xec, 0x5c, 0x53, 0x55, 0x56, 0x8b, 0x74, 0x24, 0x74};
    if (memcmp(base + 0xa1940, sig, sizeof(sig))) { puts("unsupported decoder signature"); return 5; }
    auto fn = reinterpret_cast<int(__cdecl*)(const void*, void*, State*)>(base + 0xa1940);
    auto blob = read(argv[2]);
    if (blob.size() < 28 || memcmp(blob.data(), "IWi\6", 4) || blob[4] < 6 || blob[4] > 10) return 6;
    State state{};
    state.width = *reinterpret_cast<uint16_t*>(&blob[6]);
    state.height = *reinterpret_cast<uint16_t*>(&blob[8]);
    const int channels[] = {4,3,2,1,1};
    state.channels = channels[blob[4] - 6];
    state.stride = state.channels == 3 ? 4 : state.channels;
    int faces = blob[5] & 4 ? 6 : 1;
    int count = 1;
    if (!(blob[5] & 2)) for (int size = std::max(state.width, state.height); size > 1; size >>= 1) ++count;
    blob.resize(blob.size() + 16, 0); // original decoder reads ahead up to 4 bytes
    state.input = blob.data() + 28;
    std::vector<std::vector<uint8_t>> parents(faces), levels(count);
    for (state.level = count - 1; state.level >= 0; --state.level) {
        int size = std::max(1, state.width >> state.level) * std::max(1, state.height >> state.level) * state.stride;
        for (int face = 0; face < faces; ++face) {
            std::vector<uint8_t> out(size);
            fn(parents[face].data(), out.data(), &state);
            levels[state.level].insert(levels[state.level].end(), out.begin(), out.end());
            parents[face] = std::move(out);
        }
    }
    FILE* f = nullptr; fopen_s(&f, argv[3], "wb"); if (!f) return 7;
    for (auto& level : levels) fwrite(level.data(), 1, level.size(), f);
    fclose(f); return 0;
}
