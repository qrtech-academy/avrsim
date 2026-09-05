/**
 * @file Utils implementation.
 *
 *       Measure what one or more subroutines cost, and print it.
 */
#pragma once

namespace avrsim
{
/**
 * @brief Run every step the command line gives, on one machine, and print what each one cost.
 *
 * @param[in] argc Argument count.
 * @param[in] argv The image, then one or more steps.
 *
 * @return 0 on success, 1 if a subroutine never returned, 2 on a usage error.
 */
[[nodiscard]] int run(int argc, char** argv) noexcept;

} // namespace avrsim
