/**
 * @file Mcu implementation details.
 */
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include <simavr/avr_ioport.h>
#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/sim_hex.h>
#include <simavr/sim_io.h>
#include <simavr/sim_irq.h>
} // extern "C"

#include "avrsim/mcu.hpp"

namespace avrsim
{
namespace
{
/** Pin hook structure. */
struct PinHook;

/** Pin key pair. */
using PinKey = std::pair<char, std::uint8_t>;

/** Pin hook pointer. */
using PinHookPtr = std::unique_ptr<PinHook>;

/**
 * @brief Pin hook structure.
 *
 *        What a pin's notification callback needs in order to do its job.
 */
struct PinHook
{
    /** Where to record the transition. */
    PinLog* log{};

    /** The core, read only for its cycle counter. */
    avr_t* avr{};
};

/** Bits in a byte, which is also the shift between the halves of a 16-bit value. */
constexpr std::uint8_t BitWidth{8U};

/** The low byte of a wider value. */
constexpr std::uint32_t ByteMask{0xFFU};

/** General-purpose registers, at the bottom of the data space. */
constexpr std::uint8_t RegisterCount{32U};

/** Bytes per word of program memory, which is what a word address is counted in. */
constexpr std::uint32_t BytesPerWord{2U};

/** Base of the addresses an avra map file records, and what std::stoul takes them in. */
constexpr int HexRadix{16};

/** Data space address of SPL. */
constexpr std::uint16_t SplAddress{0x5DU};

/** Data space address of SPH, one byte above SPL. */
constexpr std::uint16_t SphAddress{0x5EU};

/** Data space address of SREG. */
constexpr std::uint16_t SregAddress{0x5FU};

/** Bit 7 of SREG: the global interrupt enable. */
constexpr std::uint8_t GlobalInterruptBit{7U};

/** The device every program in this course is built for. */
constexpr const char* DeviceName{"atmega328p"};

// -----------------------------------------------------------------------------
[[noreturn]] void fail(const std::string& message) noexcept
{
    // stdout is flushed first for the same reason quietLogger() flushes it: the test framework
    // writes there and this writes to stderr, and unflushed output interleaves mid-line.
    std::fflush(stdout);
    std::fprintf(stderr, "avrsim: %s\n", message.c_str());
    std::fflush(stderr);
    std::terminate();
}

// -----------------------------------------------------------------------------
std::map<std::string, std::uint32_t> readMapFile(const std::string& path) noexcept
{
    std::ifstream file{path};
    if (!file) { fail("cannot read map file " + path); }

    std::map<std::string, std::uint32_t> symbols{};
    std::string line{};

    while (std::getline(file, line))
    {
        std::istringstream stream{line};
        std::string name{}, type{}, addrText{};

        if (!(stream >> name >> type >> addrText)) { continue; }
        if ("L" != type) { continue; }
        const auto addr = static_cast<std::uint32_t>(std::stoul(addrText, nullptr, HexRadix));
        symbols[name]   = addr * BytesPerWord;
    }
    return symbols;
}

// -----------------------------------------------------------------------------
void quietLogger(avr_t* const avr, const int level, const char* const format, va_list args) noexcept
{
    (void)(avr);
    if (LOG_ERROR < level) { return; }

    // Errors are kept: "Invalid read address ... out of ram" diagnoses a driver that walked an
    // array with the wrong stride. stdout is flushed first so the two streams do not interleave.
    std::fflush(stdout);
    std::fprintf(stderr, "avrsim: ");
    std::vfprintf(stderr, format, args);
    std::fflush(stderr);
}

// -----------------------------------------------------------------------------
void onPinChanged(avr_irq_t* const irq, const std::uint32_t value, void* const param) noexcept
{
    (void)(irq);
    auto* hook = static_cast<PinHook*>(param);
    if (nullptr == hook) { return; }

    const auto cycle = static_cast<std::uint64_t>(hook->avr->cycle);
    const auto level = 0U != value;
    hook->log->record(cycle, level);
}
} // namespace

/**
 * @brief Everything whose declaration needs a simavr type.
 */
struct Mcu::Impl
{
    /** The simulated machine. */
    avr_t* avr{};

    /** The loaded program, including its symbol table. */
    elf_firmware_t firmware{};

    /** Flash byte address used as the return address of a called subroutine. */
    std::uint32_t trap{};

    /** Pin logs, keyed by (port, bit). */
    std::map<PinKey, PinLog> pins{};

    /** One hook per watched pin, owned here so it outlives the registration. */
    std::vector<PinHookPtr> hooks{};

    /** Lowest stack pointer seen since the last reset of the mark. */
    std::uint16_t lowestStack{NoStackMark};

    /**
     * @brief Where one traced function begins and ends in flash, and its log.
     */
    struct Traced
    {
        /** First flash byte address of the function. */
        std::uint32_t begin{};

        /** One past its last flash byte address, derived from the next symbol. */
        std::uint32_t end{};

        /** Whether the program counter was inside it after the previous instruction. */
        bool inside{};

        /** Every visit. */
        Trace log{};
    };

    /** Every traced function, keyed by symbol. A std::map because trace() hands out references
        into it, and those have to stay valid when another function is traced later. */
    std::map<std::string, Traced> traced{};

    /** Watched stack regions, in registration order. A std::deque because watchStack() hands
        out references into it and a vector would move them on the next push. */
    std::deque<StackWatch> stacks{};

    /** Every flash symbol address in the program, sorted. Used to derive a function's extent,
        because the AVR symbol table records where a symbol starts and not how long it is. */
    std::vector<std::uint32_t> flashSymbols{};

    /** Label to byte address, from an avra map file. Empty when an ELF was loaded, which is
        what address() dispatches on: an ELF carries its own symbol table and a hex does not. */
    std::map<std::string, std::uint32_t> symbols{};

    /** One past the last flash byte the loaded program occupies. Taken from the ELF's section
        sizes or from the highest hex chunk, whichever format was given. */
    std::uint32_t codeEnd{};

    [[nodiscard]] std::uint32_t extentEnd(const std::uint32_t begin) const noexcept
    {
        const auto next = std::upper_bound(flashSymbols.begin(), flashSymbols.end(), begin);
        if (flashSymbols.end() != next) { return *next; }
        return (0U == codeEnd) ? (avr->flashend + 1U) : codeEnd;
    }

    void sampleTraces() noexcept
    {
        // Edge-triggered on the program counter crossing the function boundary, which needs
        // neither a frame to track nor a return address to match.
        const auto pc = avr->pc;

        for (auto& entry : traced)
        {
            auto& item = entry.second;
            const bool inside{(pc >= item.begin) && (pc < item.end)};
            if (inside && !item.inside) { item.log.enter(avr->cycle); }
            else if (!inside && item.inside) { item.log.leave(avr->cycle); }
            item.inside = inside;
        }
    }

    void sampleStack() noexcept
    {
        // Sampled rather than hooked onto SPL and SPH: an interrupt's push does not go
        // through them in a way a watch would see.
        const std::uint16_t low{avr->data[SplAddress]};
        const std::uint16_t high{avr->data[SphAddress]};
        const auto ptr = static_cast<std::uint16_t>(low | (high << BitWidth));
        if (ptr < lowestStack) { lowestStack = ptr; }

        for (auto& watch : stacks)
        {
            watch.sample(ptr);
        }
    }
};

// -----------------------------------------------------------------------------
Mcu::Mcu(const std::string& imagePath, const std::uint32_t clockHz) noexcept
    : myImpl{std::make_unique<Impl>()}
    , myClockHz{clockHz}
{
    avr_global_logger_set(quietLogger);

    // Two image formats, because the courses assemble with two toolchains: avra writes a hex
    // with a map beside it, avr-gcc writes an ELF. Nothing below here can tell which it got.
    const auto dot = imagePath.find_last_of('.');
    const std::string extension{(std::string::npos == dot) ? "" : imagePath.substr(dot)};

    myImpl->avr = avr_make_mcu_by_name(DeviceName);
    if (nullptr == myImpl->avr) { fail(std::string{"unknown device "} + DeviceName); }
    avr_init(myImpl->avr);
    myImpl->avr->frequency = clockHz;

    if (".hex" == extension)
    {
        // The map sits beside the hex and shares its stem: drivers.hex, drivers.map.
        myImpl->symbols = readMapFile(imagePath.substr(0U, dot) + ".map");

        // Chunks, not one blob: a program with a vector table is not contiguous, and the
        // single-blob call would load only the first run of it.
        ihex_chunk_p chunks{nullptr};
        const int count{read_ihex_chunks(imagePath.c_str(), &chunks)};
        if (0 >= count) { fail("cannot read hex file " + imagePath); }

        for (int index{0U}; index < count; ++index)
        {
            const auto& chunk = chunks[index];
            const std::uint32_t end{chunk.baseaddr + chunk.size};
            const std::uint32_t flashSize{myImpl->avr->flashend + 1U};
            if (flashSize < end) { fail("program does not fit in flash: " + imagePath); }

            std::memcpy(&myImpl->avr->flash[chunk.baseaddr], chunk.data, chunk.size);
            if (end > myImpl->codeEnd) { myImpl->codeEnd = end; }
        }
        free_ihex_chunks(chunks);
        myImpl->avr->codeend = myImpl->codeEnd;

        // The same list trace() needs, out of the map rather than out of a symbol table. Every
        // address in it is already in flash, so there is no data space to filter out.
        for (const auto& entry : myImpl->symbols)
        {
            myImpl->flashSymbols.push_back(entry.second);
        }
    }
    else
    {
        if (0 != elf_read_firmware(imagePath.c_str(), &myImpl->firmware))
        {
            fail("cannot read ELF file " + imagePath);
        }
        myImpl->firmware.frequency = clockHz;
        avr_load_firmware(myImpl->avr, &myImpl->firmware);
        myImpl->codeEnd = myImpl->firmware.flashbase + myImpl->firmware.flashsize;

        // Every flash symbol address, sorted, so trace() can derive where a function ends. Data
        // symbols are filtered out by the flash bound, or one would cut a function short.
        for (std::uint32_t index{}; index < myImpl->firmware.symbolcount; ++index)
        {
            const auto* entry = myImpl->firmware.symbol[index];
            if ((nullptr == entry) || (myImpl->avr->flashend < entry->addr)) { continue; }
            myImpl->flashSymbols.push_back(entry->addr);
        }
    }

    std::sort(myImpl->flashSymbols.begin(), myImpl->flashSymbols.end());
    myImpl->flashSymbols.erase(
        std::unique(myImpl->flashSymbols.begin(), myImpl->flashSymbols.end()),
        myImpl->flashSymbols.end());

    // The return address a called subroutine comes back to: the first even byte past the end
    // of the program, so nothing the reader wrote can be sitting there.
    const std::uint32_t alignment{BytesPerWord - 1U};
    myImpl->trap = (myImpl->codeEnd + 1U) & ~alignment;
}

// -----------------------------------------------------------------------------
Mcu::~Mcu() noexcept
{
    // avr_terminate releases what the core allocated for itself. The avr_t and the symbol
    // table are left behind: simavr offers no paired free, and this process exits after one run.
    if (nullptr != myImpl->avr) { avr_terminate(myImpl->avr); }
}

// -----------------------------------------------------------------------------
bool Mcu::has(const std::string& symbol) const noexcept { return NoAddress != address(symbol); }

// -----------------------------------------------------------------------------
std::uint32_t Mcu::address(const std::string& symbol) const noexcept
{
    // A hex was loaded exactly when the map has entries and an ELF exactly when it has none,
    // so which container is consulted is the whole of the dispatch.
    if (!myImpl->symbols.empty())
    {
        const auto found = myImpl->symbols.find(symbol);
        return (myImpl->symbols.end() == found) ? NoAddress : found->second;
    }

    for (std::uint32_t index{}; index < myImpl->firmware.symbolcount; ++index)
    {
        const auto* entry = myImpl->firmware.symbol[index];
        if (symbol == entry->symbol) { return entry->addr; }
    }
    return NoAddress;
}

// -----------------------------------------------------------------------------
std::uint8_t Mcu::reg(const std::uint8_t index) const noexcept
{
    const bool valid{RegisterCount > index};
    return valid ? myImpl->avr->data[index] : 0U;
}

// -----------------------------------------------------------------------------
void Mcu::setReg(const std::uint8_t index, const std::uint8_t value) noexcept
{
    const bool valid{RegisterCount > index};
    if (valid) { myImpl->avr->data[index] = value; }
}

// -----------------------------------------------------------------------------
std::uint16_t Mcu::regPair(const std::uint8_t low) const noexcept
{
    const std::uint16_t lowByte{reg(low)};
    const std::uint16_t highByte{reg(low + 1U)};
    return static_cast<std::uint16_t>(lowByte | (highByte << BitWidth));
}

// -----------------------------------------------------------------------------
void Mcu::setRegPair(const std::uint8_t low, const std::uint16_t value) noexcept
{
    const auto lowByte  = static_cast<std::uint8_t>(value & ByteMask);
    const auto highByte = static_cast<std::uint8_t>(value >> BitWidth);

    setReg(low, lowByte);
    setReg(low + 1U, highByte);
}

// -----------------------------------------------------------------------------
std::uint8_t Mcu::data(const std::uint16_t addr) const noexcept
{
    const bool inRange{myImpl->avr->ramend >= addr};
    return inRange ? myImpl->avr->data[addr] : 0U;
}

// -----------------------------------------------------------------------------
void Mcu::setData(const std::uint16_t addr, const std::uint8_t value) noexcept
{
    const bool inRange{myImpl->avr->ramend >= addr};
    if (inRange) { myImpl->avr->data[addr] = value; }
}

// -----------------------------------------------------------------------------
std::uint8_t Mcu::sreg() const noexcept
{
    // One byte per flag in the core, packed here the way the device packs it.
    std::uint8_t packed{};

    for (std::uint8_t bit{}; bit < BitWidth; ++bit)
    {
        const bool set{0U != myImpl->avr->sreg[bit]};
        const auto mask = static_cast<std::uint8_t>(1U << bit);
        if (set) { packed |= mask; }
    }
    return packed;
}

// -----------------------------------------------------------------------------
void Mcu::setSreg(const std::uint8_t value) noexcept
{
    for (std::uint8_t bit{}; bit < BitWidth; ++bit)
    {
        const auto flag        = static_cast<std::uint8_t>((value >> bit) & 1U);
        myImpl->avr->sreg[bit] = flag;
    }
    // Keep the data space view consistent, so a program that reads 0x5F sees the same thing.
    myImpl->avr->data[SregAddress] = value;
}

// -----------------------------------------------------------------------------
bool Mcu::interruptsEnabled() const noexcept { return 0U != myImpl->avr->sreg[GlobalInterruptBit]; }

// -----------------------------------------------------------------------------
std::uint16_t Mcu::stackPtr() const noexcept
{
    const std::uint16_t low{data(SplAddress)};
    const std::uint16_t high{data(SphAddress)};
    return static_cast<std::uint16_t>(low | (high << BitWidth));
}

// -----------------------------------------------------------------------------
std::uint16_t Mcu::lowestStackPtr() const noexcept { return myImpl->lowestStack; }

// -----------------------------------------------------------------------------
void Mcu::resetStackWatermark() noexcept { myImpl->lowestStack = NoStackMark; }

// -----------------------------------------------------------------------------
std::uint64_t Mcu::cycle() const noexcept { return static_cast<std::uint64_t>(myImpl->avr->cycle); }

// -----------------------------------------------------------------------------
std::uint32_t Mcu::clockHz() const noexcept { return myClockHz; }

// -----------------------------------------------------------------------------
Call Mcu::call(const std::string& symbol, const std::uint64_t cycleBudget) noexcept
{
    const std::uint32_t entry{address(symbol)};
    if (NoAddress == entry) { return {false, 0U}; }

    avr_t* avr{myImpl->avr};

    // Reset the stack to RAMEND on every call, so a subroutine that leaves it unbalanced
    // cannot poison the next test with a failure that looks like it belongs somewhere else.
    std::uint16_t stack{avr->ramend};

    // Push the trap address exactly as rcall would: a word address, least significant byte
    // first, the stack growing downwards. address_size rather than a literal 2, for larger parts.
    const std::uint32_t returnWord{myImpl->trap / BytesPerWord};

    for (std::uint8_t byte{}; byte < avr->address_size; ++byte)
    {
        const auto shift   = static_cast<std::uint8_t>(BitWidth * byte);
        const auto part    = static_cast<std::uint8_t>((returnWord >> shift) & ByteMask);
        avr->data[stack--] = part;
    }

    const auto stackLow  = static_cast<std::uint8_t>(stack & ByteMask);
    const auto stackHigh = static_cast<std::uint8_t>(stack >> BitWidth);

    avr->data[SplAddress] = stackLow;
    avr->data[SphAddress] = stackHigh;

    avr->pc    = entry;
    avr->state = cpu_Running;

    // Bounded by instructions as well as by cycles: a subroutine that writes rubbish into SPL
    // and SPH can leave the core no longer counting cycles, and this loop would never end.
    std::uint64_t executed{};
    const std::uint64_t start{cycle()};

    while ((cycleBudget > (cycle() - start)) && (cycleBudget > executed))
    {
        if (myImpl->trap == avr->pc) { return {true, cycle() - start}; }
        if ((cpu_Running != avr->state) && (cpu_Sleeping != avr->state)) { break; }

        avr_run(avr);
        myImpl->sampleStack();
        myImpl->sampleTraces();
        ++executed;
    }
    return {false, cycle() - start};
}

// -----------------------------------------------------------------------------
std::uint64_t Mcu::run(const std::uint64_t cycles) noexcept
{
    avr_t* avr{myImpl->avr};
    avr->state = cpu_Running;

    // Bounded by instructions as well, for the same reason as call(): a core that has stopped
    // advancing its cycle counter would otherwise keep this loop running for ever.
    std::uint64_t executed{};
    const std::uint64_t start{cycle()};

    while ((cycles > (cycle() - start)) && (cycles > executed))
    {
        // A machine that has halted will never reach the target cycle, so stop asking it to.
        if ((cpu_Running != avr->state) && (cpu_Sleeping != avr->state)) { break; }

        avr_run(avr);
        myImpl->sampleStack();
        myImpl->sampleTraces();
        ++executed;
    }
    return cycle() - start;
}

// -----------------------------------------------------------------------------
void Mcu::reset() noexcept
{
    // The stack watermark deliberately survives a reset, so a test that resets between two
    // runs keeps the deeper of the two. resetStackWatermark() clears it explicitly.
    avr_reset(myImpl->avr);

    for (auto& entry : myImpl->pins)
    {
        entry.second.clear();
    }
}

// -----------------------------------------------------------------------------
PinLog& Mcu::watch(const char port, const std::uint8_t bit) noexcept
{
    const PinKey key{port, bit};
    const auto existing = myImpl->pins.find(key);
    if (myImpl->pins.end() != existing) { return existing->second; }

    auto& log = myImpl->pins[key];
    auto irq  = avr_io_getirq(myImpl->avr, AVR_IOCTL_IOPORT_GETIRQ(port), bit);
    if (nullptr == irq) { fail(std::string{"no such pin P"} + port + std::to_string(bit)); }

    // simavr keeps the raw pointer, so the hook has to outlive this call.
    myImpl->hooks.push_back(std::make_unique<PinHook>(PinHook{&log, myImpl->avr}));
    avr_irq_register_notify(irq, onPinChanged, myImpl->hooks.back().get());
    return log;
}

// -----------------------------------------------------------------------------
Trace& Mcu::trace(const std::string& symbol) noexcept
{
    auto found = myImpl->traced.find(symbol);
    if (myImpl->traced.end() != found) { return found->second.log; }

    // A symbol the program does not define gets a log that is never written to rather than an
    // error, so a test for a function nobody has written yet reports "0 visits".
    const std::uint32_t begin{address(symbol)};
    const std::uint32_t end{(NoAddress == begin) ? 0U : myImpl->extentEnd(begin)};

    auto& item = myImpl->traced[symbol];
    item.begin = (NoAddress == begin) ? 1U : begin;
    item.end   = end;

    // Seeded from where the program counter is now, so tracing a function the machine is
    // already inside does not record a spurious entry.
    const auto pc = myImpl->avr->pc;
    item.inside   = (pc >= item.begin) && (pc < item.end);
    return item.log;
}

// -----------------------------------------------------------------------------
StackWatch& Mcu::watchStack(const std::uint16_t low, const std::uint16_t high) noexcept
{
    for (auto& watch : myImpl->stacks)
    {
        if ((low == watch.low()) && (high == watch.high())) { return watch; }
    }
    myImpl->stacks.emplace_back(low, high);
    return myImpl->stacks.back();
}

// -----------------------------------------------------------------------------
Call Mcu::runUntil(const std::string& symbol, const std::uint64_t cycleBudget) noexcept
{
    const std::uint32_t target{address(symbol)};
    if (NoAddress == target) { return {false, 0U}; }

    avr_t* avr{myImpl->avr};
    avr->state = cpu_Running;

    // Bounded by instructions as well as by cycles, for the same reason call() is: a core that
    // has stopped advancing its cycle counter would otherwise keep this loop running for ever.
    std::uint64_t executed{};
    const std::uint64_t start{cycle()};

    while ((cycleBudget > (cycle() - start)) && (cycleBudget > executed))
    {
        if (target == avr->pc) { return {true, cycle() - start}; }
        if ((cpu_Running != avr->state) && (cpu_Sleeping != avr->state)) { break; }
        avr_run(avr);
        myImpl->sampleStack();
        myImpl->sampleTraces();
        ++executed;
    }
    return {false, cycle() - start};
}

// -----------------------------------------------------------------------------
std::uint16_t Mcu::untouchedBytes(const std::uint16_t low, const std::uint16_t high,
                                  const std::uint8_t paintByte) const noexcept
{
    if (high < low) { return 0U; }
    std::uint16_t untouched{};

    for (std::uint32_t addr{low}; addr <= high; ++addr)
    {
        const auto byte = data(static_cast<std::uint16_t>(addr));
        if (paintByte != byte) { break; }
        ++untouched;
    }
    return untouched;
}

// -----------------------------------------------------------------------------
void Mcu::drive(const char port, const std::uint8_t bit, const bool level) noexcept
{
    auto irq = avr_io_getirq(myImpl->avr, AVR_IOCTL_IOPORT_GETIRQ(port), bit);
    if (nullptr == irq) { return; }
    const std::uint32_t raised{level ? 1U : 0U};
    avr_raise_irq(irq, raised);
}
} // namespace avrsim
