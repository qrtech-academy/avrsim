/**
 * @file Trace implementation.
 *
 *       Recorded history of one function.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace avrsim
{
/**
 * @brief One pass through a function: when it was entered, when control left it, and what that
 *        cost.
 */
struct Visit
{
    /** Cycle at which the program counter entered the function. */
    std::uint64_t entry{};

    /** Cycle at which it left. Equal to `entry` while the visit is still open. */
    std::uint64_t exit{};

    /** Cycles spent inside, which is `exit - entry`. Zero while the visit is still open. */
    std::uint64_t cycles{};

    /** False if the run ended before control left the function. */
    bool complete{};
};

/**
 * @brief Every visit to one function, in order.
 *
 *        This class is non-copyable and non-movable.
 */
class Trace final
{
public:
    /**
     * @brief Constructor.
     */
    Trace() noexcept;

    /**
     * @brief Destructor.
     */
    ~Trace() noexcept;

    /**
     * @brief Open a visit. Called by the simulator, not by a test.
     *
     * @param[in] cycle Cycle at which the function was entered.
     */
    void enter(std::uint64_t cycle) noexcept;

    /**
     * @brief Close the open visit. Called by the simulator, not by a test.
     *
     * @param[in] cycle Cycle at which control left the function.
     */
    void leave(std::uint64_t cycle) noexcept;

    /**
     * @brief Every visit, oldest first.
     *
     * @return The visits.
     */
    [[nodiscard]] const std::vector<Visit>& visits() const noexcept;

    /**
     * @brief How many times the function was entered.
     *
     * @return The number of visits, open ones included.
     */
    [[nodiscard]] std::size_t count() const noexcept;

    /**
     * @brief Cycles spent inside the function, over every completed visit.
     *
     *        An open visit contributes nothing, because a cost that is still being paid is not
     *        a measurement. This is the numerator of L08's overhead fraction.
     *
     * @return The total.
     */
    [[nodiscard]] std::uint64_t totalCycles() const noexcept;

    /**
     * @brief The cheapest completed visit.
     *
     * @return Its cost, or 0 if no visit completed.
     */
    [[nodiscard]] std::uint64_t shortest() const noexcept;

    /**
     * @brief The most expensive completed visit.
     *
     *        Worth more than the average, and the two differing is usually the interesting part:
     *        a switch that costs more on some passes than on others is a switch that took an
     *        interrupt, and that is a term the arithmetic did not have.
     *
     * @return Its cost, or 0 if no visit completed.
     */
    [[nodiscard]] std::uint64_t longest() const noexcept;

    /**
     * @brief Cycles between two entries.
     *
     *        Between *entries*, not between an exit and the next entry, because what a tick
     *        period means is how often the handler starts rather than how long the gaps between
     *        handlers are.
     *
     * @param[in] first  Index of the earlier visit.
     * @param[in] second Index of the later visit.
     *
     * @return The gap in cycles, or 0 if either index is out of range.
     */
    [[nodiscard]] std::uint64_t gap(std::size_t first, std::size_t second) const noexcept;

    /**
     * @brief The indices of the visits at which a fresh call to this function begins.
     *
     *        A function that calls another is recorded as several visits per call, so consecutive
     *        gaps alternate short and long. This separates them: a gap longer than half the
     *        largest is a fresh call, and a leaf function reports every index. It cannot tell
     *        apart calls made by different tasks, nor a call lasting longer than half its own
     *        period.
     *
     * @return The indices, oldest first, or empty if there are fewer than two visits.
     */
    [[nodiscard]] std::vector<std::size_t> callStarts() const noexcept;

    /**
     * @brief Forget everything recorded so far.
     */
    void clear() noexcept;

    Trace(const Trace&)            = delete; // No copy constructor.
    Trace(Trace&&)                 = delete; // No move constructor.
    Trace& operator=(const Trace&) = delete; // No copy assignment.
    Trace& operator=(Trace&&)      = delete; // No move assignment.

private:
    /** Every visit, oldest first. */
    std::vector<Visit> myVisits;
};
} // namespace avrsim
