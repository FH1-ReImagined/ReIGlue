#ifdef NDEBUG
#undef NDEBUG
#endif
#include <rex/graphics/vulkan/pipeline_share.h>

#include <cassert>

// 0520 (FH1 #607): pipeline_share.h, with creation info as VulkanPipelineCache fills it.
using namespace rex::graphics::vulkan::pipeline_share;

namespace {

struct Info {
  VkPipelineShaderStageCreateInfo stages[2] = {};
  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  VkPipelineViewportStateCreateInfo viewport = {};
  VkPipelineRasterizationStateCreateInfo rasterization = {};
  VkPipelineMultisampleStateCreateInfo multisample = {};
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
  VkPipelineColorBlendAttachmentState attachments[2] = {};
  VkPipelineColorBlendStateCreateInfo color_blend = {};
  VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {};
  VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
  VkPipelineRenderingCreateInfo rendering = {};
  VkGraphicsPipelineCreateInfo info = {};

  Info() {
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = reinterpret_cast<VkShaderModule>(uintptr_t(0x10));
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = reinterpret_cast<VkShaderModule>(uintptr_t(0x20));
    stages[1].pName = "main";
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    viewport.viewportCount = viewport.scissorCount = 1;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    for (auto& attachment : attachments) {
      attachment.blendEnable = VK_TRUE;
      attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      attachment.colorWriteMask = 0xF;
    }
    color_blend.attachmentCount = 2;
    color_blend.pAttachments = attachments;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 2;
    rendering.pColorAttachmentFormats = formats;
    rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat =
        VK_FORMAT_D32_SFLOAT_S8_UINT;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = reinterpret_cast<VkPipelineLayout>(uintptr_t(0x30));
  }
  Info(const Info&) = delete;

  void Canonicalize(bool reads_facing = false, bool interlock = false) {
    CanonicalizeIgnoredState(rasterization, depth_stencil, attachments, 2, reads_facing, interlock);
  }
  // Modules 0x10 and 0x11 are created from the same code (two translations, one SPIR-V).
  std::string KeyOf() const {
    return Key(info, [](VkShaderModule module) {
      StageCode code;
      const auto value = reinterpret_cast<uintptr_t>(module);
      if (value == 0x10 || value == 0x11 || value == 0x20) {
        code.by_code = true;
        code.low = value == 0x20 ? 2 : 1;
        code.high = 7;
      }
      return code;
    });
  }
};

}  // namespace

int main() {
  // The same creation info, and the same code in another module: the same key.
  {
    Info a, b;
    assert(!a.KeyOf().empty() && a.KeyOf() == b.KeyOf());
    b.stages[0].module = reinterpret_cast<VkShaderModule>(uintptr_t(0x11));
    assert(a.KeyOf() == b.KeyOf());
    // Other code, a module compared by handle, another layout, format or state: another key.
    b.stages[0].module = reinterpret_cast<VkShaderModule>(uintptr_t(0x20));
    assert(a.KeyOf() != b.KeyOf());
    Info c;
    c.stages[1].module = reinterpret_cast<VkShaderModule>(uintptr_t(0x40));
    assert(a.KeyOf() != c.KeyOf());
    Info d;
    d.info.layout = reinterpret_cast<VkPipelineLayout>(uintptr_t(0x31));
    assert(a.KeyOf() != d.KeyOf());
    Info e;
    e.formats[1] = VK_FORMAT_R16G16B16A16_UNORM;
    assert(a.KeyOf() != e.KeyOf());
    Info f;
    f.depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
    assert(a.KeyOf() != f.KeyOf());
    Info g;
    g.attachments[1].colorWriteMask = 0x7;
    assert(a.KeyOf() != g.KeyOf());
    uint32_t mask = 0b1001;
    Info h;
    h.multisample.pSampleMask = &mask;
    assert(a.KeyOf() != h.KeyOf());
  }
  // Unexpected structures are never shared.
  {
    Info a;
    VkSpecializationInfo specialization = {};
    a.stages[1].pSpecializationInfo = &specialization;
    assert(a.KeyOf().empty());
    Info b;
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip = {};
    clip.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
    b.rendering.pNext = &clip;
    assert(b.KeyOf().empty());
    Info c;
    c.info.pNext = &clip;
    assert(c.KeyOf().empty());
  }
  // The alpha blend of an attachment that does not write alpha, and the factors of min and max.
  {
    Info a, b;
    a.attachments[0].colorWriteMask = b.attachments[0].colorWriteMask = 0x7;
    b.attachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    b.attachments[0].alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
    assert(a.KeyOf() != b.KeyOf());
    a.Canonicalize();
    b.Canonicalize();
    assert(a.KeyOf() == b.KeyOf());
    assert(a.attachments[0].blendEnable == VK_TRUE);
    // The colour blend is kept, and alpha is still blended where it is written.
    assert(a.attachments[0].srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
    assert(a.attachments[1].dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
    Info c, d;
    c.attachments[1].colorBlendOp = d.attachments[1].colorBlendOp = VK_BLEND_OP_MAX;
    d.attachments[1].srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    c.Canonicalize();
    d.Canonicalize();
    assert(c.KeyOf() == d.KeyOf());
    assert(c.attachments[1].colorBlendOp == VK_BLEND_OP_MAX);
    // A colour mask without RGB: the colour blend.
    Info e, f;
    e.attachments[0].colorWriteMask = f.attachments[0].colorWriteMask = 0x8;
    f.attachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    e.Canonicalize();
    f.Canonicalize();
    assert(e.KeyOf() == f.KeyOf());
    // Written channels keep their blend.
    Info g, h;
    h.attachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    g.Canonicalize();
    h.Canonicalize();
    assert(g.KeyOf() != h.KeyOf());
  }
  // The winding: without culling, stencil test and a shader reading it, only.
  {
    Info a, b;
    b.rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    a.Canonicalize();
    b.Canonicalize();
    assert(a.KeyOf() == b.KeyOf());
    Info c, d;
    d.rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    c.Canonicalize(true);
    d.Canonicalize(true);
    assert(c.KeyOf() != d.KeyOf());
    Info e, f;
    e.rasterization.cullMode = f.rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    f.rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    e.Canonicalize();
    f.Canonicalize();
    assert(e.KeyOf() != f.KeyOf());
    Info g, h;
    g.depth_stencil.stencilTestEnable = h.depth_stencil.stencilTestEnable = VK_TRUE;
    h.rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    g.Canonicalize();
    h.Canonicalize();
    assert(g.KeyOf() != h.KeyOf());
  }
  // The stencil operations of a culled face.
  {
    Info a, b;
    for (Info* i : {&a, &b}) {
      i->rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
      i->depth_stencil.stencilTestEnable = VK_TRUE;
      i->depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
      i->depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
    }
    b.depth_stencil.back.passOp = VK_STENCIL_OP_INVERT;
    a.Canonicalize();
    b.Canonicalize();
    assert(a.KeyOf() == b.KeyOf());
    assert(a.depth_stencil.back.passOp == VK_STENCIL_OP_REPLACE);
    // Not culled: kept.
    Info c, d;
    for (Info* i : {&c, &d}) {
      i->depth_stencil.stencilTestEnable = VK_TRUE;
    }
    d.depth_stencil.back.passOp = VK_STENCIL_OP_INVERT;
    c.Canonicalize();
    d.Canonicalize();
    assert(c.KeyOf() != d.KeyOf());
  }
  // Fragment shader interlock: nothing changes.
  {
    Info a, b;
    a.attachments[0].colorWriteMask = b.attachments[0].colorWriteMask = 0x7;
    b.attachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    b.rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    a.Canonicalize(false, true);
    b.Canonicalize(false, true);
    assert(a.KeyOf() != b.KeyOf());
    assert(a.rasterization.frontFace == VK_FRONT_FACE_CLOCKWISE);
  }
  return 0;
}
