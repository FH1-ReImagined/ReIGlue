// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/hdr_surface.h>
#include <array>

namespace rex::ui::vulkan {
// One full-surface write after the composed SDR attachment becomes readable.
// The acquire semaphore protects the color-output stage; the completed
// attachment is transitioned for the presentation engine by the render pass.
inline VkRenderPass CreateHdrOutputPass(const VulkanDevice& owner, VkFormat format) {
  VkAttachmentDescription color{};
  color.format = format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  const std::array<VkSubpassDependency, 2> dependencies{{
      {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT},
      {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       VK_ACCESS_MEMORY_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT}}};
  VkRenderPassCreateInfo description{};
  description.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  description.attachmentCount = description.subpassCount = 1;
  description.pAttachments = &color;
  description.pSubpasses = &subpass;
  description.dependencyCount = uint32_t(dependencies.size());
  description.pDependencies = dependencies.data();
  VkRenderPass result = VK_NULL_HANDLE;
  if (owner.functions().vkCreateRenderPass(owner.device(), &description, nullptr, &result) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  return result;
}

template <size_t Count>
bool EnsureHdrShader(const VulkanDevice& owner, const uint32_t (&code)[Count], VkShaderModule& module) {
  if (module) return true;
  VkShaderModuleCreateInfo description{};
  description.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  description.codeSize = sizeof(code);
  description.pCode = code;
  return owner.functions().vkCreateShaderModule(owner.device(), &description, nullptr, &module) == VK_SUCCESS;
}

// Host-sized rectangle and fullscreen viewport for the color conversion. No
// image lifetime or settings state is stored in the command recorder.
template <typename Parameters>
void RecordHdrOutput(const VulkanDevice& owner, VkCommandBuffer commands, VkExtent2D extent,
                     const HdrSurface& frame, VkRenderPass pass, VkPipeline pipeline,
                     VkPipelineLayout layout, const Parameters& parameters) {
  const auto& api = owner.functions();
  const auto samples = frame.samples();
  VkRenderPassBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin.renderPass = pass;
  begin.framebuffer = frame.target();
  begin.renderArea.extent = extent;
  const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
  const VkRect2D scissor{{0,0}, extent};
  const std::array<float, 4> rectangle{-1, -1, 2, 2};
  api.vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
  api.vkCmdSetScissor(commands, 0, 1, &scissor);
  api.vkCmdSetViewport(commands, 0, 1, &viewport);
  api.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  api.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                              0, 1, &samples, 0, nullptr);
  api.vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                          sizeof(rectangle), rectangle.data());
  api.vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                          sizeof(rectangle), sizeof(parameters), &parameters);
  api.vkCmdDraw(commands, 4, 1, 0, 0);
  api.vkCmdEndRenderPass(commands);
}
}  // namespace rex::ui::vulkan
