#include <psx/instructor/instructor_metadata.hpp>
#include <psx/instructor/communicator.hpp>
#include "metadata_json.hpp"
#include <array>
#include <cmath>
#include <map>
#include <variant>
namespace psx::instructor {
namespace {
[[noreturn]] void unavailable(const std::string &s) {throw std::runtime_error("PSX instructor readout unavailable: "+s);}
}
InstructorMetadata::InstructorMetadata(std::filesystem::path p)
 :directory_(std::move(p)) {}
InstructorSample InstructorMetadata::parse(std::string_view bytes,std::uint32_t pid,std::uint64_t birth,std::uint64_t time) {
  if(bytes.empty()||bytes.size()>4096)unavailable("metadata exceeds bounds");
  metadata::Json p{bytes};using V=std::variant<std::monostate,std::string,double,bool>;std::map<std::string,V> f;
  p.expect('{');if(!p.take('}'))for(;;) {
    auto key=p.str();p.expect(':');p.ws();V value;
    if(p.at<bytes.size()&&bytes[p.at]=='"')value=p.str();
    else if(bytes.substr(p.at,4)=="null")p.at+=4;
    else if(bytes.substr(p.at,4)=="true"){value=true;p.at+=4;}
    else if(bytes.substr(p.at,5)=="false"){value=false;p.at+=5;}
    else value=p.real();
    if(!f.emplace(std::move(key),std::move(value)).second||f.size()>11)unavailable("duplicate or unknown fields");
    if(p.take('}'))break;p.expect(',');
  }
  p.ws();if(p.at!=bytes.size()||f.size()!=11)unavailable("incomplete metadata");
  auto number=[&](const char *key){auto i=f.find(key);if(i==f.end()||!std::holds_alternative<double>(i->second))unavailable(std::string("missing ")+key);return std::get<double>(i->second);};
  auto whole=[&](const char *key){auto n=number(key);if(n<0||n>9007199254740991.||std::trunc(n)!=n)unavailable(std::string("invalid ")+key);return std::uint64_t(n);};
  if(whole("schema")!=1||whole("pid")!=pid||whole("process_start_unix_ms")!=birth)unavailable("metadata does not match Main process");
  InstructorSample result;result.pid=pid;result.process_start_unix_ms=birth;result.sequence=whole("sequence");result.sampled_unix_ms=whole("sampled_unix_ms");result.lifecycle_epoch=whole("lifecycle_epoch");
  if(!result.sequence||result.sampled_unix_ms>time||time-result.sampled_unix_ms>2000)unavailable("sample is stale or from the future");
  auto loading=f.find("loading");if(loading==f.end()||(!std::holds_alternative<std::monostate>(loading->second)&&!std::holds_alternative<bool>(loading->second)))unavailable("invalid loading state");
  if(auto flag=std::get_if<bool>(&loading->second))result.loading=*flag;
  auto read=[&](const char *key,const char *error,double low,double high,std::optional<double>&out,std::string&why) {
    auto e=f.find(error),v=f.find(key);if(e==f.end()||!std::holds_alternative<std::string>(e->second)||v==f.end())unavailable("incomplete native readout");
    why=std::get<std::string>(e->second);
    if(!result.loading||*result.loading){why="PSX loading or initialization unavailable";return;}
    if(auto n=std::get_if<double>(&v->second)) {if(why.empty()&&std::isfinite(*n)&&*n>=low&&*n<=high)out=*n;else if(why.empty())why="Native readout out of range";}
    else if(!std::holds_alternative<std::monostate>(v->second))unavailable("invalid native readout type");
    else if(why.empty())why="Native readout unavailable";
  };
  read("corrected_altitude_ft","corrected_altitude_error",-10000,100000,result.corrected_altitude_ft,result.corrected_altitude_error);
  read("tropopause_ft","tropopause_error",1,100000,result.tropopause_ft,result.tropopause_error);
  return result;
}
InstructorSample InstructorMetadata::read(const NavigationSelection &selection,std::uint64_t time) {
  if(!selection.pid||!selection.process_start_unix_ms)unavailable("local Main identity unavailable");
  const auto directory=directory_.empty()?(selection.metadata_directory.empty()?NavigationDatabaseSelection::default_directory():selection.metadata_directory).parent_path()/L"instructor":directory_;
  const auto path=directory/(std::to_wstring(selection.pid)+L".json");
  HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)unavailable("read-only exporter metadata is not active");
  std::array<char,4097> bytes{};DWORD count{};bool ok=ReadFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr)!=0;CloseHandle(file);
  if(!ok)unavailable("cannot read metadata");
  const auto text=std::string_view(bytes.data(),count);auto result=parse(text,selection.pid,selection.process_start_unix_ms,time);
  if(last_&&last_->pid==result.pid&&last_->process_start_unix_ms==result.process_start_unix_ms&&result.sequence<=last_->sequence) {
    if(result.sequence==last_->sequence&&last_bytes_==text)return *last_;
    unavailable("regressed or changed same-sequence metadata");
  }
  last_=result;last_bytes_=text;return result;
}
}
