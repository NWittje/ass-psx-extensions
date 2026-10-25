#include "psx_plugin/traffic_model.hpp"
#include "psx_plugin/traffic_inputs.hpp"
#include "psx_plugin/traffic_channels.hpp"
#include "psx_plugin/audio_assets.hpp"
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <map>
#include <algorithm>
#include <Windows.h>
#include <bcrypt.h>
namespace psx::xp {
namespace {
std::string read(const std::filesystem::path& p) {std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("model input");return {std::istreambuf_iterator<char>(f),{}};}
std::string utf8(const std::filesystem::path& p) {const auto s=p.generic_u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
std::string hash(std::string_view value){BCRYPT_ALG_HANDLE algorithm{};if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("hash");std::array<UCHAR,32> digest{};const auto status=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),static_cast<ULONG>(value.size()),digest.data(),static_cast<ULONG>(digest.size()));BCryptCloseAlgorithmProvider(algorithm,0);if(status<0)throw std::runtime_error("hash");std::string out;constexpr char hex[]="0123456789ABCDEF";for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;}
}
bool traffic_sdk_path(std::string_view path) noexcept {
 if(path.empty()||path.front()=='/'||path.find(':')!=std::string_view::npos||path.find('\\')!=std::string_view::npos||path.find('\0')!=std::string_view::npos)return false;
 for(std::size_t at=0;at<path.size();){const auto end=path.find('/',at);const auto part=path.substr(at,end==std::string_view::npos?path.size()-at:end-at);if(part.empty()||part=="."||part=="..")return false;if(end==std::string_view::npos)return true;at=end+1;if(at==path.size())return false;}
 return false;
}
bool prepare_traffic_load_paths(TrafficModel& model,const std::filesystem::path& root) noexcept {
 model.sdk_loadable=false;for(auto& path:model.sdk_objects)path.clear();
 try {
  if(!model.qualified||!root.is_absolute())return false;
  const auto base=root.lexically_normal();std::array<std::string,8> paths;
  for(std::size_t i=0;i<paths.size();++i){if(model.objects[i].find('\0')!=std::string::npos)return false;const auto absolute=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(model.objects[i].c_str())));if(!absolute.is_absolute())return false;const auto relative=absolute.lexically_normal().lexically_relative(base);paths[i]=utf8(relative);if(!traffic_sdk_path(paths[i]))return false;}
  model.sdk_objects=std::move(paths);model.sdk_loadable=true;return true;
 }catch(...){return false;}
}
TrafficModel prepare_traffic_model(const std::filesystem::path& root,const std::filesystem::path& cache,bool runway_actor) noexcept {
 try {
  TrafficModel out;
  const std::string prefix=runway_actor?"psx_ios/runway_actor/v1/":"psx_ios/traffic/v1/";
  for(const auto& input:traffic_inputs)if(!audio_file_identity(root/input.path,input.bytes,input.hash))return out;
  constexpr std::array<const char*,8> names{"738fuselage.obj","738wings.obj","738tailwinglets.obj","738cfm56L.obj","738cfm56R.obj","738nosegear.obj","738maingear.obj","738lights.obj"};
  const auto source=root/"Aircraft/Laminar Research/Boeing 737-800/objects";
  std::array<std::string,8> originals;
  std::set<std::string> refs;const std::regex native(R"((?:sim|laminar)/[^\s]+)");
  for(std::size_t i=0;i<8;++i){originals[i]=read(source/names[i]);for(std::sregex_iterator j(originals[i].begin(),originals[i].end(),native),end;j!=end;++j)refs.insert(j->str());}
  // Explicit scalar substitution; no native aircraft bindings enter an instance.
  std::map<std::string,std::string> mapping;
  if(refs.size()!=traffic_channels.size())return {};
  for(const auto& ref:refs){const auto channel=std::find_if(traffic_channels.begin(),traffic_channels.end(),[&](const TrafficChannel& c){return c.source==ref;});if(channel==traffic_channels.end())return {};
   const auto name=prefix+"channel"+std::to_string(out.refs.size());mapping.emplace(ref,name);out.refs.push_back(name);out.defaults.push_back(runway_actor&&ref.find("/gear/deploy_ratio[")!=std::string::npos?1.0F:runway_actor&&channel->neutral<0?0.0F:channel->neutral);}
  const auto strobe=out.refs.size();out.refs.push_back(prefix+"strobe");out.defaults.push_back(0);
  out.refs.push_back(prefix+"visible");out.defaults.push_back(0);
  if(strobe>=126)return {};
  std::filesystem::create_directories(cache);
  std::ofstream manifest(cache/"profile.txt",std::ios::binary|std::ios::trunc);
  if(!manifest)return {};
  manifest<<(runway_actor?"runway actor/profile revision1; gear down; zero tyre deflection/steer/rotation; deployed mesh nose/main contact anchors; true heading\n":"model/profile revision1; native anchor1 = OBJ origin; zero anatomical offset; true track; generic approximate placement\n");
  for(const auto& input:traffic_inputs)manifest<<input.path<<' '<<input.bytes<<' '<<input.hash<<'\n';
  for(const auto& channel:traffic_channels)manifest<<channel.source<<" -> "<<mapping.at(std::string(channel.source))<<" fixed "<<(runway_actor&&channel.source.find("/gear/deploy_ratio[")!=std::string_view::npos?1.0F:runway_actor&&channel.neutral<0?0.0F:channel.neutral)<<'\n';
  for(std::size_t i=0;i<8;++i){
   std::istringstream input(originals[i]);std::ostringstream result;std::string line;
   // Paired bb/pm describe one physical light. Generic static emits both billboard and spill.
   std::set<std::string> lights;
   while(std::getline(input,line)){
    std::istringstream tokens(line);std::string cmd;tokens>>cmd;
    if(cmd=="GLOBAL_cockpit_lit")continue;
    if(cmd=="LIGHT_PARAM"){
     std::string kind,x,y,z,r,g,b,index,intensity,dx,dy,dz,width;
     if(!(tokens>>kind>>x>>y>>z>>r>>g>>b>>index>>intensity>>dx>>dy>>dz>>width))return {};
     const bool nav=kind.rfind("airplane_nav_",0)==0,flash=kind.rfind("airplane_strobe_",0)==0;
     if(!nav&&!flash)continue;
     const auto key=x+" "+y+" "+z+" "+r+" "+g+" "+b+" "+intensity+" "+dx+" "+dy+" "+dz+" "+width;
     if(!lights.insert(key).second)continue;
     result<<"ANIM_begin\nANIM_hide 0 0.5 "<<prefix<<"visible\n";
     if(flash)result<<"ANIM_hide 0 0.5 "<<prefix<<"strobe\n";
     result<<"LIGHT_PARAM airplane_generic_static "<<key<<'\n';
     result<<"ANIM_end\n";
     continue;
    }
    if(cmd.rfind("TEXTURE",0)==0){
     std::string name;tokens>>name;if(name.empty()){result<<line<<'\n';continue;}
     auto path=source/name;if(!std::filesystem::exists(path)){path.replace_extension(".dds");if(!std::filesystem::exists(path))return {};}
     auto relative=std::filesystem::relative(path,cache);
     if(relative.empty()){
      // Cross-volume qualification requires an explicit hash-qualified texture directory
      // in the task cache. Production's application-owned cache shares the XP volume.
      relative=std::filesystem::path("textures")/path.filename();
     }
     const auto identity=std::find_if(traffic_inputs.begin(),traffic_inputs.end(),[&](const TrafficInput& file){return std::filesystem::path(file.path).filename()==path.filename();});
     if(identity==traffic_inputs.end()||!audio_file_identity(cache/relative,identity->bytes,identity->hash))return {};
     result<<cmd<<' '<<utf8(relative)<<'\n';continue;
    }
    std::string mapped;std::size_t cursor{};
    for(std::sregex_iterator j(line.begin(),line.end(),native),end;j!=end;++j){const auto pos=static_cast<std::size_t>(j->position());mapped+=line.substr(cursor,pos-cursor);mapped+=mapping.at(j->str());cursor=pos+static_cast<std::size_t>(j->length());}
    mapped+=line.substr(cursor);
    const bool geometry=cmd=="TRIS"||cmd=="LINES"||cmd=="LIGHTS";
    if(geometry)result<<"ANIM_begin\nANIM_hide 0 0.5 "<<prefix<<"visible\n";
    result<<mapped<<'\n';if(geometry)result<<"ANIM_end\n";
   }
   const auto path=cache/names[i];const auto text=result.str();
   // Generation-owned output is newly authored text, never a vendor-file transfer.
   std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(text.data(),static_cast<std::streamsize>(text.size()));file.close();if(!file)return {};
   out.objects[i]=utf8(path);manifest<<names[i]<<" derived_bytes "<<text.size()<<" sha256 "<<hash(text)<<'\n';
  }
  manifest.close();if(!manifest)return {};out.qualified=true;prepare_traffic_load_paths(out,root);return out;
 }catch(...){return {};}
}
}
