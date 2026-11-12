#include "../src/graphics/vulkan/fh1_world_ao.h"
#include <cstdio>
using namespace rex::graphics::vulkan::fh1_world_ao;
constexpr bool CheckCapacity() {
  ImageCapacity c{};
  c=CapacityFor(c,1919,1079,true);
  if (!(c==ImageCapacity{1919,1079,1919,1079})) return false;
  c=CapacityFor(c,1920,1080,true);
  if (!(c==ImageCapacity{1920,1080,1919,1079})) return false;
  if (!(CapacityFor(c,1919,1079,true)==c)) return false;
  const auto active=GridFor(1920,1080);
  if(active.downsample!=2 || active.width!=960 || active.height!=540) return false;
  c=CapacityFor(c,1920,1080,false);
  if (!(c==ImageCapacity{1920,1080,960,540})) return false;
  c=CapacityFor(c,1919,1079,true);
  if (!(c==ImageCapacity{1920,1080,1919,1079})) return false;
  c=CapacityFor(c,641,2161,true);
  if (!(c==ImageCapacity{1920,2161,1919,2161})) return false;
  return CapacityFor(c,641,361,false)==ImageCapacity{641,361,641,361};
}
static_assert(CheckCapacity());
int main() {
 ImageCapacity cap{}; const unsigned sizes[][2]={{1919,1079},{1920,1080},{2501,751},{641,2161},{1920,1080},{1919,1079}};
 for(const auto& wh:sizes) {
  const auto old=cap;cap=CapacityFor(cap,wh[0],wh[1],true);const auto active=GridFor(wh[0],wh[1]);
  if(cap.full_width<wh[0] || cap.full_height<wh[1] || cap.ao_width<active.width || cap.ao_height<active.height ||
     cap.full_width<old.full_width || cap.full_height<old.full_height || cap.ao_width<old.ao_width || cap.ao_height<old.ao_height) return 1;
 }
 std::puts("FH1 AO capacity and active grid: 0 failures");return 0;
}
