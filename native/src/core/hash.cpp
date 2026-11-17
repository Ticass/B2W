#include "hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace w2b::hash
{
    std::string Sha256Hex(const std::string_view data)
    {
        unsigned char digest[32];
        const auto status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                       static_cast<ULONG>(data.size()), digest, sizeof(digest));
        if (status != 0)
            throw std::runtime_error("SHA-256 failed");
        static constexpr char hex[] = "0123456789abcdef";
        std::string out(64, '0');
        for (int i = 0; i < 32; i++)
        {
            out[static_cast<size_t>(2 * i)] = hex[digest[i] >> 4];
            out[static_cast<size_t>(2 * i + 1)] = hex[digest[i] & 0xF];
        }
        return out;
    }
} // namespace w2b::hash
