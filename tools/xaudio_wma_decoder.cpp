// x86 xWMA bridge to the user's installed, hash-audited XAudio2 2.0 decoder.
// No audio engine, device, voice or playback. Functions resolve by unique code
// signatures, not fixed offsets. Python validates the entire supported DLL.
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <string>

static uint16_t u16(const unsigned char* p){uint16_t v;memcpy(&v,p,2);return v;}
static uint32_t u32(const unsigned char* p){uint32_t v;memcpy(&v,p,4);return v;}
struct Input { const unsigned char* data; unsigned remaining,packet; unsigned calls=0; };
static int __stdcall provide(void* opaque,uint32_t* info) {
    auto& src=*static_cast<Input*>(opaque);
    memset(info,0,40);
    const unsigned amount=std::min(src.remaining,src.packet);
    info[0]=reinterpret_cast<uint32_t>(src.data); info[1]=amount;
    info[2]=1; info[3]=(src.remaining<=amount);
    src.remaining-=amount; src.data+=amount; ++src.calls;
    return 0;
}
int wmain(int argc,wchar_t** argv) {
    if(sizeof(void*)!=4||argc!=3)return 2;
    std::ifstream input(argv[1],std::ios::binary);
    std::vector<unsigned char> source((std::istreambuf_iterator<char>(input)),{});
    if(source.size()<12||memcmp(source.data(),"RIFF",4)||memcmp(source.data()+8,"XWMA",4))return 3;
    const size_t riffEnd=static_cast<size_t>(u32(source.data()+4))+8;
    if(riffEnd<12||riffEnd>source.size())return 4;
    const unsigned char *fmt=nullptr,*data=nullptr,*dpds=nullptr;unsigned fmtSize=0,dataSize=0,dpdsSize=0;
    for(size_t p=12;p<riffEnd;) {
        if(riffEnd-p<8)return 4;
        const unsigned size=u32(source.data()+p+4);if(size>riffEnd-p-8)return 4;
        if(!memcmp(source.data()+p,"fmt ",4)){fmt=source.data()+p+8;fmtSize=size;}
        if(!memcmp(source.data()+p,"data",4)){data=source.data()+p+8;dataSize=size;}
        if(!memcmp(source.data()+p,"dpds",4)){dpds=source.data()+p+8;dpdsSize=size;}
        p+=8+size+(size&1);
    }
    if(!fmt||!data||!dpds||fmtSize<18||dpdsSize<4||dpdsSize%4)return 5;
    const unsigned channels=u16(fmt+2),rate=u32(fmt+4),packet=u16(fmt+12);
    if((channels!=1&&channels!=2)||!packet||dataSize%packet)return 6;
    wchar_t system[MAX_PATH];GetSystemDirectoryW(system,MAX_PATH);
    HMODULE module=LoadLibraryW((std::wstring(system)+L"\\XAudio2_0.dll").c_str());if(!module)return 7;
    const auto base=reinterpret_cast<unsigned char*>(module);
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    auto sections=IMAGE_FIRST_SECTION(nt);
    auto find=[&](const std::string& text)->unsigned char* {
        std::vector<int> pattern; std::istringstream tokens(text); std::string token;
        while(tokens>>token)pattern.push_back(token=="?"?-1:std::stoi(token,nullptr,16));
        unsigned char* match=nullptr;
        for(unsigned s=0;s<nt->FileHeader.NumberOfSections;++s) {
            if(!(sections[s].Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
            auto start=base+sections[s].VirtualAddress;
            const auto length=sections[s].Misc.VirtualSize;
            if(pattern.size()>length)continue;
            for(size_t p=0;p<=length-pattern.size();++p) {
                bool same=true;for(size_t b=0;b<pattern.size();++b)
                    if(pattern[b]>=0&&start[p+b]!=pattern[b]){same=false;break;}
                if(same){if(match)return nullptr;match=start+p;}
            }
        }
        return match;
    };
    auto pcm_to_wma=find("8B FF 55 8B EC 8B 45 0C 8B 55 08 33 C9 89 08 89 48 04 89 48 08 89 48 0C 89 48 10 89 48 14 56 66");
    auto defaults=find("8B FF 55 8B EC 56 8B 75 08 57 6A 74 33 FF 57 56 E8 F9 BE FD FF 89 3E 89 7E 04 89 7E 08 89 7E 0C");
    auto format=find("8B FF 55 8B EC 83 EC 18 56 8B 75 08 57 8D 45 E8 50 56 E8 59 ED FF FF 8B 7D 0C 57 56 E8 0F FC FF");
    auto create=find("8B FF 55 8B EC 8B 45 08 85 C0 8B 4D 0C 74 0A 85 C9 7D 06 33 C0 5D C2 08 00 56 51 50 6A 00 E8 8D");
    auto destroy=find("8B FF 55 8B EC 56 8B 75 08 85 F6 74 17 6A 00 6A 00 6A 02 E8 38 DA 00 00 56 E8 02 FE FF FF 56 E8");
    auto init=find("8B FF 55 8B EC 83 EC 10 53 8B 5D 08 56 57 33 FF 3B DF 89 7D F4 89 7D FC 0F 84 DE 08 00 00 39 7D");
    auto reset=find("8B FF 55 8B EC 53 57 8B 7D 08 33 DB 3B FB 75 08 5F 33 C0 5B 5D C2 04 00 56 8B 37 3B F3 75 09 5E");
    auto decode=find("8B FF 55 8B EC 51 53 56 8B 75 08 57 33 FF 3B F7 0F 84 E8 01 00 00 8B 06 3B C7 89 45 FC 0F 84 DB");
    auto get_pcm=find("8B FF 55 8B EC 6A FE 68 ? ? ? ? 68 ? ? ? ? 64 A1 00 00 00 00 50 83 EC 08 53 56 57 A1 ?");
    if(!pcm_to_wma||!defaults||!format||!create||!destroy||!init||!reset||!decode||!get_pcm){FreeLibrary(module);return 8;}
    using PcmToWma=void(__stdcall*)(void*,void*);
    using Defaults=void(__stdcall*)(void*);
    using Format=int(__stdcall*)(void*,void*,void*);
    using New=void*(__stdcall*)(void*,int);
    using Delete=void(__stdcall*)(void*);
    using Init=int(__stdcall*)(void*,void*,void*,void*,unsigned*,void*);
    using Reset=int(__stdcall*)(void*);
    using Decode=int(__stdcall*)(void*,unsigned*,unsigned*,void*);
    using GetPCM=int(__stdcall*)(void*,unsigned,unsigned*,unsigned char*,unsigned,unsigned*,int64_t*,unsigned*,void*,void*);
    uint32_t pcmFormat[6]={rate,channels,0,16,2,0};
    alignas(16) unsigned char wma[32]={},options[128]={},profile[128]={};
    reinterpret_cast<PcmToWma>(pcm_to_wma)(pcmFormat,wma);
    memcpy(wma,fmt,12); // tag/channels/rate/average bytes; source profile inferred by native engine.
    reinterpret_cast<Defaults>(defaults)(options);
    const int formatResult=reinterpret_cast<Format>(format)(wma,options,profile);
    if(formatResult<0){std::cerr<<"format "<<std::hex<<formatResult;return 9;}
    const unsigned profilePacket=u16(profile+12);
    if(profilePacket!=packet){std::cerr<<"packet source="<<packet<<" profile="<<profilePacket;return 10;}
    void* decoder=reinterpret_cast<New>(create)(nullptr,0);if(!decoder)return 11;
    Input context{data,dataSize,packet};
    uint32_t params[4]={0,reinterpret_cast<uint32_t>(&provide),reinterpret_cast<uint32_t>(&context),0};
    unsigned state=0;
    int result=reinterpret_cast<Init>(init)(decoder,profile,pcmFormat,nullptr,&state,params);
    if(result<0){std::cerr<<"init "<<std::hex<<result;reinterpret_cast<Delete>(destroy)(decoder);return 12;}
    result=reinterpret_cast<Reset>(reset)(decoder);state=2;
    std::vector<unsigned char> decoded,buffer(16384*channels*2);
    unsigned passes=0;
    while(result>=0&&state>=2&&passes++<1000000) {
        unsigned frames=0;
        if(state==2)result=reinterpret_cast<Decode>(decode)(decoder,&frames,&state,nullptr);
        else if(state==3) {
            result=reinterpret_cast<GetPCM>(get_pcm)(decoder,16384,&frames,buffer.data(),16384,nullptr,nullptr,&state,nullptr,nullptr);
            if(frames>16384){result=-1;break;}
            decoded.insert(decoded.end(),buffer.begin(),buffer.begin()+frames*channels*2);
        } else {result=-1;break;}
        if(decoded.size()>0x40000000){result=-1;break;}
    }
    reinterpret_cast<Delete>(destroy)(decoder);FreeLibrary(module);
    if(result<0||passes>=1000000){std::cerr<<"decode "<<std::hex<<result<<" state="<<state;return 13;}
    std::ofstream output(argv[2],std::ios::binary);output.write(reinterpret_cast<const char*>(decoded.data()),decoded.size());
    std::cout<<"{\"decoded_bytes\":"<<decoded.size()<<",\"dpds_bytes\":"<<u32(dpds+dpdsSize-4)
             <<",\"input_remaining\":"<<context.remaining<<",\"callbacks\":"<<context.calls<<",\"state\":"<<state<<"}\n";
    return output?0:14;
}
