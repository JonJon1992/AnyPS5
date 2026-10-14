#include "prx/libSceAgcDriver/Graphics/include/HostImportBudget.hpp"
#include <iostream>

namespace {

using AgcDriver::Graphics::DefaultHostImportBudget;
using AgcDriver::Graphics::HostImportBudgetFloor;
constexpr std::uint64_t GiB = std::uint64_t{1} << 30u;
constexpr std::uint64_t MiB = std::uint64_t{1} << 20u;

static_assert(DefaultHostImportBudget(0) == HostImportBudgetFloor, "no memory reported keeps the floor");
static_assert(DefaultHostImportBudget(8 * GiB) == 6 * GiB, "an 8 GB machine keeps the floor");
static_assert(DefaultHostImportBudget(16 * GiB) == 6 * GiB, "a 16 GB machine keeps the earlier 6 GiB default");
static_assert(DefaultHostImportBudget(32 * GiB) == 14 * GiB, "a 32 GB machine gets half its memory less 2 GiB");
static_assert(DefaultHostImportBudget(64 * GiB) == 30 * GiB, "a 64 GB machine gets half its memory less 2 GiB");

}

int main() {
    // A "32 GB" machine reports a little less (34279649280 bytes measured): ASTRO BOT's 3232 MiB
    // registered range on top of 9171 MiB of other imports must still fit, in whole MiB.
    const auto budget = DefaultHostImportBudget(34279649280ull);
    if (budget < (9171 + 3232) * MiB || budget % MiB != 0) {
        std::cerr << "budget for a 32 GB machine is " << budget << " bytes\n";
        return 1;
    }
    std::cout << "host import budget tests passed\n";
    return 0;
}
