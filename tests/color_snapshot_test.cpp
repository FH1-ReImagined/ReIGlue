#ifdef NDEBUG
#undef NDEBUG
#endif
#include <rex/ui/vulkan/color_snapshot.h>
#include <cassert>
#include <vector>
using namespace rex::ui::vulkan;

struct Recorder {
  std::vector<int> events;
  SnapshotAccess state{VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkImageCopy recorded{};
  void Begin(SnapshotAccess read) {
    assert(events.empty());
    assert(read.stage == VK_PIPELINE_STAGE_TRANSFER_BIT);
    assert(read.access == VK_ACCESS_TRANSFER_READ_BIT);
    assert(read.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    state = read;
    events.push_back(1);
  }
  void Copy(VkImage from, VkImage to, const VkImageCopy& region) {
    assert(events == std::vector<int>{1} && from != to);
    assert(state.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    recorded = region;
    events.push_back(2);
  }
  void Finish(SnapshotAccess next) {
    assert(events == (std::vector<int>{1,2}));
    state = next;
    events.push_back(3);
  }
};

int main() {
  SnapshotSurface scene{reinterpret_cast<VkImage>(uintptr_t(1)), VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      2560, 1440, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
  SnapshotSurface target{reinterpret_cast<VkImage>(uintptr_t(2)), scene.format,
      1280, 720, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT};
  const SnapshotAccess draw{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  Recorder recorder;
  assert(RecordColorSnapshot(scene,target,1280,720,draw,recorder));
  assert(recorder.events == (std::vector<int>{1,2,3}));
  assert(recorder.state.layout == draw.layout && recorder.state.access == draw.access);
  assert(recorder.recorded.extent.width == 1280 && recorder.recorded.extent.height == 720);
  assert(recorder.recorded.extent.depth == 1);
  assert(recorder.recorded.srcOffset.x == 0 && recorder.recorded.dstOffset.y == 0);
  assert(recorder.recorded.srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
  assert(recorder.recorded.srcSubresource.mipLevel == 0 && recorder.recorded.dstSubresource.layerCount == 1);
  assert(recorder.recorded.srcSubresource.baseArrayLayer == 0);
  for (unsigned fault = 0; fault < 12; ++fault) {
    auto source = scene, destination = target;
    uint32_t width = 1280, height = 720;
    switch (fault) {
      case 0: source.image = VK_NULL_HANDLE; break;
      case 1: destination.image = source.image; break;
      case 2: source.samples = VK_SAMPLE_COUNT_2_BIT; break;
      case 3: destination.samples = VK_SAMPLE_COUNT_4_BIT; break;
      case 4: destination.format = VK_FORMAT_R8G8B8A8_UNORM; break;
      case 5: source.usage = 0; break;
      case 6: destination.usage = VK_IMAGE_USAGE_SAMPLED_BIT; break;
      case 7: source.width = 1279; break;
      case 8: height = 721; break;
      case 9: width = 0; break;
      case 10: source.height = uint64_t(UINT32_MAX) + 1; break;
      case 11: destination.width = uint64_t(UINT32_MAX) + 1281; break;
    }
    Recorder rejected;
    assert(!RecordColorSnapshot(source,destination,width,height,draw,rejected));
    assert(rejected.events.empty());
    assert(rejected.state.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  assert(PlanColorSnapshot(scene,target,1280,720));
  assert(!PlanColorSnapshot(scene,target,1281,720));
}
