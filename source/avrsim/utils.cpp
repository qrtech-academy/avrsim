/**
 * @file Utils implementation details.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "avrsim/mcu.hpp"
#include "avrsim/pin.hpp"
#include "avrsim/stackwatch.hpp"
#include "avrsim/trace.hpp"
#include "avrsim/utils.hpp"

namespace
{
/**
 * @brief One pin to hold at a level before anything runs, as a button or a sensor would.
 */
struct Drive
{
    char port{'B'};
    std::uint8_t bit{};
    bool level{};
};

/**
 * @brief One call to make: a symbol, and what to put in the argument registers.
 */
struct Invocation
{
    std::string symbol{};
    std::uint16_t wordArgument{};
    std::uint8_t byteArgument{};
};

/**
 * @brief What a step does.
 */
enum class Kind : std::uint8_t
{
    CallSymbol,  ///< Call a subroutine and report what it cost.
    DrivePin,    ///< Hold a pin at a level, the way a button would.
    FreeRun,     ///< Let the loaded program run on its own for a while.
    WatchPin,    ///< Record every transition of a pin, and report the gaps at the end.
    TraceSymbol, ///< Record every visit to a function, and report what each cost at the end.
    RunUntil,    ///< Let the program run until control first reaches a symbol.
    WatchStack,  ///< Record how deep the stack pointer goes inside one range of addresses.
    PaintRegion, ///< Fill a range with a known byte now, and report how much of it survives.
};

/**
 * @brief A range of data-space addresses, and the byte a painted one was filled with.
 */
struct Region
{
    std::uint16_t low{};
    std::uint16_t high{};
    std::uint8_t value{};
};

/**
 * @brief One thing to do, in the order the command line gave it.
 */
struct Step
{
    Invocation call{};
    Drive drive{};
    Region region{};
    std::uint64_t cycles{};
    Kind kind{};
};

/** The fewest arguments that can name an image and one step to run on it. */
constexpr int MinimumArguments{3};

/** Argument holding the image path. */
constexpr int ImageArgument{1};

/** Argument holding the first step. */
constexpr int FirstStepArgument{2};

/** r25:r24, where this course's subroutines take their first argument. */
constexpr std::uint8_t WordArgumentRegister{24U};

/** r22, where they take a second, byte-sized one. */
constexpr std::uint8_t ByteArgumentRegister{22U};

/** r24, where a subroutine leaves its result. */
constexpr std::uint8_t ResultRegister{24U};

/** Microseconds in a second, for turning a cycle count into a time. */
constexpr double MicrosecondsPerSecond{1e6};

/** Hertz in a megahertz, for printing the clock the way a datasheet does. */
constexpr double HertzPerMegahertz{1e6};

/** Exit code when the command line is wrong, or a symbol is missing. */
constexpr int UsageError{2};

/** Exit code when a subroutine did not return. */
constexpr int NoReturn{1};

/** Top of SRAM on an ATmega328P, which is where the stack starts. */
constexpr std::uint16_t RamEnd{0x08FFU};

/** How many gaps to print per watched pin before summarising the rest. */
constexpr std::size_t MaxGapsShown{8U};

// -----------------------------------------------------------------------------
[[noreturn]] void fail(const std::string& message) noexcept
{
    // stdout is flushed first so that this does not interleave with the step output above it.
    std::fflush(stdout);
    std::fprintf(stderr, "error: %s\n", message.c_str());
    std::fflush(stderr);
    std::terminate();
}

// -----------------------------------------------------------------------------
void usage(const char* const program) noexcept
{
    std::fprintf(
        stderr,
        "usage: %s <elf> <step> [<step> ...]\n"
        "\n"
        "Steps happen in the order you give them, on one machine.\n"
        "\n"
        "  symbol[:arg1[:arg2]]   call a subroutine and report what it cost\n"
        "                         arg1 goes in r25:r24, so it may be a byte or a pointer\n"
        "                         arg2 goes in r22; both are decimal or 0x-prefixed\n"
        "  --drive P<bit>=<level> hold a pin, the way a button does. A press holds the\n"
        "                         pin low, so --drive B5=0 is a press\n"
        "  --run <cycles>         let the loaded program run on its own. The only way to\n"
        "                         reach a handler: an interrupt cannot be called\n"
        "  --watch P<bit>         record every transition of a pin and report the gaps\n"
        "                         between them, which is how a period gets measured\n"
        "  --trace <symbol>       record every visit to a function: how many, what each\n"
        "                         cost, and how far apart they were. A switch and a tick\n"
        "                         are measured this way, because neither can be called\n"
        "  --until <symbol>       run until control first reaches a symbol, and say how\n"
        "                         long that took. How long a task waited\n"
        "  --stack <low>:<high>   how deep the stack pointer went inside that range. One\n"
        "                         task's stack, rather than all of them at once\n"
        "  --paint <low>:<high>=<byte>\n"
        "                         fill a range with a byte now, and report how much of it\n"
        "                         is still untouched at the end\n"
        "\n"
        "  %s app.elf --trace switch_context --run 200000\n"
        "  %s app.elf --trace tick_isr --run 320000\n"
        "  %s app.elf --stack 0x0500:0x057F --run 500000\n"
        "  %s app.elf --paint 0x0500:0x057F=0xC5 --run 500000\n"
        "  %s app.elf --until task_b --run 40000\n"
        "  %s kernel.elf task_init:0x0500:0x1234\n"
        "\n"
        "A call's figure excludes the rcall that would have got you there, because there\n"
        "was no rcall: this sets the program counter directly. The deepest-stack line at\n"
        "the end covers every step, and a call adds two bytes of its own for the return\n"
        "address this tool pushed.\n",
        program, program, program, program, program, program, program);
}

// -----------------------------------------------------------------------------
[[nodiscard]] unsigned long numberOf(const std::string& text) noexcept
{
    return std::strtoul(text.c_str(), nullptr, 0);
}

// -----------------------------------------------------------------------------
[[nodiscard]] Region parseRegion(const std::string& spec) noexcept
{
    // "0x0500:0x057F", or "0x0500:0x057F=0xC5" where the byte after the equals sign is what
    // --paint fills the range with and --stack ignores.
    const auto colon = spec.find(':');
    if (std::string::npos == colon) { fail("expected <low>:<high>"); }

    const auto equals = spec.find('=', colon);
    Region region{};
    region.low  = static_cast<std::uint16_t>(numberOf(spec.substr(0U, colon)));
    region.high = static_cast<std::uint16_t>(numberOf(spec.substr(
        colon + 1U, (std::string::npos == equals) ? std::string::npos : equals - colon - 1U)));
    if (std::string::npos != equals)
    {
        region.value = static_cast<std::uint8_t>(numberOf(spec.substr(equals + 1U)));
    }
    if (region.high < region.low) { fail("the range runs backwards"); }
    return region;
}

// -----------------------------------------------------------------------------
[[nodiscard]] Drive parseDrive(const std::string& spec) noexcept
{
    Drive drive{};
    if (spec.empty()) { return drive; }
    drive.port        = spec[0U];
    const auto equals = spec.find('=');
    const auto digits = spec.substr(1U, equals - 1U);
    drive.bit         = static_cast<std::uint8_t>(numberOf(digits));
    drive.level       = (std::string::npos != equals) && (0U != numberOf(spec.substr(equals + 1U)));
    return drive;
}

// -----------------------------------------------------------------------------
[[nodiscard]] Invocation parse(const std::string& spec) noexcept
{
    Invocation call{};
    const auto first = spec.find(':');

    if (std::string::npos == first)
    {
        call.symbol = spec;
        return call;
    }

    call.symbol       = spec.substr(0U, first);
    const auto second = spec.find(':', first + 1U);

    if (std::string::npos == second)
    {
        call.wordArgument = static_cast<std::uint16_t>(numberOf(spec.substr(first + 1U)));
        return call;
    }

    call.wordArgument =
        static_cast<std::uint16_t>(numberOf(spec.substr(first + 1U, second - first - 1U)));
    call.byteArgument = static_cast<std::uint8_t>(numberOf(spec.substr(second + 1U)));
    return call;
}
} // namespace

namespace avrsim
{
// -----------------------------------------------------------------------------
int run(const int argc, char** const argv) noexcept
{
    if (MinimumArguments > argc)
    {
        usage(argv[0U]);
        return UsageError;
    }

    const std::string elf{argv[ImageArgument]};
    std::vector<Step> steps{};

    for (int index{FirstStepArgument}; index < argc; ++index)
    {
        const std::string argument{argv[index]};
        if ("--drive" == argument)
        {
            if (argc <= ++index)
            {
                usage(argv[0U]);
                return UsageError;
            }
            Step step{{}, parseDrive(argv[index]), {}, 0U, Kind::DrivePin};
            steps.push_back(step);
            continue;
        }
        if ("--watch" == argument)
        {
            if (argc <= ++index)
            {
                usage(argv[0U]);
                return UsageError;
            }
            Step step{Step{{}, parseDrive(argv[index]), {}, 0U, Kind::WatchPin}};
            steps.push_back(step);
            continue;
        }
        if ("--run" == argument)
        {
            if (argc <= ++index)
            {
                usage(argv[0U]);
                return UsageError;
            }

            Step step{{}, {}, {}, numberOf(argv[index]), Kind::FreeRun};
            steps.push_back(step);
            continue;
        }
        if (("--trace" == argument) || ("--until" == argument))
        {
            if (argc <= ++index)
            {
                usage(argv[0U]);
                return UsageError;
            }
            Invocation named{};
            named.symbol = argv[index];
            steps.push_back(Step{
                named, {}, {}, 0U, ("--trace" == argument) ? Kind::TraceSymbol : Kind::RunUntil});
            continue;
        }
        if (("--stack" == argument) || ("--paint" == argument))
        {
            if (argc <= ++index)
            {
                usage(argv[0U]);
                return UsageError;
            }
            steps.push_back(Step{{},
                                 {},
                                 parseRegion(argv[index]),
                                 0U,
                                 ("--stack" == argument) ? Kind::WatchStack : Kind::PaintRegion});
            continue;
        }
        steps.push_back(Step{parse(argument), {}, {}, 0U, Kind::CallSymbol});
    }

    if (steps.empty())
    {
        usage(argv[0U]);
        return UsageError;
    }

    avrsim::Mcu mcu{elf};

    // Every symbol is checked before anything runs, so a typo in the third call does not
    // report itself only after the first two have already changed the machine's state.
    for (const auto& step : steps)
    {
        if ((Kind::CallSymbol != step.kind) && (Kind::TraceSymbol != step.kind) &&
            (Kind::RunUntil != step.kind))
        {
            continue;
        }
        const auto& call = step.call;
        if (!mcu.has(call.symbol))
        {
            std::fprintf(
                stderr,
                "error: %s defines no global symbol '%s'.\n"
                "       A function without a .global line, or a static C function, is\n"
                "       invisible outside its own object file, which is the usual cause.\n",
                elf.c_str(), call.symbol.c_str());
            return UsageError;
        }
    }

    int status{};
    std::vector<avrsim::PinLog*> watched{};
    std::vector<std::string> watchedNames{};
    std::vector<avrsim::Trace*> traced{};
    std::vector<std::string> tracedNames{};
    std::vector<avrsim::StackWatch*> stacks{};
    std::vector<Region> painted{};

    for (const auto& step : steps)
    {
        if (Kind::DrivePin == step.kind)
        {
            constexpr std::uint64_t cycles{4U};
            mcu.drive(step.drive.port, step.drive.bit, step.drive.level);
            mcu.run(cycles);
            std::printf("holding P%c%u %s\n", step.drive.port, step.drive.bit,
                        step.drive.level ? "high" : "low");
            continue;
        }

        if (Kind::WatchPin == step.kind)
        {
            watched.push_back(&mcu.watch(step.drive.port, step.drive.bit));
            watchedNames.push_back(std::string{"P"} + step.drive.port +
                                   std::to_string(step.drive.bit));
            std::printf("watching P%c%u\n", step.drive.port, step.drive.bit);
            continue;
        }

        if (Kind::TraceSymbol == step.kind)
        {
            traced.push_back(&mcu.trace(step.call.symbol));
            tracedNames.push_back(step.call.symbol);
            std::printf("tracing %s\n", step.call.symbol.c_str());
            continue;
        }

        if (Kind::WatchStack == step.kind)
        {
            stacks.push_back(&mcu.watchStack(step.region.low, step.region.high));
            std::printf("watching the stack in 0x%04X to 0x%04X\n", step.region.low,
                        step.region.high);
            continue;
        }

        if (Kind::PaintRegion == step.kind)
        {
            for (std::uint32_t addr{step.region.low}; addr <= step.region.high; ++addr)
            {
                mcu.setData(static_cast<std::uint16_t>(addr), step.region.value);
            }
            painted.push_back(step.region);
            std::printf("painted 0x%04X to 0x%04X with 0x%02X\n", step.region.low, step.region.high,
                        step.region.value);
            continue;
        }

        if (Kind::RunUntil == step.kind)
        {
            const auto reached = mcu.runUntil(step.call.symbol);
            if (reached.returned)
            {
                std::printf("reached %s after %llu cycles\n", step.call.symbol.c_str(),
                            static_cast<unsigned long long>(reached.cycles));
            }
            else
            {
                std::printf("never reached %s within %llu cycles\n", step.call.symbol.c_str(),
                            static_cast<unsigned long long>(avrsim::Mcu::DefaultBudget));
                status = NoReturn;
            }
            continue;
        }

        if (Kind::FreeRun == step.kind)
        {
            const auto ran = mcu.run(step.cycles);
            std::printf("ran the program for %llu cycles\n", static_cast<unsigned long long>(ran));
            continue;
        }

        const auto& call = step.call;
        mcu.setRegPair(WordArgumentRegister, call.wordArgument);
        mcu.setReg(ByteArgumentRegister, call.byteArgument);
        const auto result = mcu.call(call.symbol);

        std::printf("%s(r25:r24 = 0x%04X, r22 = %u)\n", call.symbol.c_str(), call.wordArgument,
                    call.byteArgument);
        if (!result.returned)
        {
            std::printf("  did not return within %llu cycles.\n",
                        static_cast<unsigned long long>(avrsim::Mcu::DefaultBudget));
            std::printf("  That is what an infinite loop looks like from out here.\n");
            status = NoReturn;
            continue;
        }

        const double microseconds{static_cast<double>(result.cycles) * MicrosecondsPerSecond /
                                  static_cast<double>(mcu.clockHz())};
        std::printf("  cycles     %llu\n", static_cast<unsigned long long>(result.cycles));
        std::printf("  time       %.3f us at %.1f MHz\n", microseconds,
                    static_cast<double>(mcu.clockHz()) / HertzPerMegahertz);
        std::printf("  returned   r24 = %u (0x%02X)\n", mcu.reg(ResultRegister),
                    mcu.reg(ResultRegister));
    }

    // What each watched pin did, and how far apart its transitions were. Gaps rather than
    // absolute times, because a period is a difference.
    for (std::size_t index{}; index < watched.size(); ++index)
    {
        const auto& log = *watched[index];
        std::printf("\n%s: %zu transition(s)\n", watchedNames[index].c_str(), log.count());

        for (std::size_t step{1U}; (step < log.count()) && (step <= MaxGapsShown); ++step)
        {
            std::printf("  gap %zu to %zu: %llu cycles\n", step - 1U, step,
                        static_cast<unsigned long long>(log.gap(step - 1U, step)));
        }
        const auto shown = MaxGapsShown + 1U;
        if (shown < log.count()) { std::printf("  ... %zu more\n", log.count() - shown); }
    }

    // What each traced function did. The count and the per-visit cost are separate claims,
    // and both matter to an overhead figure.
    for (std::size_t index{}; index < traced.size(); ++index)
    {
        const auto& log = *traced[index];
        std::printf("\n%s: %zu visit(s)\n", tracedNames[index].c_str(), log.count());
        if (0U == log.count())
        {
            std::printf("  never entered. Nothing called it, or nothing reached it.\n");
            continue;
        }
        std::printf("  cycles per visit   %llu shortest, %llu longest\n",
                    static_cast<unsigned long long>(log.shortest()),
                    static_cast<unsigned long long>(log.longest()));
        std::printf("  cycles in total    %llu\n",
                    static_cast<unsigned long long>(log.totalCycles()));

        for (std::size_t step{1U}; (step < log.count()) && (step <= MaxGapsShown); ++step)
        {
            std::printf("  gap %zu to %zu: %llu cycles\n", step - 1U, step,
                        static_cast<unsigned long long>(log.gap(step - 1U, step)));
        }
        const auto shown = MaxGapsShown + 1U;
        if (shown < log.count()) { std::printf("  ... %zu more\n", log.count() - shown); }
    }

    // Each watched stack region, which is the per-task question the global mark below
    // cannot answer.
    for (const auto* watch : stacks)
    {
        std::printf("\nstack 0x%04X to 0x%04X: ", watch->low(), watch->high());
        if (!watch->entered())
        {
            std::printf("never entered.\n");
            std::printf("  Not a depth of zero. Nothing ran on this stack at all, and a\n");
            std::printf("  watermark reports the moments that happened.\n");
            continue;
        }
        std::printf("SP reached 0x%04X, %u byte(s) used\n", watch->lowest(),
                    static_cast<unsigned>(watch->deepestBytes()));
    }

    // Each painted region. This measures where the stack *reached*; the watch above
    // measures where the stack *pointer* went, and the two are allowed to disagree.
    for (const auto& region : painted)
    {
        const auto untouched = mcu.untouchedBytes(region.low, region.high, region.value);
        const auto size      = static_cast<unsigned>(region.high - region.low + 1U);
        std::printf("\npaint 0x%04X to 0x%04X: %u of %u byte(s) still 0x%02X\n", region.low,
                    region.high, static_cast<unsigned>(untouched), size, region.value);
        std::printf("  high-water mark %u byte(s) from the top\n",
                    size - static_cast<unsigned>(untouched));
        if (0U == untouched)
        {
            std::printf("  Nothing of the paint survives, so this region overflowed, or it\n");
            std::printf("  was exactly filled. A watermark cannot tell those apart.\n");
        }
    }

    // The stack's low-water mark over everything above. A call() measurement includes the two
    // bytes this tool pushed itself; a --run measurement does not, and the line says so.
    const auto lowest = mcu.lowestStackPtr();
    if (avrsim::Mcu::NoStackMark != lowest)
    {
        std::printf("\ndeepest stack: SP reached 0x%04X, %u bytes below RAMEND\n", lowest,
                    static_cast<unsigned>(RamEnd - lowest));
    }
    return status;
}
} // namespace avrsim
