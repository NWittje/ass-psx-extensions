#pragma once
#include <cstddef>
namespace psx::norm::detail
{
char *allocate_payload(std::size_t);
void free_payload(char *);
} // namespace psx::norm::detail
