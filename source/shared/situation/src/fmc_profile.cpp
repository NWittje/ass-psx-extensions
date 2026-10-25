#include <psx/situation/fmc_profile.hpp>
#include <algorithm>
#include <stdexcept>

namespace psx::fmc {namespace {
void check(bool v,const char* why){if(!v)throw std::invalid_argument(why);}
bool nonzero(const Hash& hash){return std::any_of(hash.begin(),hash.end(),[](auto v){return v!=0;});}
unsigned element_width(Kind k){switch(k){
 case Kind::boolean_array:case Kind::byte_array:return 1;
 case Kind::char_array:case Kind::short_array:case Kind::string:case Kind::string_builder:return 2;
 case Kind::int_array:case Kind::float_array:case Kind::reference_array:return 4;
 case Kind::long_array:case Kind::double_array:return 8;
 default:return 0;
}}
void sorted_ids(const std::vector<std::uint32_t>& ids,std::uint32_t maximum){
 std::uint32_t prior{};for(auto id:ids){check(id>prior&&id<=maximum,"FMC profile reference classes");prior=id;}
}
void field_rule(const FieldRule& f,std::size_t types){
 check(f.id>0&&static_cast<unsigned>(f.kind)>=1&&static_cast<unsigned>(f.kind)<=9,"FMC profile field");
 if(f.kind==Scalar::reference){
  sorted_ids(f.owned_types,static_cast<std::uint32_t>(types));sorted_ids(f.external_roles,65535);
  check(f.nullable||!f.owned_types.empty()||!f.external_roles.empty(),"FMC profile empty reference rule");
 }else check(!f.nullable&&f.owned_types.empty()&&f.external_roles.empty(),"FMC profile scalar reference rule");
}
void reference_rule(std::uint32_t ref,const FieldRule& f,const Snapshot& s){
 if(!ref){check(f.nullable,"FMC profile null binding");return;}
 if(ref&external_reference)check(std::binary_search(f.external_roles.begin(),f.external_roles.end(),ref&~external_reference),"FMC profile external binding");
 else check(std::binary_search(f.owned_types.begin(),f.owned_types.end(),s.nodes[ref-1].type),"FMC profile object type");
}
}
void validate_profile(const Profile& p){
 check(nonzero(p.compatibility.source_jar)&&nonzero(p.compatibility.profile)&&nonzero(p.compatibility.navigation_database)&&nonzero(p.compatibility.model_configuration),"FMC profile compatibility");
 check(p.compatibility.model==Model::legacy||p.compatibility.model==Model::ng,"FMC profile model");
 check(!p.types.empty()&&p.types.size()<=4096&&!p.roots.empty()&&p.roots.size()<=1024,"FMC profile count");
 std::uint32_t prior{};
 for(const auto& t:p.types){
  check(t.id==++prior,"FMC profile type identity");
  if(t.kind==Kind::record){
   check(t.minimum_elements==0&&t.maximum_elements==0&&!t.fields.empty()&&t.fields.size()<=max_cells,"FMC profile record shape");
   std::uint32_t field{};for(const auto& f:t.fields){check(f.id>field,"FMC profile duplicate field");field=f.id;field_rule(f,p.types.size());}
   check(t.component.id==0,"FMC profile record component");
  }else {
   auto width=element_width(t.kind);check(width&&t.fields.empty()&&t.minimum_elements<=t.maximum_elements&&t.maximum_elements<=psx::situation::max_artifact/width,"FMC profile array capacity");
   if(t.kind==Kind::reference_array){check(t.component.id==1&&t.component.kind==Scalar::reference,"FMC profile array component");field_rule(t.component,p.types.size());}
   else check(t.component.id==0,"FMC profile primitive component");
  }
 }
 prior=0;for(auto r:p.roots){check(r.role>prior&&r.type>0&&r.type<=p.types.size(),"FMC profile root");prior=r.role;}
 check(!p.recall_qualified||p.capture_qualified,"FMC profile recall without capture proof");
}
void require_profile(const Snapshot& s,const Profile& p,bool recall){
 validate_profile(p);check(p.capture_qualified&&(!recall||p.recall_qualified),"FMC native profile unavailable");
 require_compatible(s,p.compatibility);check(s.roots.size()==p.roots.size(),"FMC profile root coverage");
 for(std::size_t i=0;i<s.roots.size();++i){auto root=s.roots[i];auto rule=p.roots[i];check(root.role==rule.role&&s.nodes[root.node-1].type==rule.type,"FMC profile root binding");}
 for(const auto& node:s.nodes){
  check(node.type<=p.types.size(),"FMC profile unknown type");const auto& rule=p.types[node.type-1];check(node.kind==rule.kind,"FMC profile node kind");
  if(node.kind==Kind::record){
   check(node.cells.size()==rule.fields.size(),"FMC profile field coverage");
   for(std::size_t i=0;i<node.cells.size();++i){auto cell=node.cells[i];const auto& f=rule.fields[i];check(cell.field==f.id&&cell.kind==f.kind,"FMC profile field binding");if(cell.kind==Scalar::reference)reference_rule(static_cast<std::uint32_t>(cell.bits),f,s);}
  }else {
   const auto count=node.bytes.size()/element_width(node.kind);check(count>=rule.minimum_elements&&count<=rule.maximum_elements,"FMC profile native capacity");
   if(node.kind==Kind::reference_array)for(std::size_t i=0;i<node.bytes.size();i+=4){std::uint32_t value{};for(unsigned j=0;j<4;++j)value|=std::uint32_t(node.bytes[i+j])<<(j*8);reference_rule(value,rule.component,s);}
  }
 }
}
}
