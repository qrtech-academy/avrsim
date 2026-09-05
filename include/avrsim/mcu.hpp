/**
 * @file Mcu implementation.
 *
 *       Simulated ATmega328P, driven from a test.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "avrsim/pin.hpp"
#include "avrsim/stackwatch.hpp"
#include "avrsim/trace.hpp"

namespace avrsim
{
/**
 * @brief Result of running one subroutine to completion.
 */
struct Call
{
    /** True if the subroutine returned, false if it ran past the cycle budget. */
    bool returned{};

    /** Cycles consumed between entering the subroutine and its ret. */
    std::uint64_t cycles{};
};

/**
 * @brief Simulated ATmega328P with one program loaded into it.
 *
 *        This class is non-copyable and non-movable.
 */
class Mcu final
{
public:
    /**
     * @brief Load a program and reset the machine.
     *
     *        Two formats are accepted, chosen by extension. A .hex is avra's output, and the
     *        map file it wrote beside it supplies the symbols, so the two must share a stem:
     *        drivers.hex is read together with drivers.map. Anything else is treated as an ELF
     *        and read whole, which is what an avr-gcc build produces.
     *
     * @param[in] imagePath Path to a .hex or .elf built for the ATmega328P.
     * @param[in] clockHz Clock frequency in Hz (default = 16 MHz, the Arduino Uno's).
     */
    explicit Mcu(const std::string& imagePath, std::uint32_t clockHz = DefaultClockHz) noexcept;

    /**
     * @brief Destructor.
     */
    ~Mcu() noexcept;

    /**
     * @brief Whether the program defines a global symbol with this name.
     *
     *        A test asks this before calling, so that a subroutine the reader has not written
     *        yet fails with a sentence rather than by jumping to address zero.
     *
     * @param[in] symbol Symbol name, e.g. "led_init".
     *
     * @return True if the symbol exists.
     */
    [[nodiscard]] bool has(const std::string& symbol) const noexcept;

    /**
     * @brief Byte address in program memory of a global symbol.
     *
     * @param[in] symbol Symbol name.
     *
     * @return The address, or NoAddress if the symbol is not defined.
     */
    [[nodiscard]] std::uint32_t address(const std::string& symbol) const noexcept;

    /**
     * @brief Read one of the 32 general-purpose registers.
     *
     * @param[in] index Register number, 0 to 31.
     *
     * @return The register's contents.
     */
    [[nodiscard]] std::uint8_t reg(std::uint8_t index) const noexcept;

    /**
     * @brief Write one of the 32 general-purpose registers.
     *
     * @param[in] index Register number, 0 to 31.
     * @param[in] value Value to write.
     */
    void setReg(std::uint8_t index, std::uint8_t value) noexcept;

    /**
     * @brief Read a register pair as one 16-bit value, low byte first.
     *
     *        regPair(24) is r25:r24, which is where this course's subroutines take a pointer.
     *
     * @param[in] low Number of the low register of the pair.
     *
     * @return The pair's contents.
     */
    [[nodiscard]] std::uint16_t regPair(std::uint8_t low) const noexcept;

    /**
     * @brief Write a register pair as one 16-bit value, low byte first.
     *
     * @param[in] low   Number of the low register of the pair.
     * @param[in] value Value to write.
     */
    void setRegPair(std::uint8_t low, std::uint16_t value) noexcept;

    /**
     * @brief Read one byte of the data space.
     *
     *        One flat space, exactly as the device sees it: 0x0000 to 0x001F is the register
     *        file, 0x0020 to 0x005F the I/O registers, and 0x0100 upwards the SRAM. So
     *        data(0x25) is PORTB and data(0x0200) is wherever a test put a struct.
     *
     * @param[in] addr Data space address.
     *
     * @return The byte at that address, or 0 if the address is out of range.
     */
    [[nodiscard]] std::uint8_t data(std::uint16_t addr) const noexcept;

    /**
     * @brief Write one byte of the data space.
     *
     * @param[in] addr  Data space address.
     * @param[in] value Value to write.
     */
    void setData(std::uint16_t addr, std::uint8_t value) noexcept;

    /**
     * @brief Read the status register.
     *
     *        Assembled from the core's own flag storage rather than read out of the data space.
     *        The simulator keeps the eight flags as eight separate bytes and only materialises
     *        the packed byte at 0x5F when the program reads it, so `data(0x5F)` can be stale.
     *        Use this, and setSreg(), rather than reaching for the address.
     *
     * @return SREG, packed as the device packs it: bit 0 is C and bit 7 is I.
     */
    [[nodiscard]] std::uint8_t sreg() const noexcept;

    /**
     * @brief Write the status register.
     *
     *        Unpacked into the core's flag storage, for the same reason. Writing 0x5F through
     *        setData() does not change any flag the processor will act on, which is a quiet way
     *        for a test to set up a condition that never actually existed.
     *
     * @param[in] value The byte to unpack.
     */
    void setSreg(std::uint8_t value) noexcept;

    /**
     * @brief Whether the global interrupt enable is set.
     *
     * @return True if SREG's I bit is set.
     */
    [[nodiscard]] bool interruptsEnabled() const noexcept;

    /**
     * @brief The stack pointer now.
     *
     * @return SPH:SPL, as a data space address.
     */
    [[nodiscard]] std::uint16_t stackPtr() const noexcept;

    /**
     * @brief The lowest the stack pointer has been since the last resetStackWatermark().
     *
     *        Sampled after every instruction, so it catches the deepest moment rather than the
     *        state at some convenient boundary. Subtract it from RAMEND to get how many bytes of
     *        stack were in use at the worst instant, which is the number that decides whether a
     *        stack and a set of variables can share 2 KB.
     *
     * @return The low-water mark, or NoStackMark if nothing has run yet.
     */
    [[nodiscard]] std::uint16_t lowestStackPtr() const noexcept;

    /**
     * @brief Forget the low-water mark, so a later measurement starts clean.
     */
    void resetStackWatermark() noexcept;

    /**
     * @brief Cycles elapsed since the machine was created.
     *
     * @return The cycle counter.
     */
    [[nodiscard]] std::uint64_t cycle() const noexcept;

    /**
     * @brief The clock this machine was created with.
     *
     * @return Clock frequency in Hz.
     */
    [[nodiscard]] std::uint32_t clockHz() const noexcept;

    /**
     * @brief Call one subroutine and run until it returns.
     *
     *        Set up the argument registers with setReg() and setRegPair() first, then read the
     *        result out of reg(24) or out of memory afterwards. The stack pointer is reset to
     *        RAMEND on every call, so an unbalanced subroutine cannot poison the next test.
     *
     * @param[in] symbol Name of the subroutine to call.
     * @param[in] cycleBudget Give up after this many cycles (default = `DefaultBudget`).
     *
     * @return Whether it returned, and how many cycles it took.
     */
    Call call(const std::string& symbol, std::uint64_t cycleBudget = DefaultBudget) noexcept;

    /**
     * @brief Run the loaded program from its reset vector for a while.
     *
     *        Used by the tests that cannot call one subroutine in isolation because what they
     *        are testing is an interrupt, which by definition arrives on its own.
     *
     * @param[in] cycles How long to run for.
     *
     * @return Cycles actually consumed, which may exceed the request by one instruction.
     */
    std::uint64_t run(std::uint64_t cycles) noexcept;

    /**
     * @brief Reset the machine to its power-on state, keeping the program loaded.
     */
    void reset() noexcept;

    /**
     * @brief Start recording every transition of one pin.
     *
     *        Call before running. The returned log lives as long as this Mcu does, and is what
     *        a test uses to say "the LED changed twice, and the second time was 1000 cycles
     *        after the first".
     *
     * @param[in] port Port letter, 'B', 'C' or 'D'.
     * @param[in] bit Bit number within the port, 0 to 7.
     *
     * @return The log for that pin.
     */
    [[nodiscard]] PinLog& watch(char port, std::uint8_t bit) noexcept;

    /**
     * @brief Drive a pin from outside, the way a button or a sensor would.
     *
     * @param[in] port Port letter, 'B', 'C' or 'D'.
     * @param[in] bit Bit number within the port, 0 to 7.
     * @param[in] level Level to drive.
     */
    void drive(char port, std::uint8_t bit, bool level) noexcept;

    /**
     * @brief Start recording every visit to one function.
     *
     *        Call before running. The returned log lives as long as this Mcu does, and is what a
     *        test uses to say "the switch happened 412 times and cost 66 cycles each time".
     *
     * @param[in] symbol Name of the function to record, e.g. "switch_context".
     *
     * @return The log for that function. Empty and never written to if the symbol is not defined.
     */
    [[nodiscard]] Trace& trace(const std::string& symbol) noexcept;

    /**
     * @brief Start recording how deep the stack pointer goes inside one range of addresses.
     *
     *        Call before running. This is what a per-task watermark needs: four tasks have four
     *        stacks in the same 2048 bytes, and lowestStackPtr() answers one question where
     *        there are four.
     *
     * @param[in] low Lowest address of the range, inclusive.
     * @param[in] high Highest address of the range, inclusive.
     *
     * @return The watch for that range.
     */
    [[nodiscard]] StackWatch& watchStack(std::uint16_t low, std::uint16_t high) noexcept;

    /**
     * @brief Run until control first reaches a symbol, or until the budget runs out.
     *
     *        Used where the question is *when* rather than *how long*: how many cycles after a
     *        task asked for a delay does it run again, how long a high-priority task waits on a
     *        resource a low-priority task is holding.
     *
     * @param[in] symbol      Name of the symbol to run to.
     * @param[in] cycleBudget Give up after this many cycles (default = `DefaultBudget`).
     *
     * @return Whether the symbol was reached, and the cycles consumed getting there.
     */
    Call runUntil(const std::string& symbol, std::uint64_t cycleBudget = DefaultBudget) noexcept;

    /**
     * @brief How many bytes of a painted region are still holding the paint.
     *
     * @param[in] low        Lowest address of the region, inclusive.
     * @param[in] high       Highest address of the region, inclusive.
     * @param[in] paintByte  The value the region was painted with.
     *
     * @return The number of bytes at the bottom of the region that still hold the paint.
     */
    [[nodiscard]] std::uint16_t untouchedBytes(std::uint16_t low, std::uint16_t high,
                                               std::uint8_t paintByte) const noexcept;

    /** Returned by address() for a symbol the program does not define. */
    static constexpr std::uint32_t NoAddress{0xFFFFFFFFU};

    /** Returned by lowestStackPtr() when nothing has run yet, being no address the stack can
        hold. */
    static constexpr std::uint16_t NoStackMark{0xFFFFU};

    /** The Arduino Uno's clock, and this course's assumption everywhere. */
    static constexpr std::uint32_t DefaultClockHz{16000000U};

    /** Cycles a call may consume before it is declared not to have returned. */
    static constexpr std::uint64_t DefaultBudget{100000U};

    Mcu()                      = delete; // No default constructor.
    Mcu(const Mcu&)            = delete; // No copy constructor.
    Mcu(Mcu&&)                 = delete; // No move constructor.
    Mcu& operator=(const Mcu&) = delete; // No copy assignment.
    Mcu& operator=(Mcu&&)      = delete; // No move assignment.

private:
    /** MCU implementation details. */
    struct Impl;

    /** The machine, its firmware, and its pin logs. */
    std::unique_ptr<Impl> myImpl;

    /** Clock frequency in Hz. */
    std::uint32_t myClockHz;
};
} // namespace avrsim
