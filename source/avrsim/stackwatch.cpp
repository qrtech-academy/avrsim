/**
 * @file Stack watch implementation details.
 */
#include <cstdint>

#include "avrsim/stackwatch.hpp"

namespace avrsim
{
namespace
{
/** The lowest seen before anything has been sampled, being no address the stack can hold. */
std::uint16_t NoMark{0xFFFFU};
} // namespace

// -----------------------------------------------------------------------------
StackWatch::StackWatch(const std::uint16_t low, const std::uint16_t high) noexcept
    : myLow{low}
    , myHigh{high}
    , myLowest{NoMark}
    , myEntered{}
{}

// -----------------------------------------------------------------------------
StackWatch::~StackWatch() noexcept = default;

// -----------------------------------------------------------------------------
void StackWatch::sample(const std::uint16_t ptr) noexcept
{
    const bool inside{(ptr >= myLow) && (ptr <= myHigh)};
    if (!inside) { return; }
    if (!myEntered || (ptr < myLowest)) { myLowest = ptr; }
    myEntered = true;
}

// -----------------------------------------------------------------------------
bool StackWatch::entered() const noexcept { return myEntered; }

// -----------------------------------------------------------------------------
std::uint16_t StackWatch::lowest() const noexcept { return myEntered ? myLowest : 0U; }

// -----------------------------------------------------------------------------
std::uint16_t StackWatch::deepestBytes() const noexcept
{
    const auto used = static_cast<std::uint16_t>(myHigh - myLowest);
    return myEntered ? used : 0U;
}

// -----------------------------------------------------------------------------
std::uint16_t StackWatch::low() const noexcept { return myLow; }

// -----------------------------------------------------------------------------
std::uint16_t StackWatch::high() const noexcept { return myHigh; }

// -----------------------------------------------------------------------------
void StackWatch::clear() noexcept
{
    myEntered = false;
    myLowest  = NoMark;
}
} // namespace avrsim
