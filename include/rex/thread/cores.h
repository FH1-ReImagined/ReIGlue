/**
 * @file        thread/cores.h
 * @brief       Placing threads on CPU cores by name (linux_thread_cores).
 *
 * The idea, the rule syntax and the Steam Deck layout come from Pinyon Shift
 * (ShiftGlue 2849938, linux_thread_cores; BSD 3-Clause, Copyright (c) 2026 Pinyon
 * Shift contributors).
 */
#pragma once

#include <string_view>

namespace rex::thread {

// Gives the calling thread a second name to be matched by, next to its name: guest threads
// are named after their entry function ("Guest_825CEC88"), because their own names carry a
// host address or handle that differs from run to run. Applies the cvar's rule for this
// thread at once, so threads it starts inherit that placement, and starts the placement thread
// that follows later changes. Does nothing unless linux_thread_cores is set, on Linux.
void SetCurrentThreadPlacementAlias(std::string_view alias);

}  // namespace rex::thread
