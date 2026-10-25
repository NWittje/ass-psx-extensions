#pragma once

#include "sdk_api.hpp"
#include <cmath>
#include <cstddef>
#include <span>
#include <type_traits>

namespace psx::xp {
template<class T> struct Result {
    Status status{Status::inactive};
    T value{};
    explicit operator bool() const noexcept { return status == Status::ok; }
};

namespace detail {
template<class T> bool finite(T value) noexcept {
    if constexpr (std::is_floating_point_v<T>) return std::isfinite(value);
    else return true;
}
template<class T> struct Scalar;
template<> struct Scalar<int> {
    static constexpr auto type = xplmType_Int;
    static int get(XPLMDataRef r) { return XPLMGetDatai(r); }
    static void set(XPLMDataRef r, int v) { XPLMSetDatai(r, v); }
};
template<> struct Scalar<float> {
    static constexpr auto type = xplmType_Float;
    static float get(XPLMDataRef r) { return XPLMGetDataf(r); }
    static void set(XPLMDataRef r, float v) { XPLMSetDataf(r, v); }
};
template<> struct Scalar<double> {
    static constexpr auto type = xplmType_Double;
    static double get(XPLMDataRef r) { return XPLMGetDatad(r); }
    static void set(XPLMDataRef r, double v) { XPLMSetDatad(r, v); }
};
template<class T> struct Array;
template<> struct Array<int> {
    static constexpr auto type = xplmType_IntArray;
    static int get(XPLMDataRef r, int* p, int o, int n) { return XPLMGetDatavi(r,p,o,n); }
    static void set(XPLMDataRef r, int* p, int o, int n) { XPLMSetDatavi(r,p,o,n); }
};
template<> struct Array<float> {
    static constexpr auto type = xplmType_FloatArray;
    static int get(XPLMDataRef r, float* p, int o, int n) { return XPLMGetDatavf(r,p,o,n); }
    static void set(XPLMDataRef r, float* p, int o, int n) { XPLMSetDatavf(r,p,o,n); }
};
template<> struct Array<std::byte> {
    static constexpr auto type = xplmType_Data;
    static int get(XPLMDataRef r, std::byte* p, int o, int n) { return XPLMGetDatab(r,p,o,n); }
    static void set(XPLMDataRef r, std::byte* p, int o, int n) { XPLMSetDatab(r,p,o,n); }
};

class Binding {
public:
    explicit Binding(SdkApi& sdk) noexcept : sdk_(sdk) {}
    Status bind(const char* name, XPLMDataTypeID type, Access access) {
        if (auto s = sdk_.ready(); s != Status::ok) return s;
        ref_ = nullptr; // Failed rebind must not leave the former name usable.
        generation_ = sdk_.generation();
        access_ = access;
        if (!name || !name[0]) return Status::invalid_argument;
        auto ref = XPLMFindDataRef(name);
        auto s = sdk_.inspect(ref, type, access == Access::read_write);
        if (s == Status::ok) ref_ = ref;
        return s;
    }
protected:
    Status check(XPLMDataTypeID type, bool writing) const {
        if (auto s = sdk_.ready(); s != Status::ok) return s;
        if (generation_ != sdk_.generation()) return Status::stale_binding;
        if (writing && access_ != Access::read_write) return Status::write_not_authorized;
        return sdk_.inspect(ref_, type, writing);
    }
    XPLMDataRef ref_{};
private:
    SdkApi& sdk_;
    std::uint64_t generation_{};
    Access access_{Access::read_only};
};
} // namespace detail

template<class T> class DataRef final : private detail::Binding {
    using Api = detail::Scalar<T>;
public:
    explicit DataRef(SdkApi& sdk) noexcept : Binding(sdk) {}
    Status bind(const char* name, Access access = Access::read_only) {
        return Binding::bind(name, Api::type, access);
    }
    Result<T> read() const {
        if (auto s = check(Api::type, false); s != Status::ok) return {s, {}};
        const auto value = Api::get(ref_);
        if (!detail::finite(value)) return {Status::non_finite, {}};
        return {Status::ok, value};
    }
    // ok means setter dispatched, not acknowledged/applied by the simulator.
    Status write(T value) const {
        if (auto s = check(Api::type, true); s != Status::ok) return s;
        if (!detail::finite(value)) return Status::non_finite;
        Api::set(ref_, value);
        return Status::ok;
    }
};

template<class T> class DataArray final : private detail::Binding {
    using Api = detail::Array<T>;
public:
    static constexpr std::size_t max_transfer = 4096;
    explicit DataArray(SdkApi& sdk) noexcept : Binding(sdk) {}
    Status bind(const char* name, Access access = Access::read_only) {
        return Binding::bind(name, Api::type, access);
    }
    Result<int> size() const {
        if (auto s = check(Api::type, false); s != Status::ok) return {s, 0};
        int n = Api::get(ref_, nullptr, 0, 0);
        return n < 0 ? Result<int>{Status::unavailable,0} : Result<int>{Status::ok,n};
    }
    // Treat the entire destination as invalid on failure, including short reads.
    Status read(int offset, std::span<T> destination) const {
        if (auto s = range(offset, destination.size(), false); s != Status::ok) return s;
        if (destination.empty()) return Status::ok;
        const auto n = static_cast<int>(destination.size());
        if (Api::get(ref_, destination.data(), offset, n) != n) return Status::short_read;
        for (auto value : destination) if (!detail::finite(value)) return Status::non_finite;
        return Status::ok;
    }
    Status write(int offset, std::span<const T> source) const {
        if (auto s = range(offset, source.size(), true); s != Status::ok) return s;
        for (auto value : source) if (!detail::finite(value)) return Status::non_finite;
        if (!source.empty()) {
            // SDK's legacy signature lacks const; its contract treats this as input.
            Api::set(ref_, const_cast<T*>(source.data()), offset, static_cast<int>(source.size()));
        }
        return Status::ok;
    }
private:
    Status range(int offset, std::size_t count, bool writing) const {
        if (auto s = check(Api::type, writing); s != Status::ok) return s;
        if (offset < 0 || count > max_transfer) return Status::out_of_range;
        const int length = Api::get(ref_, nullptr, 0, 0);
        if (length < 0) return Status::unavailable;
        // Subtraction prevents offset+count overflow, even for INT_MAX input.
        if (offset > length || count > static_cast<std::size_t>(length-offset)) return Status::out_of_range;
        return Status::ok;
    }
};
} // namespace psx::xp
