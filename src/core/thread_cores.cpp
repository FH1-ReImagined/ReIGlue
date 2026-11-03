/**
 * @file        thread_cores.cpp
 * @brief       Placing threads on CPU cores by name (linux_thread_cores).
 *
 * The idea, the rule syntax and the Steam Deck layout come from Pinyon Shift
 * (ShiftGlue 2849938, linux_thread_cores; BSD 3-Clause, Copyright (c) 2026 Pinyon
 * Shift contributors). This is written against ReIGlue's threads: guest threads are
 * matched by their entry function, and a thread placement thread follows the process.
 */

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/thread/cores.h>

#if REX_PLATFORM_LINUX

#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/thread/cores_layout.h>

REXCVAR_DEFINE_STRING(linux_thread_cores, "off", "Kernel",
                      "Pins this process's threads to CPU cores by thread name (Linux). off: "
                      "the scheduler places them. auto: a layout for a CPU of four cores with "
                      "two threads each (a Steam Deck), from the CPUs the process may use and "
                      "their SMT siblings; any other CPU is left to the scheduler. Otherwise "
                      "entries NAME=CORES separated by semicolons: NAME is the start of a "
                      "thread's name or of its guest entry (Guest_825CEC88), _ also matches a "
                      "space, * is every thread no other entry names; CORES is a list such as 6-7 "
                      "or 0,2-5 (of the CPUs the process may use). REX_THREAD_CORES_FILE=path in "
                      "the environment: the first line of that file, while it exists, replaces "
                      "this value (to flip a layout in a running game). Idea and syntax from "
                      "Pinyon Shift's linux_thread_cores")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::thread {
namespace {

namespace layout = rex::thread::cores;
using CpuSet = cpu_set_t;

struct Rule {
  std::string name;  // empty: the fallback (*)
  CpuSet cpus;
};

std::string ReadFirstLine(const std::string& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

// The value in force: the first line of the file REX_THREAD_CORES_FILE names while it exists and
// is not empty (to switch a layout on and off in a running game, for A/B tests), else the cvar.
std::string CurrentValue() {
  static const char* const file = std::getenv("REX_THREAD_CORES_FILE");
  if (file && *file) {
    std::string value = ReadFirstLine(file);
    if (!value.empty()) return value;
  }
  return REXCVAR_GET(linux_thread_cores);
}

CpuSet ToSet(const std::vector<int>& cpus) {
  CpuSet set;
  CPU_ZERO(&set);
  for (int cpu : cpus) {
    if (cpu >= 0 && cpu < CPU_SETSIZE) CPU_SET(cpu, &set);
  }
  return set;
}

std::string ToString(const CpuSet& set) {
  std::vector<int> cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set)) cpus.push_back(cpu);
  }
  return layout::CpuListToString(cpus);
}

// The cores among `allowed`: each core's CPUs are the SMT siblings sysfs lists for it.
std::vector<std::vector<int>> CoresOf(const CpuSet& allowed) {
  std::vector<std::vector<int>> cores;
  std::set<int> seen;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &allowed) || seen.count(cpu)) continue;
    std::vector<int> siblings;
    const std::string list = ReadFirstLine("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                                           "/topology/thread_siblings_list");
    std::vector<int> listed;
    if (!list.empty() && layout::ParseCpuList(list, listed)) {
      for (int c : listed) {
        if (c >= 0 && c < CPU_SETSIZE && CPU_ISSET(c, &allowed)) siblings.push_back(c);
      }
    }
    if (siblings.empty()) siblings.push_back(cpu);
    for (int c : siblings) seen.insert(c);
    cores.push_back(std::move(siblings));
  }
  return cores;
}

class Placement {
 public:
  static Placement& Get() {
    static Placement* instance = new Placement();  // never destroyed: threads outlive statics
    return *instance;
  }

  void SetAlias(std::string_view alias) {
    const std::string value = CurrentValue();
    const bool off = value.empty() || value == "off";
    const pid_t tid = pid_t(syscall(SYS_gettid));
    // A change of the cvar at run time (hot reload) starts the placement thread.
    static std::once_flag callback_once;
    std::call_once(callback_once, [] {
      rex::cvar::RegisterChangeCallback("linux_thread_cores", [](std::string_view, std::string_view) {
        Placement& placement = Get();
        std::lock_guard<std::mutex> lock(placement.mutex_);
        placement.EnsureStarted();
      });
    });
    std::lock_guard<std::mutex> lock(mutex_);
    // The alias is kept while the value is off too: a later change (the cvar's hot reload, the
    // REX_THREAD_CORES_FILE file) places threads that were started before it.
    aliases_[tid] = std::string(alias);
    static const bool file_override = std::getenv("REX_THREAD_CORES_FILE") != nullptr;
    if (off && !file_override) return;
    EnsureStarted();
    if (off) return;
    const std::string comm = CommOf(tid);
    if (alias_log_budget_ > 0) {
      --alias_log_budget_;
      REXSYS_INFO("linux_thread_cores: thread {} '{}' is {}", tid, comm, alias);
    }
    Reparse(value);
    if (const Rule* rule = Find(comm, aliases_[tid])) Apply(tid, *rule, comm, aliases_[tid]);
  }

 private:
  Placement() {
    CPU_ZERO(&initial_);
    // The mask the process started with (taskset, a cpuset): the layout stays inside it.
    sched_getaffinity(0, sizeof(initial_), &initial_);
  }

  void EnsureStarted() {
    if (started_) return;
    started_ = true;
    thread_ = std::thread([this] { Run(); });
    std::atexit([] { Get().Stop(); });
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(sleep_mutex_);
      stopping_ = true;
    }
    sleep_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  static std::string CommOf(pid_t tid) {
    return ReadFirstLine("/proc/self/task/" + std::to_string(tid) + "/comm");
  }

  // Rules from the value; "auto" is decided once.
  void Reparse(const std::string& value) {
    if (value == parsed_value_) return;
    parsed_value_ = value;
    rules_.clear();
    fallback_.reset();
    std::string text = value;
    if (value == "auto") {
      if (!auto_decided_) {
        auto_decided_ = true;
        const auto cores = CoresOf(initial_);
        auto_layout_ = layout::AutoLayout(cores);
        if (auto_layout_.empty()) {
          REXSYS_INFO(
              "linux_thread_cores auto: {} cores of the CPUs this process may use are not 4 cores "
              "of 2 threads; threads left to the scheduler",
              cores.size());
        } else {
          REXSYS_INFO("linux_thread_cores auto: layout {}", auto_layout_);
        }
      }
      text = auto_layout_;
    }
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ';')) {
      const size_t equals = item.find('=');
      if (equals == std::string::npos || equals == 0) continue;
      Rule rule;
      rule.name = item.substr(0, equals);
      std::vector<int> listed;
      if (!layout::ParseCpuList(item.substr(equals + 1), listed)) {
        REXSYS_WARN("linux_thread_cores: bad core list in '{}'", item);
        continue;
      }
      const CpuSet requested = ToSet(listed);
      CPU_AND(&rule.cpus, &requested, &initial_);
      if (CPU_COUNT(&rule.cpus) == 0) {
        REXSYS_WARN("linux_thread_cores: '{}' names no CPU this process may use", item);
        continue;
      }
      if (rule.name == "*") {
        rule.name.clear();
        fallback_ = std::move(rule);
      } else {
        rules_.push_back(std::move(rule));
      }
    }
    REXSYS_INFO("linux_thread_cores '{}': {} rules{}", value, rules_.size(),
                fallback_ ? " and a fallback" : "");
  }

  const Rule* Find(const std::string& comm, const std::string& alias) const {
    for (const Rule& rule : rules_) {
      if (layout::NameMatches(rule.name, comm) ||
          (!alias.empty() && layout::NameMatches(rule.name, alias))) {
        return &rule;
      }
    }
    return fallback_ ? &*fallback_ : nullptr;
  }

  // Returns whether the thread was moved.
  bool Apply(pid_t tid, const Rule& rule, const std::string& comm, const std::string& alias) {
    CpuSet current;
    if (sched_getaffinity(tid, sizeof(current), &current) != 0) return false;
    if (CPU_EQUAL(&current, &rule.cpus)) {
      pinned_.insert(tid);
      return false;
    }
    if (sched_setaffinity(tid, sizeof(rule.cpus), &rule.cpus) != 0) return false;
    pinned_.insert(tid);
    if (log_budget_ > 0) {
      --log_budget_;
      REXSYS_INFO("linux_thread_cores: thread {} '{}' {} on CPUs {}", tid, comm, alias,
                  ToString(rule.cpus));
    }
    return true;
  }

  void Run() {
    const pid_t self = pid_t(syscall(SYS_gettid));
    // Started by a guest thread that is already placed: this one stays free.
    sched_setaffinity(0, sizeof(initial_), &initial_);
    const auto start = std::chrono::steady_clock::now();
    static const bool file_override = std::getenv("REX_THREAD_CORES_FILE") != nullptr;
    while (true) {
      const std::string value = CurrentValue();
      size_t moved = 0, restored = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool off = value.empty() || value == "off";
        if (!off) Reparse(value);
        std::set<pid_t> live;
        if (DIR* tasks = opendir("/proc/self/task")) {
          while (const dirent* entry = readdir(tasks)) {
            const pid_t tid = pid_t(std::atoi(entry->d_name));
            if (tid <= 0 || tid == self) continue;
            live.insert(tid);
            if (off) {
              if (pinned_.count(tid) && sched_setaffinity(tid, sizeof(initial_), &initial_) == 0) {
                ++restored;
              }
              continue;
            }
            const std::string comm = CommOf(tid);
            const auto alias = aliases_.find(tid);
            const std::string alias_text = alias == aliases_.end() ? std::string() : alias->second;
            if (const Rule* rule = Find(comm, alias_text)) moved += Apply(tid, *rule, comm, alias_text);
          }
          closedir(tasks);
        }
        if (off) pinned_.clear();
        for (auto it = aliases_.begin(); it != aliases_.end();) {
          it = live.count(it->first) ? std::next(it) : aliases_.erase(it);
        }
        for (auto it = pinned_.begin(); it != pinned_.end();) {
          it = live.count(*it) ? std::next(it) : pinned_.erase(it);
        }
      }
      if (moved || restored) {
        REXSYS_INFO("linux_thread_cores: {} threads placed, {} restored", moved, restored);
      }
      // Often at first (threads inherit the mask of the thread that starts them), then rarely.
      const bool early = std::chrono::steady_clock::now() - start < std::chrono::seconds(20);
      std::unique_lock<std::mutex> lock(sleep_mutex_);
      if (sleep_cv_.wait_for(lock,
                             file_override ? std::chrono::milliseconds(250)
                                           : early ? std::chrono::milliseconds(500)
                                                   : std::chrono::seconds(5),
                             [this] { return stopping_; })) {
        return;
      }
    }
  }

  std::mutex mutex_;
  std::mutex sleep_mutex_;
  std::condition_variable sleep_cv_;
  bool stopping_ = false;
  bool started_ = false;
  std::thread thread_;
  CpuSet initial_;
  std::string parsed_value_;
  bool auto_decided_ = false;
  std::string auto_layout_;
  std::vector<Rule> rules_;
  std::optional<Rule> fallback_;
  std::unordered_map<pid_t, std::string> aliases_;
  std::set<pid_t> pinned_;
  int log_budget_ = 80;
  int alias_log_budget_ = 80;
};

}  // namespace

void SetCurrentThreadPlacementAlias(std::string_view alias) { Placement::Get().SetAlias(alias); }

}  // namespace rex::thread

#else  // REX_PLATFORM_LINUX

namespace rex::thread {
void SetCurrentThreadPlacementAlias(std::string_view) {}
}  // namespace rex::thread

#endif
