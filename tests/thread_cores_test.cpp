// linux_thread_cores: the pure parts (core lists, name matching, the auto layout) for a Steam Deck
// (4 cores, 8 threads), a 4-core CPU without SMT, and other CPUs. No runtime needed.
#include <rex/thread/cores_layout.h>

#include <cstdio>
#include <cstdlib>

using namespace rex::thread::cores;

#define CHECK(x)                                                         \
  do {                                                                   \
    if (!(x)) {                                                          \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                                      \
    }                                                                    \
  } while (0)

int main() {
  std::vector<int> cpus;
  CHECK(ParseCpuList("6-7", cpus) && CpuListToString(cpus) == "6-7");
  CHECK(ParseCpuList("0,2-5", cpus) && CpuListToString(cpus) == "0,2-5" && cpus.size() == 5);
  CHECK(ParseCpuList("3,3,1", cpus) && CpuListToString(cpus) == "1,3");
  CHECK(!ParseCpuList("", cpus));
  CHECK(!ParseCpuList("5-2", cpus));
  CHECK(!ParseCpuList("x", cpus));

  CHECK(NameMatches("GPU_Commands", "GPU Commands (F8000024)"));
  CHECK(NameMatches("GPU_Commands", "GPU_Commands"));
  CHECK(!NameMatches("GPU_Commands", "GPU VSync"));
  CHECK(NameMatches("Guest_825CEC88", "Guest_825CEC88"));
  CHECK(!NameMatches("Guest_825CEC88", "Guest_825CEC8"));
  CHECK(!NameMatches("Vulkan_Submissions", "Vulkan Submissi"));  // longer than the 15-character comm

  // A Steam Deck: CPUs 0-1 are core 0's siblings, 2-3 core 1's, and so on.
  const std::vector<std::vector<int>> deck = {{0, 1}, {2, 3}, {4, 5}, {6, 7}};
  CHECK(AutoLayout(deck) ==
        "Guest_825CEC88=0;Guest_83075D18=1;Guest_8261E318=2;Vulkan_Submiss=3;GPU_Commands=4;*=5-7");
  // Four cores without SMT (taskset -c 0-3 on a desktop): measured slower than the scheduler, so no layout.
  const std::vector<std::vector<int>> plain = {{0}, {1}, {2}, {3}};
  CHECK(AutoLayout(plain).empty());
  // Siblings numbered the other way (Intel: CPU n and n+4).
  const std::vector<std::vector<int>> intel = {{0, 4}, {1, 5}, {2, 6}, {3, 7}};
  CHECK(AutoLayout(intel) ==
        "Guest_825CEC88=0;Guest_83075D18=4;Guest_8261E318=1;Vulkan_Submiss=5;GPU_Commands=2;*=3,6-7");
  CHECK(AutoLayout({{0, 1}, {2, 3}}).empty());                     // two cores
  CHECK(AutoLayout({{0}, {1}, {2}, {3}, {4}, {5}}).empty());       // six cores
  std::puts("thread_cores_test ok");
  return 0;
}
