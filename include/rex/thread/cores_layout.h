/**
 * @file        thread/cores_layout.h
 * @brief       The pure parts of linux_thread_cores: core lists, name matching, the "auto" layout.
 *
 * Rule syntax and the Steam Deck layout follow Pinyon Shift's linux_thread_cores (ShiftGlue
 * 2849938; BSD 3-Clause, Copyright (c) 2026 Pinyon Shift contributors). Header only, so a test
 * can run it without the runtime.
 */
#pragma once

#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rex::thread::cores {

// "6-7" or "0,2-5" as a sorted list of CPUs; false when malformed or empty.
inline bool ParseCpuList(const std::string& list, std::vector<int>& out) {
  out.clear();
  std::stringstream stream(list);
  std::string part;
  while (std::getline(stream, part, ',')) {
    if (part.empty()) continue;
    const size_t dash = part.find('-');
    char* end = nullptr;
    const long first = std::strtol(part.c_str(), &end, 10);
    if (end == part.c_str() || first < 0) return false;
    long last = first;
    if (dash != std::string::npos) {
      last = std::strtol(part.c_str() + dash + 1, &end, 10);
      if (last < first) return false;
    }
    for (long cpu = first; cpu <= last && cpu < 1024; ++cpu) out.push_back(int(cpu));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return !out.empty();
}

inline std::string CpuListToString(const std::vector<int>& cpus) {
  std::string out;
  for (size_t i = 0; i < cpus.size(); ++i) {
    size_t j = i;
    while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
    if (!out.empty()) out += ',';
    out += std::to_string(cpus[i]);
    if (j > i) out += '-' + std::to_string(cpus[j]);
    i = j;
  }
  return out;
}

// Whether `text` starts with `rule_name`, where '_' in the rule also matches a space.
inline bool NameMatches(std::string_view rule_name, std::string_view text) {
  if (text.size() < rule_name.size()) return false;
  for (size_t i = 0; i < rule_name.size(); ++i) {
    if (rule_name[i] == text[i] || (rule_name[i] == '_' && text[i] == ' ')) continue;
    return false;
  }
  return true;
}

// The guest entries (Title Update 4) the layout names: the main loop, the audio mixer, the render
// thread. The threads' names carry a host address, so XThread registers "Guest_<entry>" for them.
inline constexpr const char* kMainLoopEntry = "Guest_825CEC88";
inline constexpr const char* kAudioMixerEntry = "Guest_83075D18";
inline constexpr const char* kRenderEntry = "Guest_8261E318";

// "auto" for a CPU with exactly four cores of two threads each (a Steam Deck: 4 Zen 2 cores with SMT):
// `cores` lists each core's CPUs (its SMT siblings) among the CPUs the process may use, cores in CPU
// order. Core 0 runs the simulation (main loop) and the audio mixer, core 1 the render thread and the
// Vulkan submission thread, core 2's first CPU the GPU command processor alone; every other thread runs
// on the rest (core 2's other CPU and core 3). Empty for any other CPU: on four cores without SMT, with
// one CPU for every pair of threads and one for all the others, this layout was measured slower than the
// scheduler (docs/perf.md). This is Pinyon Shift's Steam Deck layout, measured by them on a Deck.
inline std::string AutoLayout(const std::vector<std::vector<int>>& cores) {
  if (cores.size() != 4) return {};
  for (const auto& core : cores) {
    if (core.size() != 2) return {};
  }
  std::vector<int> others = cores[3];
  others.push_back(cores[2][1]);
  std::sort(others.begin(), others.end());
  return std::string(kMainLoopEntry) + "=" + std::to_string(cores[0][0]) + ";" + kAudioMixerEntry + "=" +
         std::to_string(cores[0][1]) + ";" + kRenderEntry + "=" + std::to_string(cores[1][0]) +
         ";Vulkan_Submiss=" + std::to_string(cores[1][1]) + ";GPU_Commands=" + std::to_string(cores[2][0]) +
         ";*=" + CpuListToString(others);
}

}  // namespace rex::thread::cores
