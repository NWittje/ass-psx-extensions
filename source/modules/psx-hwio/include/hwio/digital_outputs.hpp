#pragma once
#include "catalogue.hpp"

namespace hwio {
struct DigitalOutputState {std::string id,reason;bool available{},on{};std::int64_t qpc{};};
class DigitalOutputs {
    friend struct FdsContractProbe;
    struct Source {
        std::string record;
        DigitalEncoding encoding{DigitalEncoding::numbers};
        std::vector<std::string> prefix;
        std::array<double,256> values{};
        unsigned fields{};
        bool valid{},initialized{},local_arm{};
        std::uint64_t sequence{},epoch{};
        std::int64_t qpc{},completed_qpc{};
    };
    struct Operand {DigitalOperand definition;std::size_t source{};};
    struct Output {
        const CatalogueDigitalOutput* definition{};
        std::vector<std::vector<Operand>> branches;
        std::vector<Operand> requirements;
        std::vector<std::pair<std::size_t,std::vector<int>>> applicability;
        std::vector<std::size_t> dependencies;
        DigitalOutputState state;
    };
    std::vector<Source> sources_;
    std::vector<Output> outputs_;
    std::map<std::string,std::size_t,std::less<>> ids_;
    std::vector<std::uint8_t> changed_;
    std::uint64_t epoch_{};
    bool ready_{};
public:
    void configure(const Catalogue&);
    void update(const Mapper&);
    const DigitalOutputState* find(std::string_view) const noexcept;
    std::vector<DigitalOutputState> snapshot() const;
};
} // namespace hwio
