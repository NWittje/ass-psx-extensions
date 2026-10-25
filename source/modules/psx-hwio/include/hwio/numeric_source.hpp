#pragma once
#include "core.hpp"
namespace hwio {
struct NumericSource {
    std::string id,name,record,unit,power_record;int field{-1};unsigned fields{1};std::uint32_t mask{},power_mask{};unsigned shift{};
    double minimum{},maximum{1},scale{1},offset{};bool demand{};
    bool operator==(const NumericSource&)const=default;
};
struct NumericSourceCache {
    std::uint64_t epoch{UINT64_MAX},sequence{UINT64_MAX},power_sequence{UINT64_MAX};NumericSource source;std::optional<double> number,power;
};
std::optional<double> actuator_numeric(std::string_view,int,unsigned)noexcept;
std::optional<double> numeric_source_value(const Mapper&,const NumericSource&,NumericSourceCache&);
std::vector<NumericSource> numeric_sources();
}
