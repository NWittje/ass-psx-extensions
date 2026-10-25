#include "psx_plugin/audio_assets.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <fstream>

namespace psx::xp {
bool audio_file_identity(const std::filesystem::path& path,std::uint64_t expected_size,std::string_view expected) noexcept {
    try {
        std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file||file.tellg()!=static_cast<std::streamoff>(expected_size))return false;
        file.seekg(0);
        struct Algorithm {BCRYPT_ALG_HANDLE p{};~Algorithm(){if(p)BCryptCloseAlgorithmProvider(p,0);}} algorithm;
        struct Hash {BCRYPT_HASH_HANDLE p{};~Hash(){if(p)BCryptDestroyHash(p);}} hash;
        if(BCryptOpenAlgorithmProvider(&algorithm.p,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||BCryptCreateHash(algorithm.p,&hash.p,nullptr,0,nullptr,0,0)<0)return false;
        std::array<UCHAR,65536> bytes{};std::uint64_t remaining=expected_size;
        while(remaining){const auto count=static_cast<ULONG>((std::min)(remaining,static_cast<std::uint64_t>(bytes.size())));
            if(!file.read(reinterpret_cast<char*>(bytes.data()),count)||BCryptHashData(hash.p,bytes.data(),count,0)<0)return false;remaining-=count;}
        std::array<UCHAR,32> digest{};if(BCryptFinishHash(hash.p,digest.data(),static_cast<ULONG>(digest.size()),0)<0)return false;
        std::array<char,64> text{};constexpr char hex[]="0123456789ABCDEF";
        for(std::size_t i=0;i<digest.size();++i){text[2*i]=hex[digest[i]>>4];text[2*i+1]=hex[digest[i]&15];}
        return std::string_view(text.data(),text.size())==expected;
    }catch(...){return false;}
}
bool qualify_audio_assets(const AudioHost& host) noexcept {
    if(!host.profile.audio_qualified||!host.aircraft_path[0]||host.aircraft_path.back())return false;
    try {
        std::u8string utf8;for(const char c:std::string_view(host.aircraft_path.data()))utf8.push_back(static_cast<char8_t>(c));
        const auto directory=std::filesystem::path(utf8).parent_path()/"fmod";
        for(const auto& asset:sparky_assets)if(!audio_file_identity(directory/asset.name,asset.bytes,asset.sha256))return false;
        return true;
    }catch(...){return false;}
}
bool qualify_audio_host() noexcept {
    std::array<wchar_t,32768> path{};const auto n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!n||n>=path.size())return false;
    // docs/evidence/xplane-fmod-threading-20261002.json: asynchronous Studio,
    // right-handed Core. A different executable requires new evidence.
    return audio_file_identity(path.data(),91041072,"B9D613BF4F9ADDCB205D38827F101D29A71932EE79321D958760D2F4E0681DD5");
}
}
