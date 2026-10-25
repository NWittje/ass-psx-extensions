#include "psx_plugin/motion_support.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <fstream>
#include <filesystem>
#include <string>
#include <charconv>
#include <psx/terrain/protocol.hpp>
#include "psx_plugin/aircraft_adapter_identity.hpp"

namespace psx::xp {
MotionTimeout parse_motion_timeout(std::wstring_view value) noexcept {
    MotionTimeout result;result.provided=true;result.valid=false;
    if(value.empty() || value.size()>5)return result;
    std::uint32_t number{};for(const auto c:value){if(c<L'0'||c>L'9')return result;number=number*10+static_cast<unsigned>(c-L'0');}
    if(number<50 || number>60000)return result;
    result.milliseconds=number;result.valid=true;return result;
}
MotionTimeout motion_timeout_from_environment() noexcept {
    std::array<wchar_t,32> buffer{};SetLastError(ERROR_SUCCESS);
    const auto length=GetEnvironmentVariableW(L"PSX_IOS_BOOST_TIMEOUT_MS",buffer.data(),static_cast<DWORD>(buffer.size()));
    if(!length && GetLastError()==ERROR_ENVVAR_NOT_FOUND)return {};
    if(!length || length>=buffer.size()){MotionTimeout invalid;invalid.valid=false;invalid.provided=true;return invalid;}
    return parse_motion_timeout(std::wstring_view(buffer.data(),length));
}
MotionProfile verify_motion_profile(const ProfileRequest& request) noexcept {
    MotionProfile result;result.generation=request.generation;
    try {
        if(!request.path[0] || request.path.back())return result;
        std::u8string path;for(const char c:std::string_view(request.path.data()))path.push_back(static_cast<char8_t>(c));
        std::ifstream file(std::filesystem::path(path),std::ios::binary|std::ios::ate);
        if(!file)return result;const auto length=file.tellg();
        if(length<=0||length>8*1024*1024)return result;
        std::string bytes(static_cast<std::size_t>(length),'\0');file.seekg(0);
        if(!file.read(bytes.data(),length))return result;
        struct Algorithm {BCRYPT_ALG_HANDLE p{};~Algorithm(){if(p)BCryptCloseAlgorithmProvider(p,0);}} algorithm;
        if(BCryptOpenAlgorithmProvider(&algorithm.p,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return result;
        std::array<UCHAR,32> digest{};
        if(BCryptHash(algorithm.p,nullptr,0,reinterpret_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),digest.data(),static_cast<ULONG>(digest.size()))<0)return result;
        constexpr char hex[]="0123456789ABCDEF";
        for(std::size_t i=0;i<digest.size();++i){result.sha256[i*2]=hex[digest[i]>>4];result.sha256[i*2+1]=hex[digest[i]&15];}
        const auto hash=std::string_view(result.sha256.data());
        const auto generated=hash==aircraft_adapter_acf_sha256;
        result.sparky=hash=="B9EDC9C710AD6EF31E2514C411955D8D7BB2A3F1F30A9E2B4F19A7309F17A29A"||
                      hash=="19D499D63793C5492AC22158748C8E61DA1FF3675D3D686327DF94B161D48DE6"||generated;
        result.audio_qualified=hash=="19D499D63793C5492AC22158748C8E61DA1FF3675D3D686327DF94B161D48DE6"||generated;
        if(generated&&std::filesystem::path(path).filename()==L"747-400-PSX.acf") {
            bool valid_adapter=true;const auto root=std::filesystem::path(path).parent_path();
            for(const auto& identity:aircraft_adapter_files) {
                std::ifstream asset(root/std::filesystem::path(identity.path),std::ios::binary|std::ios::ate);
                if(!asset){valid_adapter=false;break;}const auto size=asset.tellg();
                // Qualified installed RB211 OBJ copies are about20MiB each.
                if(size<=0||size>32*1024*1024){valid_adapter=false;break;}
                std::string content(static_cast<std::size_t>(size),'\0');asset.seekg(0);
                if(!asset.read(content.data(),size)){valid_adapter=false;break;}
                std::array<UCHAR,32> asset_digest{};std::array<char,65> asset_hash{};
                if(BCryptHash(algorithm.p,nullptr,0,reinterpret_cast<PUCHAR>(content.data()),static_cast<ULONG>(content.size()),
                    asset_digest.data(),static_cast<ULONG>(asset_digest.size()))<0){valid_adapter=false;break;}
                for(std::size_t i=0;i<asset_digest.size();++i){asset_hash[i*2]=hex[asset_digest[i]>>4];asset_hash[i*2+1]=hex[asset_digest[i]&15];}
                if(std::string_view(asset_hash.data())!=identity.sha256){valid_adapter=false;break;}
            }
            result.aircraft_state_qualified=valid_adapter;
        }
        if(!result.sparky&&hash!="00E7D19CBD6F053F9DECD2E58FF72F9341C7D4DB970AD68A9603616872EA9B23")return result;
        auto field=[&](std::string_view key,double& value){
            const auto prefix=std::string("\nP ")+std::string(key)+" ";
            const auto start=bytes.find(prefix);if(start==std::string::npos||bytes.find(prefix,start+1)!=std::string::npos)return false;
            const auto begin=start+prefix.size();const auto end=bytes.find_first_of("\r\n",begin);if(end==std::string::npos)return false;
            const auto parsed=std::from_chars(bytes.data()+begin,bytes.data()+end,value);
            return parsed.ec==std::errc{} && parsed.ptr==bytes.data()+end && std::isfinite(value);
        };
        double x{},y{},z{};
        if(!field("acf/_cgY",result.cg_y_ft)||!field("acf/_cgZ",result.cg_z_ft)||!field("acf/_pe_xyz/0",x)||!field("acf/_pe_xyz/1",y)||!field("acf/_pe_xyz/2",z))return result;
        result.eye={x*0.3048,(y-result.cg_y_ft)*0.3048,(z-result.cg_z_ft)*0.3048};result.qualified=true;
        if(result.audio_qualified) {
            NominalFootprint footprint;
            for(unsigned i=0;i<10;++i) {
                const auto root="_gear/"+std::to_string(i)+"/";double type{};
                if(!field(root+"_gear_type",type)||type!=(i==0?3.0:i<5?5.0:0.0))return result;
                if(i>=5)continue;
                double gx{},gy{},gz{},leg_length{},lateral{},longitudinal{},axis{};
                if(!field(root+"_gear_x",gx)||!field(root+"_gear_y",gy)||!field(root+"_gear_z",gz)||
                   !field(root+"_leg_len",leg_length)||!field(root+"_latE",lateral)||!field(root+"_lonE",longitudinal)||
                   !field(root+"_axiE",axis)||leg_length<=0||leg_length>20||lateral!=0||longitudinal!=1||axis!=0)return result;
                // This pinned profile has no lateral strut angle. Positive longitudinal
                // angle points forward (negative body Z); do not guess two-axis order.
                const auto angle=longitudinal*motion::pi/180;
                footprint.centres[i]={gx*.3048,(gy-result.cg_y_ft-leg_length*std::cos(angle))*.3048,
                    (gz-result.cg_z_ft-leg_length*std::sin(angle))*.3048};
                if(!motion::finite(footprint.centres[i]))return result;
            }
            footprint.id=terrain::sparky_footprint_geometry_id;footprint.revision=terrain::footprint_geometry_revision;
            footprint.qualified=true;result.footprint=footprint;
        }
    } catch(...) {result.qualified=false;}
    return result;
}
}
