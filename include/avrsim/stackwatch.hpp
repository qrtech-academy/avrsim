/**
 * @file Stack watch implementation.
 */
#pragma once

#include <cstdint>

namespace avrsim
{
/**
 * @brief The deepest the stack pointer went while it was inside one range of addresses.
 *
 *        This class is non-copyable and non-movable.
 */
class StackWatch final
{
public:
    /**
     * @brief Constructor.
     *
     * @param[in] low Lowest address of the range, inclusive.
     * @param[in] high Highest address of the range, inclusive.
     */
    StackWatch(std::uint16_t low, std::uint16_t high) noexcept;

    /**
     * @brief Destructor.
     */
    ~StackWatch() noexcept;

    /**
     * @brief Record the stack pointer. Called by the simulator, not by a test.
     *
     * @param[in] pointer The stack pointer now.
     */
    void sample(std::uint16_t pointer) noexcept;

    /**
     * @brief Whether the stack pointer was ever inside the range.
     *
     * @return True if it was.
     */
    [[nodiscard]] bool entered() const noexcept;

    /**
     * @brief The lowest stack pointer seen inside the range.
     *
     * @return The address, or 0 if the range was never entered.
     */
    [[nodiscard]] std::uint16_t lowest() const noexcept;

    /**
     * @brief How many bytes of the range were in use at the deepest instant.
     *
     *        Measured from the top of the range, because a stack grows downwards. Note that this
     *        counts the bytes *used*: the stack pointer sits one below the last byte written, so
     *        this is `high - lowest`, not `high - lowest + 1`.
     *
     * @return The byte count, or 0 if the range was never entered.
     */
    [[nodiscard]] std::uint16_t deepestBytes() const noexcept;

    /**
     * @brief Lowest address of the range.
     *
     * @return The address.
     */
    [[nodiscard]] std::uint16_t low() const noexcept;

    /**
     * @brief Highest address of the range.
     *
     * @return The address.
     */
    [[nodiscard]] std::uint16_t high() const noexcept;

    /**
     * @brief Forget what was recorded, keeping the range.
     */
    void clear() noexcept;

    StackWatch()                             = delete; // No default constructor.
    StackWatch(const StackWatch&)            = delete; // No copy constructor.
    StackWatch(StackWatch&&)                 = delete; // No move constructor.
    StackWatch& operator=(const StackWatch&) = delete; // No copy assignment.
    StackWatch& operator=(StackWatch&&)      = delete; // No move assignment.

private:
    /** Lowest address of the range, inclusive. */
    std::uint16_t myLow;

    /** Highest address of the range, inclusive. */
    std::uint16_t myHigh;

    /** Lowest stack pointer seen inside it. */
    std::uint16_t myLowest;

    /** Whether the stack pointer was ever inside it. */
    bool myEntered;
};
} // namespace avrsim
