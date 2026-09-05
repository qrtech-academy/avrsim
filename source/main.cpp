/**
 * @file AVR simulator entry point.
 */
#include "avrsim/utils.hpp"

/**
 * @brief Measure every step given on the command line and print what each one cost.
 *
 * @param[in] argc Argument count.
 * @param[in] argv The image, then one or more steps.
 *
 * @return 0 on success, 1 if a subroutine never returned, 2 on a usage error.
 */
int main(const int argc, char** const argv) { return avrsim::run(argc, argv); }
