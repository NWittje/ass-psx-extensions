#pragma once
#include "core.hpp"
#include <span>

namespace hwio::cpflight {
enum class InputKind { press, state, step, unavailable };
struct Input {
    std::string_view code, name, action, group;
    unsigned role{}, channel{};
    InputKind kind{};
};
std::span<const Input> inputs() noexcept;
const Input* input(std::string_view) noexcept;
class Framer {
    std::array<char,256> bytes_{};
    std::size_t size_{};
    bool discard_{};
public:
    template<class F> void feed(std::span<const std::uint8_t> bytes,F&& receive) {
        for(const auto b:bytes) {
            if(!b){if(size_ && !discard_)receive(std::string_view(bytes_.data(),size_));size_=0;discard_=false;}
            else if(b<32 || b>126 || size_==bytes_.size()){size_=0;discard_=true;}
            else if(!discard_)bytes_[size_++]=static_cast<char>(b);
        }
    }
};
struct Identity {
    bool manufacturer{}, inventory{}, captain{}, first_officer{};
    std::string model,serial,problem;
    bool receive(std::string_view);
    bool ready() const noexcept;
};
struct Numeric { unsigned channel{}; int value{}; bool mach{}; };
std::optional<Numeric> numeric(std::string_view) noexcept;
std::string lamp(unsigned,bool);
std::string display(unsigned,std::string_view);
}
