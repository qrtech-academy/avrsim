/**
 * @file Trace implementation details.
 */
#include <cstddef>
#include <cstdint>
#include <vector>

#include "avrsim/trace.hpp"

namespace avrsim
{
// -----------------------------------------------------------------------------
Trace::Trace() noexcept
    : myVisits{}
{}

// -----------------------------------------------------------------------------
Trace::~Trace() noexcept = default;

// -----------------------------------------------------------------------------
void Trace::enter(const std::uint64_t cycle) noexcept
{
    const Visit visit{cycle, cycle, 0U, false};
    myVisits.push_back(visit);
}

// -----------------------------------------------------------------------------
void Trace::leave(const std::uint64_t cycle) noexcept
{
    if (myVisits.empty() || myVisits.back().complete) { return; }

    auto& visit    = myVisits.back();
    visit.exit     = cycle;
    visit.cycles   = cycle - visit.entry;
    visit.complete = true;
}

// -----------------------------------------------------------------------------
const std::vector<Visit>& Trace::visits() const noexcept { return myVisits; }

// -----------------------------------------------------------------------------
std::size_t Trace::count() const noexcept { return myVisits.size(); }

// -----------------------------------------------------------------------------
std::uint64_t Trace::totalCycles() const noexcept
{
    std::uint64_t total{};

    for (const auto& visit : myVisits)
    {
        if (visit.complete) { total += visit.cycles; }
    }
    return total;
}

// -----------------------------------------------------------------------------
std::uint64_t Trace::shortest() const noexcept
{
    std::uint64_t best{};
    bool found{false};

    for (const auto& visit : myVisits)
    {
        if (!visit.complete) { continue; }
        if (!found || (visit.cycles < best)) { best = visit.cycles; }
        found = true;
    }
    return best;
}

// -----------------------------------------------------------------------------
std::uint64_t Trace::longest() const noexcept
{
    std::uint64_t worst{};

    for (const auto& visit : myVisits)
    {
        if (visit.complete && (visit.cycles > worst)) { worst = visit.cycles; }
    }
    return worst;
}

// -----------------------------------------------------------------------------
std::uint64_t Trace::gap(const std::size_t first, const std::size_t second) const noexcept
{
    const auto count = myVisits.size();
    if ((count <= first) || (count <= second)) { return 0U; }

    return myVisits[second].entry - myVisits[first].entry;
}

// -----------------------------------------------------------------------------
std::vector<std::size_t> Trace::callStarts() const noexcept
{
    constexpr std::size_t MinimumVisits{2U};
    constexpr std::uint64_t HalfDivisor{2U};

    std::vector<std::size_t> starts{};
    if (MinimumVisits > myVisits.size()) { return starts; }

    std::uint64_t largest{};

    for (std::size_t index{1U}; index < myVisits.size(); ++index)
    {
        const std::uint64_t between{myVisits[index].entry - myVisits[index - 1U].entry};
        if (between > largest) { largest = between; }
    }

    const std::uint64_t threshold{largest / HalfDivisor};
    starts.push_back(0U);

    for (std::size_t index{1U}; index < myVisits.size(); ++index)
    {
        const std::uint64_t between{myVisits[index].entry - myVisits[index - 1U].entry};
        if (threshold < between) { starts.push_back(index); }
    }
    return starts;
}

// -----------------------------------------------------------------------------
void Trace::clear() noexcept { myVisits.clear(); }

} // namespace avrsim
