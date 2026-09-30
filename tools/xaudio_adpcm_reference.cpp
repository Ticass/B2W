// Audit-only x86 harness for the installed XAudio2 2.0 ADPCM decoder.
// No engine, device, voice, or playback is created. Do not redistribute DLLs.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <algorithm>

static uint16_t u16(const unsigned char* p) { uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t u32(const unsigned char* p) { uint32_t v; memcpy(&v,p,4); return v; }

int wmain(int argc, wchar_t** argv) {
    if (sizeof(void*) != 4 || argc != 3) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<unsigned char> source((std::istreambuf_iterator<char>(input)), {});
    if(source.size()<12 || memcmp(source.data(),"RIFF",4) || memcmp(source.data()+8,"WAVE",4)) return 3;
    const unsigned char *fmt=nullptr,*data=nullptr; uint32_t fmtSize=0,dataSize=0;
    for(size_t p=12;p+8<=source.size();) {
        const uint32_t n=u32(source.data()+p+4);
        if(n>source.size()-p-8) return 4;
        if(!memcmp(source.data()+p,"fmt ",4)) {fmt=source.data()+p+8;fmtSize=n;}
        if(!memcmp(source.data()+p,"data",4)) {data=source.data()+p+8;dataSize=n;}
        p+=8+n+(n&1);
    }
    if(!fmt||!data||fmtSize<50||u16(fmt)!=2||u16(fmt+14)!=4) return 5;
    const unsigned channels=u16(fmt+2),align=u16(fmt+12),samples=u16(fmt+18);
    if((channels!=1&&channels!=2)||align<=7*channels||dataSize%align||samples!=2+(align-7*channels)*2/channels) return 6;
    const int16_t coefficients[14]={256,0,512,-256,0,0,192,64,240,0,460,-208,392,-232};
    if(u16(fmt+20)!=7||memcmp(fmt+22,coefficients,sizeof(coefficients))) return 7;
    for(unsigned p=0;p<dataSize;p+=align)
        for(unsigned c=0;c<channels;++c) if(data[p+c]>=7) return 8;
    wchar_t system[MAX_PATH]; GetSystemDirectoryW(system,MAX_PATH);
    const std::wstring dllPath=std::wstring(system)+L"\\XAudio2_0.dll";
    HMODULE module=LoadLibraryW(dllPath.c_str()); if(!module) return 9;
    // Function and signature measured in this installed DLL via IDA.
    const auto address=reinterpret_cast<unsigned char*>(module)+(0x44c110-0x400000);
    const unsigned char signature[]={0x8b,0xff,0x55,0x8b,0xec,0x8b,0x45,0x0c,0x83,0xf8,0x08,0x75,0x1f,0x33,0xc0,0x8b,0x4d,0x08,0x83,0xf9};
    if(memcmp(address,signature,sizeof(signature))) {FreeLibrary(module);return 10;}
    using Decoder=unsigned (__stdcall*)(const unsigned char*,unsigned,unsigned char*,unsigned,uint16_t);
    using Selector=Decoder (__stdcall*)(unsigned,unsigned);
    Decoder decode=reinterpret_cast<Selector>(address)(channels,16);
    if(!decode) {FreeLibrary(module);return 11;}
    const uint64_t required=static_cast<uint64_t>(dataSize)/align*samples*channels*2;
    if(required>0x40000000) {FreeLibrary(module);return 14;}
    const unsigned expected=static_cast<unsigned>(required);
    std::vector<unsigned char> pcm(expected+64,0);
    const unsigned used=decode(data,dataSize,pcm.data(),expected,static_cast<uint16_t>(align));
    FreeLibrary(module);
    if(used!=expected) return 12;
    std::ofstream output(argv[2],std::ios::binary); output.write(reinterpret_cast<char*>(pcm.data()),used);
    return output?0:13;
}
