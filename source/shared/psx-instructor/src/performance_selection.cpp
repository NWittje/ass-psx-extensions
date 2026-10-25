#include <psx/instructor/performance_selection.hpp>
#include <psx/instructor/communicator.hpp>
#include "metadata_json.hpp"
#include <map>
#include <variant>
#include <array>
#include <limits>
namespace psx::instructor {
namespace {
[[noreturn]] void unavailable(const std::string &why) {throw std::runtime_error("PSX flap speeds unavailable: "+why);}
}
PerformanceSelection::PerformanceSelection(std::filesystem::path directory)
 :directory_(std::move(directory)){}
FlapSpeeds PerformanceSelection::parse(std::string_view bytes,std::uint32_t pid,std::uint64_t birth,std::uint64_t time) {
  if(bytes.empty()||bytes.size()>4096)unavailable("metadata exceeds bounds");
  metadata::Json parser{bytes};using Value=std::variant<std::monostate,std::string,double>;std::map<std::string,Value> fields;
  parser.expect('{');if(!parser.take('}'))for(;;){auto name=parser.str();parser.expect(':');parser.ws();Value value;
    if(parser.at<bytes.size()&&bytes[parser.at]=='"')value=parser.str();
    else if(bytes.substr(parser.at,4)=="null")parser.at+=4;
    else value=parser.real();
    if(!fields.emplace(name,std::move(value)).second||fields.size()>13)unavailable("duplicate or unknown metadata field");
    if(parser.take('}'))break;parser.expect(',');
  }
  parser.ws();if(parser.at!=bytes.size())unavailable("incomplete metadata");
  auto numeric=[&](const char *name){auto i=fields.find(name);if(i==fields.end()||!std::holds_alternative<double>(i->second))unavailable(std::string("missing numeric ")+name);return std::get<double>(i->second);};
  auto whole=[&](const char *name){auto value=numeric(name);if(value<0||value>9007199254740991.||std::trunc(value)!=value)unavailable(std::string("invalid integer ")+name);return std::uint64_t(value);};
  const auto schema=whole("schema");
  if((schema!=1&&schema!=2)||fields.size()!=(schema==1?11u:13u))unavailable("unsupported or incomplete metadata schema");
  if(whole("pid")!=pid||whole("process_start_unix_ms")!=birth)unavailable("metadata process does not own Main");
  const auto sequence=whole("sequence"),sampled=whole("sampled_unix_ms");
  if(!sequence||sampled>time||time-sampled>3000)unavailable("metadata is stale or from the future");
  auto error=fields.find("error");if(error==fields.end()||!std::holds_alternative<std::string>(error->second))unavailable("missing reader status");
  if(!std::get<std::string>(error->second).empty())unavailable(std::get<std::string>(error->second));
  auto integer=[&](const char *name){auto value=numeric(name);if(std::trunc(value)!=value||value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())unavailable(std::string("invalid integer ")+name);return int(value);};
  FlapSpeeds result{pid,birth,sequence,sampled,numeric("gross_weight_kg"),integer("calculation_weight_kg"),integer("landing_altitude_ft"),integer("vref25_kt"),integer("vref30_kt"),{},"Native UP marker unsupported by schema1"};
  if(result.gross_weight_kg<=0||std::floor(result.gross_weight_kg+.5)!=result.calculation_weight_kg||result.ref25_kt<=0||result.ref25_kt>994||result.ref30_kt<=0||result.ref30_kt>994)unavailable("invalid native performance values");
  if(schema==2){
    auto marker=fields.find("up_maneuver_kt"),reason=fields.find("up_error");
    if(marker==fields.end()||reason==fields.end()||!std::holds_alternative<std::string>(reason->second))unavailable("missing UP marker status");
    result.up_notice=std::get<std::string>(reason->second);
    if(result.up_notice.empty()){
      const auto value=std::get_if<double>(&marker->second);
      if(value&&*value>0&&*value<=400&&std::trunc(*value)==*value)result.up_maneuver_kt=int(*value);
      else result.up_notice="Invalid native UP marker";
    }else if(!std::holds_alternative<std::monostate>(marker->second))result.up_notice="Inconsistent native UP marker evidence";
  }
  return result;
}
FlapSpeeds PerformanceSelection::read(const NavigationSelection &selection,std::uint64_t time) {
  if(!selection.pid||!selection.process_start_unix_ms)unavailable("local Main identity unavailable");
  const auto directory=directory_.empty()?(selection.metadata_directory.empty()?NavigationDatabaseSelection::default_directory():selection.metadata_directory).parent_path()/L"performance":directory_;
  const auto path=directory/(std::to_wstring(selection.pid)+L".json");
  HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)unavailable("read-only exporter reader is not active");
  std::array<char,4097> bytes{};DWORD count{};bool ok=ReadFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr)!=0;CloseHandle(file);
  if(!ok)unavailable("cannot read metadata");
  auto result=parse(std::string_view(bytes.data(),count),selection.pid,selection.process_start_unix_ms,time);
  if(last_&&last_->pid==result.pid&&last_->process_start_unix_ms==result.process_start_unix_ms&&result.sequence<=last_->sequence) {
    if(result.sequence==last_->sequence&&last_bytes_==std::string_view(bytes.data(),count))return *last_;
    unavailable("regressed or changed same-sequence metadata");
  }
  last_=result;last_bytes_.assign(bytes.data(),count);return result;
}
}
