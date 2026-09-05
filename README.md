# avrsim
A small C++17 harness for measuring AVR firmware from a unit test, and the command-line tool that
comes with it.

It wraps [simavr](https://github.com/buserror/simavr), which is a cycle-accurate AVR core you can
link against. Note the distinction: the `simavr` command-line program runs a firmware and shows you
a waveform, and this uses none of that. It links `libsimavr` and drives the core instruction by
instruction, which is what lets a test stop after one context switch and look at both stacks.

The QAcademy courses use it as a submodule at `tools/avrsim`. **This is course material, not
coursework**: nothing in either course asks you to change it, and no exercise depends on reading
it. It is documented here because you will be reading its output when a test fails.

```bash
sudo apt -y install g++ libsimavr-dev libelf-dev
make            # libavrsim.a and the avrsim tool
./avrsim app.elf --trace switch_context --run 200000
./avrsim drivers.hex led_init:0x0200:13 led_on:0x0200
```

`make lib` builds the archive alone, which is what a course's test suite links against.

---

## What it loads
Two formats, told apart by the extension, because the courses assemble with two toolchains.

**`.elf`**, from `avr-gcc`, is read whole: the program and its symbol table arrive together.

**`.hex`**, from `avra`, is read with **the map file beside it**. `avra` emits Intel hex and no
symbol table at all, so `drivers.hex` is loaded together with `drivers.map`, and the two must share
a stem. Two things about that are worth knowing before they cost you an afternoon:
* **The map's addresses are in words and simavr's program counter is in bytes.** They are doubled
  once, on the way in. Getting it wrong puts every symbol at half its address, which looks far more
  like broken hardware than like an off-by-two.
* **Only the `L` rows are labels.** The type column separates a label from an assemble-time
  constant, and `m328Pdef.inc` alone contributes some two thousand of the latter. Without the
  filter, every register name in the device file becomes a callable symbol.

A hex is also read in **chunks** rather than as one blob, because a program with a vector table is
not contiguous: `.org 0x0000` puts two bytes at zero and the next `.org` puts the rest somewhere
above. The single-blob call returns only the first run, which presents as a subroutine that never
returns rather than as a load error.

Everything above the loader is the same either way. `call()`, `trace()` and the rest are handed a
program in flash and a name-to-address map, and cannot tell which format produced them.

**One method is worse on a hex than on an ELF, and it is `trace()`.** It derives where a function
ends from the next symbol above it, which is exact when local labels never reach the symbol table
and wrong when they do. `avra` exports every label it assembles, so the symbol above `shift_bits`
is `shift_bits_loop` four bytes later, and the trace reports the two instructions in between —
a small, constant, entirely plausible number. Trace ELF builds. On a hex, only a routine with no
internal labels reports correctly, and every other method is unaffected.

---

## Why a kernel needs more than `call()`
The assembly course measured a driver library, and a driver subroutine can be called. So the
harness it needed was `call("led_init")`, and that is still here and still works for the parts of a
kernel that genuinely are ordinary functions.

**A context switch cannot be called.** It happens *to* a task, from a `yield` or from an interrupt,
and it returns onto a stack belonging to somebody else. There is no return address to match and no
frame to unwind. The same is true of everything else a kernel course measures: a tick arrives, a
delay
is a period during which the function that asked for it is *not* running, and an inversion is one
task waiting on another.

So the four additions are all about letting the program run and watching what it does.

| Method | What it answers | Used by |
|---|---|---|
| `trace(symbol)` | How often, what each pass cost, how far apart | L01, L02, L06, L08 |
| `runUntil(symbol)` | How long until control first got there | L03, L06 |
| `watchStack(low, high)` | How deep the stack pointer went in one range | L04 |
| `untouchedBytes(low, high, paint)` | How much of a painted region survived | L04 |

---

## What it can do

```cpp
constexpr std::uint16_t TaskStackLow{0x0500U};
constexpr std::uint16_t TaskStackHigh{0x057FU};
constexpr std::uint64_t RunCycles{200000U};
constexpr std::uint64_t SwitchCost{66U};
constexpr std::uint64_t TickPeriod{16000U};
constexpr std::uint16_t DeepestBytes{41U};

avrsim::Mcu mcu{"kernel/build/app.elf"};

auto& switches = mcu.trace("switch_context");
auto& ticks    = mcu.trace("tick_isr");
auto& stack    = mcu.watchStack(TaskStackLow, TaskStackHigh);

mcu.run(RunCycles);

EXPECT_EQ(switches.shortest(), SwitchCost);      // What one switch cost.
EXPECT_EQ(switches.longest(), SwitchCost);       // And that it cost the same every time.
EXPECT_EQ(ticks.gap(0U, 1U), TickPeriod);        // The tick period you actually got.
EXPECT_TRUE(stack.entered());
EXPECT_EQ(stack.deepestBytes(), DeepestBytes);   // One task's stack, not all four.
```

`data()` addresses the data space as the device sees it, so `data(0x25)` is PORTB and `data(0x0500)`
is wherever a task's stack was put. `watch('B', 5)` records every transition of a pin with the cycle
it happened on, which is how a test checks that a task ran when it was supposed to rather than
merely that it ran.

---

## How `trace()` decides where a function ends
A function is taken to run from its symbol up to **the next symbol in flash**, because the AVR
symbol table records where a symbol starts and does not record how long it is. Entry and exit are
then edge-triggered on the program counter crossing that boundary.

That is what makes it work for a switch. There is no frame to track and no return address to match,
and asking *where the program counter is* needs neither.

**It is a derivation, not a fact, and it is worth knowing when it would be wrong.** A function
followed by a constant pool rather than by another function measures wide, and so does the last
symbol in flash, whose extent runs to the end of the program. Ordinary compiled and assembled code
lays symbols out one after another and this is exact for it.

Two more things worth knowing when a trace surprises you:
* **A visit that had not finished when the run stopped is left open**, and an open visit
  contributes nothing to `totalCycles()`, `shortest()` or `longest()`. A cost that is still being
  paid is not a measurement.
* **`gap()` measures entry to entry**, not exit to entry, because what a tick period means is how
  often the handler starts.

---

## The two ways to measure a stack, and why there are two
`watchStack()` samples the stack pointer after every instruction and reports the lowest it went
inside a range. `untouchedBytes()` counts how much of a region you painted still holds the paint.

They answer different questions and **they are allowed to disagree**, which is most of L04:
* `watchStack` reports where the stack *pointer* went. An interrupt can write below it, and a
  handler that pushes before the pointer moves is writing memory this never saw.
* `untouchedBytes` reports where the stack *reached*. A byte that happened to be written with the
  paint value looks untouched for ever, so it under-reports rather than over-reports.
* Neither sees the interrupt that has not arrived yet. A watermark reports the moments that
  happened, which is why L04's cross-check trusts the arithmetic over the measurement.

A region that was never entered reports `entered() == false` rather than a depth of zero. Those are
different facts and a test that confuses them is testing nothing.

---

## How `call()` works, and why it can
The AVR has no notion of calling one subroutine in isolation. `call()` fakes what an `rcall` would
have done: it resets the stack pointer to RAMEND, pushes a return address pointing at the first even
byte past the end of your program, sets the program counter to your symbol, and runs until control
comes back to that address. Reaching it is how the harness knows the subroutine returned rather than
merely stopped being interesting.

Two consequences worth knowing when a test surprises you:
* **The stack is reset on every call**, so a function that leaves it unbalanced fails its own test
  rather than the next one. A bug that moves is much worse than a bug that stays put.
* **The three cycles of the `rcall` are not in the reported figure**, because there was no `rcall`.

A call that never returns is capped at a cycle budget and comes back with `returned == false` rather
than hanging the suite.

---

## The command-line tool
`avrsim` is the same machinery with a command line on it, and `make measure` runs it. Every
cross-check gives the exact invocation, and `avrsim` with no arguments prints
the full list of steps.

```bash
make measure TRACE=switch_context RUN=200000
make measure CALLS="--trace tick_isr --run 320000"
make measure CALLS="--stack 0x0500:0x057F --paint 0x0500:0x057F=0xC5 --run 500000"
make measure CALLS="--until task_b --run 40000"
```

Steps run in the order you give them, on one machine, so a `--paint` before a `--run` paints and
then measures while a `--paint` after one measures nothing.

---

## Building
`make -C tools/avrsim lib` produces `libavrsim.a`; `ci/build.sh` does it for you. It needs
`libsimavr-dev`, which is the package people miss:

```bash
sudo apt -y install simavr libsimavr-dev
```

`make format` reformats every source and header in place, and `ci/format.sh` reports what is not
formatted and changes nothing. Both need `clang-format`.

---
