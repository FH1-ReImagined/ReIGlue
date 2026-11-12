#pragma once

// 0520 (FH1 #607): pipelines of descriptions that give the driver the same creation info.
//
// Different pipeline descriptions can give vkCreateGraphicsPipelines the same creation info:
// guest shaders whose microcode differs where the translation does not look (the same SPIR-V),
// depth formats that the host represents with one format, and state that Vulkan defines to
// have no effect. VulkanPipelineCache creates one pipeline for each distinct creation info
// (the key below) and gives it to every description with that key.
//
// Header-only, with nothing but Vulkan's own headers, for tests/pipeline_share_test.cpp.

#include <cstdint>
#include <cstring>
#include <string>

// The types only (rex/ui/vulkan/api.h loads the functions).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

namespace rex::graphics::vulkan::pipeline_share {

// Writes state that Vulkan defines to have no effect one way, so that a pipeline created with
// the state before or after does the same. VulkanPipelineCache applies it to a copy that only
// builds the key; the driver is given each description's own state. With fragment shader
// interlock, which does the blending and the stencil test in the shader, nothing is changed:
// - the factors and operation of the colour or the alpha blend of an attachment whose write
//   mask leaves those channels out (they are blended and dropped), and the factors of a min or
//   max operation, which takes none. Blending itself stays on or off as it was;
// - the stencil operations of the face that is culled;
// - the winding, when it decides nothing: no culling, no stencil test (whose state, the dynamic
//   masks and references too, is per face) and no fragment shader reading gl_FrontFacing.
inline void CanonicalizeIgnoredState(VkPipelineRasterizationStateCreateInfo& rasterization,
                                     VkPipelineDepthStencilStateCreateInfo& depth_stencil,
                                     VkPipelineColorBlendAttachmentState* color_blend_attachments,
                                     uint32_t attachment_count, bool fragment_reads_facing,
                                     bool fragment_shader_interlock) {
  if (fragment_shader_interlock) {
    return;
  }
  constexpr VkColorComponentFlags kRgb =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
  auto canonicalize = [](bool written, VkBlendFactor& src, VkBlendFactor& dst, VkBlendOp& op) {
    if (!written) {
      src = VK_BLEND_FACTOR_ONE;
      dst = VK_BLEND_FACTOR_ZERO;
      op = VK_BLEND_OP_ADD;
    } else if (op == VK_BLEND_OP_MIN || op == VK_BLEND_OP_MAX) {
      src = VK_BLEND_FACTOR_ONE;
      dst = VK_BLEND_FACTOR_ZERO;
    }
  };
  for (uint32_t i = 0; i < attachment_count; ++i) {
    VkPipelineColorBlendAttachmentState& attachment = color_blend_attachments[i];
    if (!attachment.blendEnable) {
      continue;
    }
    canonicalize((attachment.colorWriteMask & kRgb) != 0, attachment.srcColorBlendFactor,
                 attachment.dstColorBlendFactor, attachment.colorBlendOp);
    canonicalize((attachment.colorWriteMask & VK_COLOR_COMPONENT_A_BIT) != 0,
                 attachment.srcAlphaBlendFactor, attachment.dstAlphaBlendFactor,
                 attachment.alphaBlendOp);
  }
  if (rasterization.rasterizerDiscardEnable) {
    return;
  }
  if (depth_stencil.stencilTestEnable) {
    if (rasterization.cullMode == VK_CULL_MODE_BACK_BIT) {
      depth_stencil.back = depth_stencil.front;
    } else if (rasterization.cullMode == VK_CULL_MODE_FRONT_BIT) {
      depth_stencil.front = depth_stencil.back;
    }
  }
  if (rasterization.cullMode == VK_CULL_MODE_NONE && !depth_stencil.stencilTestEnable &&
      !fragment_reads_facing) {
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  }
}

// The code of a shader stage for the key: a hash of the SPIR-V of a module created from it
// (two modules of the same code are the same stage), or none, for a module that is only
// compared by handle.
struct StageCode {
  bool by_code = false;
  uint64_t low = 0, high = 0;
};

// Everything the pipeline is created from, as bytes (empty: not to be shared). Only the
// structures VulkanPipelineCache::EnsurePipelineCreated fills are expected: one
// VkPipelineRenderingCreateInfo or nothing in pNext, no vertex input, no specialization.
// `stage_code(module)` gives the StageCode of a module.
template <typename StageCodeOf>
std::string Key(const VkGraphicsPipelineCreateInfo& info, StageCodeOf stage_code) {
  std::string key;
  key.reserve(512);
  auto put = [&key](uint64_t value) {
    key.append(reinterpret_cast<const char*>(&value), sizeof(value));
  };
  auto put_float = [&put](float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put(bits);
  };
  const auto* rendering = static_cast<const VkPipelineRenderingCreateInfo*>(info.pNext);
  if (rendering &&
      (rendering->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO || rendering->pNext)) {
    return {};
  }
  if (!info.pVertexInputState || !info.pInputAssemblyState || !info.pViewportState ||
      !info.pRasterizationState || !info.pMultisampleState || !info.pDepthStencilState ||
      !info.pColorBlendState || !info.pDynamicState || info.basePipelineHandle != VK_NULL_HANDLE) {
    return {};
  }
  put(info.flags);
  put(info.stageCount);
  for (uint32_t i = 0; i < info.stageCount; ++i) {
    const VkPipelineShaderStageCreateInfo& stage = info.pStages[i];
    if (stage.pNext || stage.pSpecializationInfo || !stage.pName ||
        std::strcmp(stage.pName, "main")) {
      return {};
    }
    put(stage.flags);
    put(stage.stage);
    const StageCode code = stage_code(stage.module);
    if (code.by_code) {
      if (!code.low && !code.high) {
        return {};
      }
      put(1);
      put(code.low);
      put(code.high);
    } else {
      put(0);
      put(uint64_t(stage.module));
    }
  }
  const auto& vertex_input = *info.pVertexInputState;
  if (vertex_input.pNext || vertex_input.vertexBindingDescriptionCount ||
      vertex_input.vertexAttributeDescriptionCount) {
    return {};
  }
  put(info.pInputAssemblyState->topology);
  put(info.pInputAssemblyState->primitiveRestartEnable);
  put(info.pTessellationState ? 1 + uint64_t(info.pTessellationState->patchControlPoints) : 0);
  put(info.pViewportState->viewportCount);
  put(info.pViewportState->scissorCount);
  const auto& rasterization = *info.pRasterizationState;
  if (rasterization.pNext) {
    return {};
  }
  put(rasterization.depthClampEnable);
  put(rasterization.rasterizerDiscardEnable);
  put(rasterization.polygonMode);
  put(rasterization.cullMode);
  put(rasterization.frontFace);
  put(rasterization.depthBiasEnable);
  put_float(rasterization.depthBiasConstantFactor);
  put_float(rasterization.depthBiasClamp);
  put_float(rasterization.depthBiasSlopeFactor);
  put_float(rasterization.lineWidth);
  const auto& multisample = *info.pMultisampleState;
  if (multisample.pNext) {
    return {};
  }
  put(multisample.rasterizationSamples);
  put(multisample.sampleShadingEnable);
  put_float(multisample.minSampleShading);
  put(multisample.pSampleMask ? 1 + uint64_t(*multisample.pSampleMask) : 0);
  put(multisample.alphaToCoverageEnable);
  put(multisample.alphaToOneEnable);
  const auto& depth_stencil = *info.pDepthStencilState;
  put(depth_stencil.depthTestEnable);
  put(depth_stencil.depthWriteEnable);
  put(depth_stencil.depthCompareOp);
  put(depth_stencil.depthBoundsTestEnable);
  put(depth_stencil.stencilTestEnable);
  for (const VkStencilOpState* face : {&depth_stencil.front, &depth_stencil.back}) {
    put(face->failOp);
    put(face->passOp);
    put(face->depthFailOp);
    put(face->compareOp);
    put(face->compareMask);
    put(face->writeMask);
    put(face->reference);
  }
  put_float(depth_stencil.minDepthBounds);
  put_float(depth_stencil.maxDepthBounds);
  const auto& color_blend = *info.pColorBlendState;
  if (color_blend.pNext) {
    return {};
  }
  put(color_blend.logicOpEnable);
  put(color_blend.logicOp);
  put(color_blend.attachmentCount);
  for (uint32_t i = 0; i < color_blend.attachmentCount; ++i) {
    const VkPipelineColorBlendAttachmentState& attachment = color_blend.pAttachments[i];
    put(attachment.blendEnable);
    put(attachment.srcColorBlendFactor);
    put(attachment.dstColorBlendFactor);
    put(attachment.colorBlendOp);
    put(attachment.srcAlphaBlendFactor);
    put(attachment.dstAlphaBlendFactor);
    put(attachment.alphaBlendOp);
    put(attachment.colorWriteMask);
  }
  for (float constant : color_blend.blendConstants) {
    put_float(constant);
  }
  put(info.pDynamicState->dynamicStateCount);
  for (uint32_t i = 0; i < info.pDynamicState->dynamicStateCount; ++i) {
    put(info.pDynamicState->pDynamicStates[i]);
  }
  put(uint64_t(info.layout));
  put(uint64_t(info.renderPass));
  put(info.subpass);
  if (rendering) {
    put(rendering->viewMask);
    put(rendering->colorAttachmentCount);
    for (uint32_t i = 0; i < rendering->colorAttachmentCount; ++i) {
      put(rendering->pColorAttachmentFormats[i]);
    }
    put(rendering->depthAttachmentFormat);
    put(rendering->stencilAttachmentFormat);
  } else {
    put(UINT64_MAX);
  }
  return key;
}

}  // namespace rex::graphics::vulkan::pipeline_share
