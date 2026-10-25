#include "psx_plugin/dataref.hpp"

// Compile every supported typed adapter against the real SDK, including when
// the passive foundation does not consume aircraft datarefs at runtime.
template class psx::xp::DataRef<int>;
template class psx::xp::DataRef<float>;
template class psx::xp::DataRef<double>;
template class psx::xp::DataArray<int>;
template class psx::xp::DataArray<float>;
template class psx::xp::DataArray<std::byte>;
