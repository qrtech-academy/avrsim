/**
 * @file Pin log implementation.
 *
 *       Recorded history of one I/O pin.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace avrsim
{
/**
 * @brief One transition: when it happened, and what the pin became.
 */
struct Transition
{
    /** Cycle at which the pin changed. */
    std::uint64_t cycle{};

    /** Level the pin changed to. */
    bool level{};
};

/**
 * @brief Every transition of one pin, in order.
 *
 *        This class is non-copyable and non-movable.
 */
class PinLog final
{
public:
    /**
     * @brief Constructor.
     */
    PinLog() noexcept;

    /**
     * @brief Destructor.
     */
    ~PinLog() noexcept;

    /**
     * @brief Record a transition. Called by the simulator, not by a test.
     *
     * @param[in] cycle Cycle at which the pin changed.
     * @param[in] level Level the pin changed to.
     */
    void record(std::uint64_t cycle, bool level) noexcept;

    /**
     * @brief Every transition, oldest first.
     *
     * @return The transitions.
     */
    [[nodiscard]] const std::vector<Transition>& transitions() const noexcept;

    /**
     * @brief How many times the pin changed.
     *
     * @return The number of transitions.
     */
    [[nodiscard]] std::size_t count() const noexcept;

    /**
     * @brief The pin's current level.
     *
     * @return The level after the last transition, or false if it never changed.
     */
    [[nodiscard]] bool level() const noexcept;

    /**
     * @brief Cycles between two recorded transitions.
     *
     * @param[in] first  Index of the earlier transition.
     * @param[in] second Index of the later transition.
     *
     * @return The gap in cycles, or 0 if either index is out of range.
     */
    [[nodiscard]] std::uint64_t gap(std::size_t first, std::size_t second) const noexcept;

    /**
     * @brief Forget everything recorded so far.
     */
    void clear() noexcept;

    PinLog(const PinLog&)            = delete; // No copy constructor.
    PinLog(PinLog&&)                 = delete; // No move constructor.
    PinLog& operator=(const PinLog&) = delete; // No copy assignment.
    PinLog& operator=(PinLog&&)      = delete; // No move assignment.

private:
    /** Every transition, oldest first. */
    std::vector<Transition> myTransitions;
};
} // namespace avrsim
