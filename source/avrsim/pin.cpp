/**
 * @file Pin log implementation details.
 */
#include <cstddef>
#include <cstdint>
#include <vector>

#include "avrsim/pin.hpp"

namespace avrsim
{
// -----------------------------------------------------------------------------
PinLog::PinLog() noexcept
    : myTransitions{}
{}

// -----------------------------------------------------------------------------
PinLog::~PinLog() noexcept = default;

// -----------------------------------------------------------------------------
void PinLog::record(const std::uint64_t cycle, const bool level) noexcept
{
    const Transition transition{cycle, level};
    myTransitions.push_back(transition);
}

// -----------------------------------------------------------------------------
const std::vector<Transition>& PinLog::transitions() const noexcept { return myTransitions; }

// -----------------------------------------------------------------------------
std::size_t PinLog::count() const noexcept { return myTransitions.size(); }

// -----------------------------------------------------------------------------
bool PinLog::level() const noexcept
{
    return myTransitions.empty() ? false : myTransitions.back().level;
}

// -----------------------------------------------------------------------------
std::uint64_t PinLog::gap(const std::size_t first, const std::size_t second) const noexcept
{
    const auto count = myTransitions.size();
    if ((count <= first) || (count <= second)) { return 0U; }

    return myTransitions[second].cycle - myTransitions[first].cycle;
}

// -----------------------------------------------------------------------------
void PinLog::clear() noexcept { myTransitions.clear(); }

} // namespace avrsim
