#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_HOSTIMPORTBUDGET_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_HOSTIMPORTBUDGET_HPP

#include <cstdint>

namespace AgcDriver::Graphics {

// Pinned host imports count against the graphics driver's system memory budget; past it ordinary host
// allocations fail. Windows lets a GPU use at most half the physical memory as shared memory, so the
// default budget there is that half less 2 GiB left to the driver's own allocations (a choice, measured
// only with ASTRO BOT on a 32 GB machine), and never below the fixed 6 GiB used elsewhere, which covers
// the registered memory of most address-based shaders. A refused import is mirrored or copied instead,
// and a mirror write-protects its whole range, CPU-only data included.
inline constexpr std::uint64_t HostImportBudgetFloor = std::uint64_t{6} << 30u;
inline constexpr std::uint64_t HostImportBudgetReserve = std::uint64_t{2} << 30u;

constexpr std::uint64_t DefaultHostImportBudget(std::uint64_t physicalBytes) {
    const auto shared = physicalBytes / 2;
    return shared > HostImportBudgetFloor + HostImportBudgetReserve ? (shared - HostImportBudgetReserve) & ~((std::uint64_t{1} << 20u) - 1) : HostImportBudgetFloor;
}

}

#endif
