#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <psapi.h>
namespace psx::cdu
{
inline std::uint64_t cpu_100ns()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;
    auto value = [](FILETIME t) {
        return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return value(kernel) + value(user);
}
inline std::size_t private_bytes()
{
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    return GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m))
               ? m.PrivateUsage
               : 0;
}
} // namespace psx::cdu
