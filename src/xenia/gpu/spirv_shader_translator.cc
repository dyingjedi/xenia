/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2016 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/spirv_shader_translator.h"

#include <gflags/gflags.h>

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/gpu/spirv/passes/control_flow_analysis_pass.h"
#include "xenia/gpu/spirv/passes/control_flow_simplification_pass.h"

DEFINE_bool(spv_validate, false, "Validate SPIR-V shaders after generation");

namespace xe {
namespace gpu {
using namespace ucode;

constexpr uint32_t kMaxInterpolators = 16;
constexpr uint32_t kMaxTemporaryRegisters = 64;

using spv::GLSLstd450;
using spv::Id;
using spv::Op;

SpirvShaderTranslator::SpirvShaderTranslator() {
  compiler_.AddPass(std::make_unique<spirv::ControlFlowSimplificationPass>());
  compiler_.AddPass(std::make_unique<spirv::ControlFlowAnalysisPass>());
}

SpirvShaderTranslator::~SpirvShaderTranslator() = default;

void SpirvShaderTranslator::StartTranslation() {
  // Create a new builder.
  builder_ = std::make_unique<spv::Builder>(0xFFFFFFFF);
  auto& b = *builder_;

  // Import required modules.
  glsl_std_450_instruction_set_ = b.import("GLSL.std.450");

  // Configure environment.
  b.setSource(spv::SourceLanguage::SourceLanguageUnknown, 0);
  b.setMemoryModel(spv::AddressingModel::AddressingModelLogical,
                   spv::MemoryModel::MemoryModelGLSL450);
  b.addCapability(spv::Capability::CapabilityShader);
  b.addCapability(spv::Capability::CapabilityGenericPointer);

  if (is_vertex_shader()) {
    b.addCapability(spv::Capability::CapabilityClipDistance);
    b.addCapability(spv::Capability::CapabilityCullDistance);
  }
  if (is_pixel_shader()) {
    b.addCapability(spv::Capability::CapabilityDerivativeControl);
  }

  bool_type_ = b.makeBoolType();
  float_type_ = b.makeFloatType(32);
  int_type_ = b.makeIntType(32);
  uint_type_ = b.makeUintType(32);
  vec2_float_type_ = b.makeVectorType(float_type_, 2);
  vec3_float_type_ = b.makeVectorType(float_type_, 3);
  vec4_float_type_ = b.makeVectorType(float_type_, 4);
  vec4_uint_type_ = b.makeVectorType(uint_type_, 4);
  vec4_bool_type_ = b.makeVectorType(bool_type_, 4);

  vec4_float_one_ = b.makeCompositeConstant(
      vec4_float_type_,
      std::vector<Id>({b.makeFloatConstant(1.f), b.makeFloatConstant(1.f),
                       b.makeFloatConstant(1.f), b.makeFloatConstant(1.f)}));
  vec4_float_zero_ = b.makeCompositeConstant(
      vec4_float_type_,
      std::vector<Id>({b.makeFloatConstant(0.f), b.makeFloatConstant(0.f),
                       b.makeFloatConstant(0.f), b.makeFloatConstant(0.f)}));

  spv::Block* function_block = nullptr;
  translated_main_ =
      b.makeFunctionEntry(spv::NoPrecision, b.makeVoidType(), "translated_main",
                          {}, {}, &function_block);

  registers_type_ = b.makeArrayType(vec4_float_type_,
                                    b.makeUintConstant(register_count()), 0);
  registers_ptr_ = b.createVariable(spv::StorageClass::StorageClassFunction,
                                    registers_type_, "r");

  aL_ = b.createVariable(spv::StorageClass::StorageClassFunction,
                         vec4_uint_type_, "aL");

  p0_ = b.createVariable(spv::StorageClass::StorageClassFunction, bool_type_,
                         "p0");
  ps_ = b.createVariable(spv::StorageClass::StorageClassFunction, float_type_,
                         "ps");
  pv_ = b.createVariable(spv::StorageClass::StorageClassFunction,
                         vec4_float_type_, "pv");
  a0_ = b.createVariable(spv::StorageClass::StorageClassFunction, int_type_,
                         "a0");

  // Uniform constants.
  Id float_consts_type =
      b.makeArrayType(vec4_float_type_, b.makeUintConstant(512), 1);
  Id loop_consts_type = b.makeArrayType(uint_type_, b.makeUintConstant(32), 1);
  Id bool_consts_type = b.makeArrayType(uint_type_, b.makeUintConstant(8), 1);

  Id consts_struct_type = b.makeStructType(
      {float_consts_type, loop_consts_type, bool_consts_type}, "consts_type");
  b.addDecoration(consts_struct_type, spv::Decoration::DecorationBlock);

  // Constants member decorations.
  b.addMemberDecoration(consts_struct_type, 0,
                        spv::Decoration::DecorationOffset, 0);
  b.addMemberDecoration(consts_struct_type, 0,
                        spv::Decoration::DecorationArrayStride,
                        4 * sizeof(float));
  b.addMemberName(consts_struct_type, 0, "float_consts");

  b.addMemberDecoration(consts_struct_type, 1,
                        spv::Decoration::DecorationOffset,
                        512 * 4 * sizeof(float));
  b.addMemberDecoration(consts_struct_type, 1,
                        spv::Decoration::DecorationArrayStride,
                        sizeof(uint32_t));
  b.addMemberName(consts_struct_type, 1, "loop_consts");

  b.addMemberDecoration(consts_struct_type, 2,
                        spv::Decoration::DecorationOffset,
                        512 * 4 * sizeof(float) + 32 * sizeof(uint32_t));
  b.addMemberDecoration(consts_struct_type, 2,
                        spv::Decoration::DecorationArrayStride,
                        sizeof(uint32_t));
  b.addMemberName(consts_struct_type, 2, "bool_consts");

  consts_ = b.createVariable(spv::StorageClass::StorageClassUniform,
                             consts_struct_type, "consts");

  b.addDecoration(consts_, spv::Decoration::DecorationDescriptorSet, 0);
  if (is_vertex_shader()) {
    b.addDecoration(consts_, spv::Decoration::DecorationBinding, 0);
  } else if (is_pixel_shader()) {
    b.addDecoration(consts_, spv::Decoration::DecorationBinding, 1);
  }

  // Push constants, represented by SpirvPushConstants.
  Id push_constants_type = b.makeStructType(
      {vec4_float_type_, vec4_float_type_, vec4_float_type_, uint_type_},
      "push_consts_type");
  b.addDecoration(push_constants_type, spv::Decoration::DecorationBlock);

  // float4 window_scale;
  b.addMemberDecoration(
      push_constants_type, 0, spv::Decoration::DecorationOffset,
      static_cast<int>(offsetof(SpirvPushConstants, window_scale)));
  b.addMemberName(push_constants_type, 0, "window_scale");
  // float4 vtx_fmt;
  b.addMemberDecoration(
      push_constants_type, 1, spv::Decoration::DecorationOffset,
      static_cast<int>(offsetof(SpirvPushConstants, vtx_fmt)));
  b.addMemberName(push_constants_type, 1, "vtx_fmt");
  // float4 alpha_test;
  b.addMemberDecoration(
      push_constants_type, 2, spv::Decoration::DecorationOffset,
      static_cast<int>(offsetof(SpirvPushConstants, alpha_test)));
  b.addMemberName(push_constants_type, 2, "alpha_test");
  // uint ps_param_gen;
  b.addMemberDecoration(
      push_constants_type, 3, spv::Decoration::DecorationOffset,
      static_cast<int>(offsetof(SpirvPushConstants, ps_param_gen)));
  b.addMemberName(push_constants_type, 3, "ps_param_gen");
  push_consts_ = b.createVariable(spv::StorageClass::StorageClassPushConstant,
                                  push_constants_type, "push_consts");

  // Texture bindings
  Id tex_t[] = {b.makeSampledImageType(b.makeImageType(
                    float_type_, spv::Dim::Dim1D, false, false, false, 1,
                    spv::ImageFormat::ImageFormatUnknown)),
                b.makeSampledImageType(b.makeImageType(
                    float_type_, spv::Dim::Dim2D, false, false, false, 1,
                    spv::ImageFormat::ImageFormatUnknown)),
                b.makeSampledImageType(b.makeImageType(
                    float_type_, spv::Dim::Dim3D, false, false, false, 1,
                    spv::ImageFormat::ImageFormatUnknown)),
                b.makeSampledImageType(b.makeImageType(
                    float_type_, spv::Dim::DimCube, false, false, false, 1,
                    spv::ImageFormat::ImageFormatUnknown))};

  Id tex_a_t[] = {b.makeArrayType(tex_t[0], b.makeUintConstant(32), 0),
                  b.makeArrayType(tex_t[1], b.makeUintConstant(32), 0),
                  b.makeArrayType(tex_t[2], b.makeUintConstant(32), 0),
                  b.makeArrayType(tex_t[3], b.makeUintConstant(32), 0)};

  for (int i = 0; i < 4; i++) {
    tex_[i] = b.createVariable(spv::StorageClass::StorageClassUniformConstant,
                               tex_a_t[i],
                               xe::format_string("textures%dD", i + 1).c_str());
    b.addDecoration(tex_[i], spv::Decoration::DecorationDescriptorSet, 1);
    b.addDecoration(tex_[i], spv::Decoration::DecorationBinding, i);
  }

  // Interpolators.
  Id interpolators_type = b.makeArrayType(
      vec4_float_type_, b.makeUintConstant(kMaxInterpolators), 0);
  if (is_vertex_shader()) {
    // Vertex inputs/outputs.
    for (const auto& binding : vertex_bindings()) {
      for (const auto& attrib : binding.attributes) {
        Id attrib_type = 0;
        switch (attrib.fetch_instr.attributes.data_format) {
          case VertexFormat::k_32:
          case VertexFormat::k_32_FLOAT:
            attrib_type = float_type_;
            break;
          case VertexFormat::k_16_16:
          case VertexFormat::k_32_32:
          case VertexFormat::k_16_16_FLOAT:
          case VertexFormat::k_32_32_FLOAT:
            attrib_type = vec2_float_type_;
            break;
          case VertexFormat::k_10_11_11:
          case VertexFormat::k_11_11_10:
          case VertexFormat::k_32_32_32_FLOAT:
            attrib_type = vec3_float_type_;
            break;
          case VertexFormat::k_8_8_8_8:
          case VertexFormat::k_2_10_10_10:
          case VertexFormat::k_16_16_16_16:
          case VertexFormat::k_32_32_32_32:
          case VertexFormat::k_16_16_16_16_FLOAT:
          case VertexFormat::k_32_32_32_32_FLOAT:
            attrib_type = vec4_float_type_;
            break;
          default:
            assert_always();
        }

        auto attrib_var = b.createVariable(
            spv::StorageClass::StorageClassInput, attrib_type,
            xe::format_string("vf%d_%d", binding.fetch_constant,
                              attrib.fetch_instr.attributes.offset)
                .c_str());
        b.addDecoration(attrib_var, spv::Decoration::DecorationLocation,
                        attrib.attrib_index);

        interface_ids_.push_back(attrib_var);
        vertex_binding_map_[binding.fetch_constant]
                           [attrib.fetch_instr.attributes.offset] = attrib_var;
      }
    }

    interpolators_ = b.createVariable(spv::StorageClass::StorageClassOutput,
                                      interpolators_type, "interpolators");
    b.addDecoration(interpolators_, spv::Decoration::DecorationLocation, 0);
    for (uint32_t i = 0; i < std::min(register_count(), kMaxInterpolators);
         i++) {
      // Zero interpolators.
      auto ptr = b.createAccessChain(spv::StorageClass::StorageClassOutput,
                                     interpolators_,
                                     std::vector<Id>({b.makeUintConstant(i)}));
      b.createStore(vec4_float_zero_, ptr);
    }

    pos_ = b.createVariable(spv::StorageClass::StorageClassOutput,
                            vec4_float_type_, "gl_Position");
    b.addDecoration(pos_, spv::Decoration::DecorationBuiltIn,
                    spv::BuiltIn::BuiltInPosition);

    vertex_id_ = b.createVariable(spv::StorageClass::StorageClassInput,
                                  int_type_, "gl_VertexId");
    b.addDecoration(vertex_id_, spv::Decoration::DecorationBuiltIn,
                    spv::BuiltIn::BuiltInVertexId);

    interface_ids_.push_back(interpolators_);
    interface_ids_.push_back(pos_);
    interface_ids_.push_back(vertex_id_);

    auto vertex_id = b.createLoad(vertex_id_);
    vertex_id = b.createUnaryOp(spv::Op::OpConvertSToF, float_type_, vertex_id);
    auto r0_ptr = b.createAccessChain(spv::StorageClass::StorageClassFunction,
                                      registers_ptr_,
                                      std::vector<Id>({b.makeUintConstant(0)}));
    auto r0 = b.createLoad(r0_ptr);
    r0 = b.createCompositeInsert(vertex_id, r0, vec4_float_type_,
                                 std::vector<uint32_t>({0}));
    b.createStore(r0, r0_ptr);
  } else {
    // Pixel inputs from vertex shader.
    interpolators_ = b.createVariable(spv::StorageClass::StorageClassInput,
                                      interpolators_type, "interpolators");
    b.addDecoration(interpolators_, spv::Decoration::DecorationLocation, 0);

    // Pixel fragment outputs (one per render target).
    Id frag_outputs_type =
        b.makeArrayType(vec4_float_type_, b.makeUintConstant(4), 0);
    frag_outputs_ = b.createVariable(spv::StorageClass::StorageClassOutput,
                                     frag_outputs_type, "oC");
    b.addDecoration(frag_outputs_, spv::Decoration::DecorationLocation, 0);

    frag_depth_ = b.createVariable(spv::StorageClass::StorageClassOutput,
                                   float_type_, "gl_FragDepth");
    b.addDecoration(frag_depth_, spv::Decoration::DecorationBuiltIn,
                    spv::BuiltIn::BuiltInFragDepth);

    interface_ids_.push_back(interpolators_);
    interface_ids_.push_back(frag_outputs_);
    interface_ids_.push_back(frag_depth_);
    // TODO(benvanik): frag depth, etc.

    // Copy interpolators to r[0..16].
    // TODO: Need physical addressing in order to do this.
    // b.createNoResultOp(spv::Op::OpCopyMemorySized,
    //                   {registers_ptr_, interpolators_,
    //                    b.makeUintConstant(16 * 4 * sizeof(float))});
    for (uint32_t i = 0; i < std::min(register_count(), kMaxInterpolators);
         i++) {
      // For now, copy interpolators register-by-register :/
      auto idx = b.makeUintConstant(i);
      auto i_a = b.createAccessChain(spv::StorageClass::StorageClassInput,
                                     interpolators_, std::vector<Id>({idx}));
      auto r_a = b.createAccessChain(spv::StorageClass::StorageClassFunction,
                                     registers_ptr_, std::vector<Id>({idx}));
      b.createNoResultOp(spv::Op::OpCopyMemory, std::vector<Id>({r_a, i_a}));
    }

    // Setup ps_param_gen
    auto ps_param_gen_idx_ptr = b.createAccessChain(
        spv::StorageClass::StorageClassPushConstant, push_consts_,
        std::vector<Id>({b.makeUintConstant(3)}));
    auto ps_param_gen_idx = b.createLoad(ps_param_gen_idx_ptr);

    auto frag_coord = b.createVariable(spv::StorageClass::StorageClassInput,
                                       vec4_float_type_, "gl_FragCoord");
    b.addDecoration(frag_coord, spv::Decoration::DecorationBuiltIn,
                    spv::BuiltIn::BuiltInFragCoord);

    auto point_coord = b.createVariable(spv::StorageClass::StorageClassInput,
                                        vec2_float_type_, "gl_PointCoord");
    b.addDecoration(point_coord, spv::Decoration::DecorationBuiltIn,
                    spv::BuiltIn::BuiltInPointCoord);
    interface_ids_.push_back(frag_coord);
    interface_ids_.push_back(point_coord);

    auto param = b.createOp(spv::Op::OpVectorShuffle, vec4_float_type_,
                            {frag_coord, point_coord, 0, 1, 4, 5});
    /*
    // TODO: gl_FrontFacing
    auto param_x = b.createCompositeExtract(param, float_type_, 0);
    auto param_x_inv = b.createBinOp(spv::Op::OpFMul, float_type_, param_x,
                                     b.makeFloatConstant(-1.f));
    param_x = b.createCompositeInsert(param_x_inv, param, vec4_float_type_, 0);
    */

    auto cond = b.createBinOp(spv::Op::OpINotEqual, bool_type_,
                              ps_param_gen_idx, b.makeUintConstant(-1));
    spv::Builder::If ifb(cond, b);

    // FYI: We do this instead of r[ps_param_gen_idx] because that causes
    // nvidia to move all registers into local memory (slow!)
    for (uint32_t i = 0; i < std::min(register_count(), kMaxInterpolators);
         i++) {
      auto reg_ptr = b.createAccessChain(
          spv::StorageClass::StorageClassFunction, registers_ptr_,
          std::vector<Id>({b.makeUintConstant(i)}));

      auto cond = b.createBinOp(spv::Op::OpIEqual, bool_type_, ps_param_gen_idx,
                                b.makeUintConstant(i));
      auto reg = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, cond, param,
                               b.createLoad(reg_ptr));
      b.createStore(reg, reg_ptr);
    }

    ifb.makeEndIf();
  }
}

std::vector<uint8_t> SpirvShaderTranslator::CompleteTranslation() {
  auto& b = *builder_;

  assert_false(open_predicated_block_);
  auto block = &b.makeNewBlock();
  b.createBranch(block);
  b.makeReturn(false);

  // main() entry point.
  auto mainFn = b.makeMain();
  if (is_vertex_shader()) {
    auto entry = b.addEntryPoint(spv::ExecutionModel::ExecutionModelVertex,
                                 mainFn, "main");

    for (auto id : interface_ids_) {
      entry->addIdOperand(id);
    }
  } else {
    auto entry = b.addEntryPoint(spv::ExecutionModel::ExecutionModelFragment,
                                 mainFn, "main");
    b.addExecutionMode(mainFn, spv::ExecutionModeOriginUpperLeft);

    for (auto id : interface_ids_) {
      entry->addIdOperand(id);
    }
  }

  // TODO(benvanik): transform feedback.
  if (false) {
    b.addCapability(spv::Capability::CapabilityTransformFeedback);
    b.addExecutionMode(mainFn, spv::ExecutionMode::ExecutionModeXfb);
  }

  b.createFunctionCall(translated_main_, std::vector<Id>({}));
  if (is_vertex_shader()) {
    // gl_Position transform
    auto vtx_fmt_ptr = b.createAccessChain(
        spv::StorageClass::StorageClassPushConstant, push_consts_,
        std::vector<Id>({b.makeUintConstant(1)}));
    auto window_scale_ptr = b.createAccessChain(
        spv::StorageClass::StorageClassPushConstant, push_consts_,
        std::vector<Id>({b.makeUintConstant(0)}));
    auto vtx_fmt = b.createLoad(vtx_fmt_ptr);
    auto window_scale = b.createLoad(window_scale_ptr);

    auto p = b.createLoad(pos_);
    auto c = b.createBinOp(spv::Op::OpFOrdNotEqual, vec4_bool_type_, vtx_fmt,
                           vec4_float_zero_);

    // pos.w = vtx_fmt.w == 0.0 ? 1.0 / pos.w : pos.w
    auto c_w = b.createCompositeExtract(c, bool_type_, 3);
    auto p_w = b.createCompositeExtract(p, float_type_, 3);
    auto p_w_inv = b.createBinOp(spv::Op::OpFDiv, float_type_,
                                 b.makeFloatConstant(1.f), p_w);
    p_w = b.createTriOp(spv::Op::OpSelect, float_type_, c_w, p_w, p_w_inv);

    // pos.xyz = vtx_fmt.xyz != 0.0 ? pos.xyz / pos.w : pos.xyz
    auto p_all_w = b.smearScalar(spv::NoPrecision, p_w, vec4_float_type_);
    auto p_inv = b.createBinOp(spv::Op::OpFDiv, vec4_float_type_, p, p_all_w);
    p = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c, p_inv, p);

    // Reinsert w
    p = b.createCompositeInsert(p_w, p, vec4_float_type_, 3);

    // Apply window scaling
    // pos.xy *= window_scale.xy
    auto p_scaled =
        b.createBinOp(spv::Op::OpFMul, vec4_float_type_, p, window_scale);
    p = b.createOp(spv::Op::OpVectorShuffle, vec4_float_type_,
                   {p, p_scaled, 4, 5, 2, 3});

    b.createStore(p, pos_);
  } else {
    // Alpha test
    auto alpha_test_enabled = b.createCompositeExtract(
        push_consts_, float_type_, std::vector<uint32_t>{2, 0});
    auto alpha_test_func = b.createCompositeExtract(
        push_consts_, float_type_, std::vector<uint32_t>{2, 1});
    auto alpha_test_ref = b.createCompositeExtract(push_consts_, float_type_,
                                                   std::vector<uint32_t>{2, 2});
    alpha_test_func =
        b.createUnaryOp(spv::Op::OpConvertFToU, uint_type_, alpha_test_func);
    auto oC0_alpha = b.createCompositeExtract(frag_outputs_, float_type_,
                                              std::vector<uint32_t>({0, 3}));

    auto cond = b.createBinOp(spv::Op::OpFOrdEqual, bool_type_,
                              alpha_test_enabled, b.makeFloatConstant(1.f));
    spv::Builder::If alpha_if(cond, b);

    std::vector<spv::Block*> switch_segments;
    b.makeSwitch(alpha_test_func, 8, std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7}),
                 std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7}), 7,
                 switch_segments);

    const static spv::Op alpha_op_map[] = {
        spv::Op::OpNop,
        spv::Op::OpFOrdGreaterThanEqual,
        spv::Op::OpFOrdNotEqual,
        spv::Op::OpFOrdGreaterThan,
        spv::Op::OpFOrdLessThanEqual,
        spv::Op::OpFOrdEqual,
        spv::Op::OpFOrdLessThan,
        spv::Op::OpNop,
    };

    // if (alpha_func == 0) passes = false;
    b.nextSwitchSegment(switch_segments, 0);
    b.makeDiscard();
    b.addSwitchBreak();

    for (int i = 1; i < 7; i++) {
      b.nextSwitchSegment(switch_segments, i);
      auto cond =
          b.createBinOp(alpha_op_map[i], bool_type_, oC0_alpha, alpha_test_ref);
      spv::Builder::If discard_if(cond, b);
      b.makeDiscard();
      discard_if.makeEndIf();
      b.addSwitchBreak();
    }

    // if (alpha_func == 7) passes = true;
    b.nextSwitchSegment(switch_segments, 7);
    b.endSwitch(switch_segments);

    alpha_if.makeEndIf();
  }

  b.makeReturn(false);

  // Compile the spv IR
  compiler_.Compile(b.getModule());

  std::vector<uint32_t> spirv_words;
  b.dump(spirv_words);

  // Cleanup builder.
  builder_.reset();

  interface_ids_.clear();

  // Copy bytes out.
  // TODO(benvanik): avoid copy?
  std::vector<uint8_t> spirv_bytes;
  spirv_bytes.resize(spirv_words.size() * 4);
  std::memcpy(spirv_bytes.data(), spirv_words.data(), spirv_bytes.size());
  return spirv_bytes;
}

void SpirvShaderTranslator::PostTranslation(Shader* shader) {
  // Validation.
  if (FLAGS_spv_validate) {
    auto validation = validator_.Validate(
        reinterpret_cast<const uint32_t*>(shader->translated_binary().data()),
        shader->translated_binary().size() / 4);
    if (validation->has_error()) {
      XELOGE("SPIR-V Shader Validation failed! Error: %s",
             validation->error_string());
    }
  }

  // TODO(benvanik): only if needed? could be slowish.
  auto disasm = disassembler_.Disassemble(
      reinterpret_cast<const uint32_t*>(shader->translated_binary().data()),
      shader->translated_binary().size() / 4);
  if (disasm->has_error()) {
    XELOGE("Failed to disassemble SPIRV - invalid?");
  } else {
    set_host_disassembly(shader, disasm->to_string());
  }
}

void SpirvShaderTranslator::PreProcessControlFlowInstruction(
    uint32_t cf_index, const ControlFlowInstruction& instr) {
  auto& b = *builder_;

  if (cf_blocks_.find(cf_index) == cf_blocks_.end()) {
    CFBlock block;
    block.block = &b.makeNewBlock();
    cf_blocks_[cf_index] = block;
  } else {
    cf_blocks_[cf_index].block = &b.makeNewBlock();
  }

  if (instr.opcode() == ControlFlowOpcode::kCondJmp) {
    auto cf_block = cf_blocks_.find(instr.cond_jmp.address());
    if (cf_block == cf_blocks_.end()) {
      CFBlock block;
      block.prev_dominates = false;
      cf_blocks_[instr.cond_jmp.address()] = block;
    } else {
      cf_block->second.prev_dominates = false;
    }
  } else if (instr.opcode() == ControlFlowOpcode::kLoopStart) {
    // TODO
  }
}

void SpirvShaderTranslator::ProcessLabel(uint32_t cf_index) {
  auto& b = *builder_;
}

void SpirvShaderTranslator::ProcessControlFlowInstructionBegin(
    uint32_t cf_index) {
  auto& b = *builder_;

  if (cf_index == 0) {
    // Kind of cheaty, but emit a branch to the first block.
    b.createBranch(cf_blocks_[cf_index].block);
  }
}

void SpirvShaderTranslator::ProcessControlFlowInstructionEnd(
    uint32_t cf_index) {
  auto& b = *builder_;
}

void SpirvShaderTranslator::ProcessControlFlowNopInstruction() {
  auto& b = *builder_;

  // b.createNoResultOp(spv::Op::OpNop);
}

void SpirvShaderTranslator::ProcessExecInstructionBegin(
    const ParsedExecInstruction& instr) {
  auto& b = *builder_;

  assert_false(open_predicated_block_);
  open_predicated_block_ = false;
  predicated_block_cond_ = false;
  predicated_block_end_ = nullptr;

  // Head has the logic to check if the body should execute.
  auto head = cf_blocks_[instr.dword_index].block;
  b.setBuildPoint(head);
  auto body = head;
  switch (instr.type) {
    case ParsedExecInstruction::Type::kUnconditional: {
      // No need to do anything.
    } break;
    case ParsedExecInstruction::Type::kConditional: {
      // Based off of bool_consts
      std::vector<Id> offsets;
      offsets.push_back(b.makeUintConstant(2));  // bool_consts
      offsets.push_back(b.makeUintConstant(instr.bool_constant_index / 32));
      auto v = b.createAccessChain(spv::StorageClass::StorageClassUniform,
                                   consts_, offsets);
      v = b.createLoad(v);

      // Bitfield extract the bool constant.
      // FIXME: NVidia's compiler seems to be broken on this instruction?
      /*
      v = b.createTriOp(spv::Op::OpBitFieldUExtract, uint_type_, v,
                        b.makeUintConstant(instr.bool_constant_index % 32),
                        b.makeUintConstant(1));

      auto cond = b.createBinOp(spv::Op::OpIEqual, bool_type_, v,
                                b.makeUintConstant(instr.condition ? 1 : 0));
      */
      v = b.createBinOp(
          spv::Op::OpBitwiseAnd, uint_type_, v,
          b.makeUintConstant(1 << (instr.bool_constant_index % 32)));
      auto cond = b.createBinOp(
          instr.condition ? spv::Op::OpINotEqual : spv::Op::OpIEqual,
          bool_type_, v, b.makeUintConstant(0));

      // Conditional branch
      assert_true(cf_blocks_.size() > instr.dword_index + 1);
      body = &b.makeNewBlock();

      auto next_block = cf_blocks_[instr.dword_index + 1];
      if (next_block.prev_dominates) {
        b.createSelectionMerge(next_block.block, spv::SelectionControlMaskNone);
      }
      b.createConditionalBranch(cond, body, next_block.block);
    } break;
    case ParsedExecInstruction::Type::kPredicated: {
      // Branch based on p0.
      assert_true(cf_blocks_.size() > instr.dword_index + 1);
      body = &b.makeNewBlock();
      auto cond =
          b.createBinOp(spv::Op::OpLogicalEqual, bool_type_, b.createLoad(p0_),
                        b.makeBoolConstant(instr.condition));

      auto next_block = cf_blocks_[instr.dword_index + 1];
      if (next_block.prev_dominates) {
        b.createSelectionMerge(next_block.block, spv::SelectionControlMaskNone);
      }
      b.createConditionalBranch(cond, body, next_block.block);

    } break;
  }
  b.setBuildPoint(body);
}

void SpirvShaderTranslator::ProcessExecInstructionEnd(
    const ParsedExecInstruction& instr) {
  auto& b = *builder_;

  if (open_predicated_block_) {
    b.createBranch(predicated_block_end_);
    b.setBuildPoint(predicated_block_end_);
    open_predicated_block_ = false;
    predicated_block_cond_ = false;
    predicated_block_end_ = nullptr;
  }

  if (instr.is_end) {
    b.makeReturn(false);
  } else {
    assert_true(cf_blocks_.size() > instr.dword_index + 1);
    b.createBranch(cf_blocks_[instr.dword_index + 1].block);
  }
}

void SpirvShaderTranslator::ProcessLoopStartInstruction(
    const ParsedLoopStartInstruction& instr) {
  auto& b = *builder_;

  auto head = cf_blocks_[instr.dword_index].block;
  b.setBuildPoint(head);

  // TODO: Emit a spv LoopMerge
  // (need to know the continue target and merge target beforehand though)

  EmitUnimplementedTranslationError();

  assert_true(cf_blocks_.size() > instr.dword_index + 1);
  b.createBranch(cf_blocks_[instr.dword_index + 1].block);
}

void SpirvShaderTranslator::ProcessLoopEndInstruction(
    const ParsedLoopEndInstruction& instr) {
  auto& b = *builder_;

  auto head = cf_blocks_[instr.dword_index].block;
  b.setBuildPoint(head);

  EmitUnimplementedTranslationError();

  assert_true(cf_blocks_.size() > instr.dword_index + 1);
  b.createBranch(cf_blocks_[instr.dword_index + 1].block);
}

void SpirvShaderTranslator::ProcessCallInstruction(
    const ParsedCallInstruction& instr) {
  auto& b = *builder_;

  auto head = cf_blocks_[instr.dword_index].block;
  b.setBuildPoint(head);

  // Unused instruction(?)
  assert_always();
  EmitUnimplementedTranslationError();

  assert_true(cf_blocks_.size() > instr.dword_index + 1);
  b.createBranch(cf_blocks_[instr.dword_index + 1].block);
}

void SpirvShaderTranslator::ProcessReturnInstruction(
    const ParsedReturnInstruction& instr) {
  auto& b = *builder_;

  auto head = cf_blocks_[instr.dword_index].block;
  b.setBuildPoint(head);

  // Unused instruction(?)
  assert_always();
  EmitUnimplementedTranslationError();

  assert_true(cf_blocks_.size() > instr.dword_index + 1);
  b.createBranch(cf_blocks_[instr.dword_index + 1].block);
}

// CF jump
void SpirvShaderTranslator::ProcessJumpInstruction(
    const ParsedJumpInstruction& instr) {
  auto& b = *builder_;

  auto head = cf_blocks_[instr.dword_index].block;
  b.setBuildPoint(head);
  switch (instr.type) {
    case ParsedJumpInstruction::Type::kUnconditional: {
      b.createBranch(cf_blocks_[instr.target_address].block);
    } break;
    case ParsedJumpInstruction::Type::kConditional: {
      assert_true(cf_blocks_.size() > instr.dword_index + 1);

      // Based off of bool_consts
      std::vector<Id> offsets;
      offsets.push_back(b.makeUintConstant(2));  // bool_consts
      offsets.push_back(b.makeUintConstant(instr.bool_constant_index / 32));
      auto v = b.createAccessChain(spv::StorageClass::StorageClassUniform,
                                   consts_, offsets);
      v = b.createLoad(v);

      // FIXME: NVidia's compiler seems to be broken on this instruction?
      /*
      // Bitfield extract the bool constant.
      v = b.createTriOp(spv::Op::OpBitFieldUExtract, uint_type_, v,
                        b.makeUintConstant(instr.bool_constant_index % 32),
                        b.makeUintConstant(1));

      // Conditional branch
      auto cond = b.createBinOp(spv::Op::OpIEqual, bool_type_, v,
                                b.makeUintConstant(instr.condition ? 1 : 0));
      */
      v = b.createBinOp(
          spv::Op::OpBitwiseAnd, uint_type_, v,
          b.makeUintConstant(1 << (instr.bool_constant_index % 32)));
      auto cond = b.createBinOp(
          instr.condition ? spv::Op::OpINotEqual : spv::Op::OpIEqual,
          bool_type_, v, b.makeUintConstant(0));

      b.createConditionalBranch(cond, cf_blocks_[instr.target_address].block,
                                cf_blocks_[instr.dword_index + 1].block);
    } break;
    case ParsedJumpInstruction::Type::kPredicated: {
      assert_true(cf_blocks_.size() > instr.dword_index + 1);

      auto cond =
          b.createBinOp(spv::Op::OpLogicalEqual, bool_type_, b.createLoad(p0_),
                        b.makeBoolConstant(instr.condition));
      b.createConditionalBranch(cond, cf_blocks_[instr.target_address].block,
                                cf_blocks_[instr.dword_index + 1].block);
    } break;
  }
}

void SpirvShaderTranslator::ProcessAllocInstruction(
    const ParsedAllocInstruction& instr) {
  auto& b = *builder_;

  auto head = cf_blocks_[instr.dword_index].block;
  b.setBuildPoint(head);

  switch (instr.type) {
    case AllocType::kNone: {
      // ?
    } break;
    case AllocType::kVsPosition: {
      assert_true(is_vertex_shader());
    } break;
    // Also PS Colors
    case AllocType::kVsInterpolators: {
    } break;
    default:
      break;
  }

  assert_true(cf_blocks_.size() > instr.dword_index + 1);
  b.createBranch(cf_blocks_[instr.dword_index + 1].block);
}

void SpirvShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  auto& b = *builder_;
  assert_true(is_vertex_shader());
  assert_not_zero(vertex_id_);

  // Close the open predicated block if this instr isn't predicated or the
  // conditions do not match.
  if (open_predicated_block_ &&
      (!instr.is_predicated ||
       instr.predicate_condition != predicated_block_cond_)) {
    b.createBranch(predicated_block_end_);
    b.setBuildPoint(predicated_block_end_);
    open_predicated_block_ = false;
    predicated_block_cond_ = false;
    predicated_block_end_ = nullptr;
  }

  if (!open_predicated_block_ && instr.is_predicated) {
    Id pred_cond =
        b.createBinOp(spv::Op::OpLogicalEqual, bool_type_, b.createLoad(p0_),
                      b.makeBoolConstant(instr.predicate_condition));
    auto block = &b.makeNewBlock();
    open_predicated_block_ = true;
    predicated_block_cond_ = instr.predicate_condition;
    predicated_block_end_ = &b.makeNewBlock();

    b.createSelectionMerge(predicated_block_end_,
                           spv::SelectionControlMaskNone);
    b.createConditionalBranch(pred_cond, block, predicated_block_end_);
    b.setBuildPoint(block);
  }

  // Operand 0 is the index
  // Operand 1 is the binding
  // TODO: Indexed fetch
  auto vertex_id = LoadFromOperand(instr.operands[0]);
  vertex_id = b.createCompositeExtract(vertex_id, float_type_, 0);
  vertex_id = b.createUnaryOp(spv::Op::OpConvertFToS, int_type_, vertex_id);
  auto shader_vertex_id = b.createLoad(vertex_id_);
  auto cond =
      b.createBinOp(spv::Op::OpIEqual, bool_type_, vertex_id, shader_vertex_id);
  cond = b.smearScalar(spv::NoPrecision, cond, vec4_bool_type_);

  // Skip loading if it's an indexed fetch.
  auto vertex_ptr = vertex_binding_map_[instr.operands[1].storage_index]
                                       [instr.attributes.offset];
  assert_not_zero(vertex_ptr);
  auto vertex = b.createLoad(vertex_ptr);

  switch (instr.attributes.data_format) {
    case VertexFormat::k_8_8_8_8:
    case VertexFormat::k_16_16:
    case VertexFormat::k_16_16_16_16:
    case VertexFormat::k_16_16_16_16_FLOAT:
    case VertexFormat::k_32:
    case VertexFormat::k_32_32:
    case VertexFormat::k_32_32_32_32:
    case VertexFormat::k_32_FLOAT:
    case VertexFormat::k_32_32_FLOAT:
    case VertexFormat::k_32_32_32_FLOAT:
    case VertexFormat::k_32_32_32_32_FLOAT:
      // These are handled, for now.
      break;

    case VertexFormat::k_10_11_11: {
      // No conversion needed. Natively supported.
    } break;

    case VertexFormat::k_11_11_10: {
      // This needs to be converted.
    } break;
  }

  auto vertex_components = b.getNumComponents(vertex);
  Id alt_vertex = 0;
  switch (vertex_components) {
    case 1:
      alt_vertex = b.makeFloatConstant(0.f);
      break;
    case 2:
      alt_vertex = b.makeCompositeConstant(
          vec2_float_type_, std::vector<Id>({b.makeFloatConstant(0.f),
                                             b.makeFloatConstant(1.f)}));
      break;
    case 3:
      alt_vertex = b.makeCompositeConstant(
          vec3_float_type_,
          std::vector<Id>({b.makeFloatConstant(0.f), b.makeFloatConstant(0.f),
                           b.makeFloatConstant(1.f)}));
      break;
    case 4:
      alt_vertex = b.makeCompositeConstant(
          vec4_float_type_,
          std::vector<Id>({b.makeFloatConstant(0.f), b.makeFloatConstant(0.f),
                           b.makeFloatConstant(0.f),
                           b.makeFloatConstant(1.f)}));
      break;
    default:
      assert_unhandled_case(vertex_components);
  }

  vertex = b.createTriOp(spv::Op::OpSelect, b.getTypeId(vertex), cond, vertex,
                         alt_vertex);
  StoreToResult(vertex, instr.result);
}

void SpirvShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  auto& b = *builder_;

  // Close the open predicated block if this instr isn't predicated or the
  // conditions do not match.
  if (open_predicated_block_ &&
      (!instr.is_predicated ||
       instr.predicate_condition != predicated_block_cond_)) {
    b.createBranch(predicated_block_end_);
    b.setBuildPoint(predicated_block_end_);
    open_predicated_block_ = false;
    predicated_block_cond_ = false;
    predicated_block_end_ = nullptr;
  }

  if (!open_predicated_block_ && instr.is_predicated) {
    Id pred_cond =
        b.createBinOp(spv::Op::OpLogicalEqual, bool_type_, b.createLoad(p0_),
                      b.makeBoolConstant(instr.predicate_condition));
    auto block = &b.makeNewBlock();
    open_predicated_block_ = true;
    predicated_block_cond_ = instr.predicate_condition;
    predicated_block_end_ = &b.makeNewBlock();

    b.createSelectionMerge(predicated_block_end_,
                           spv::SelectionControlMaskNone);
    b.createConditionalBranch(pred_cond, block, predicated_block_end_);
    b.setBuildPoint(block);
  }

  // Operand 0 is the offset
  // Operand 1 is the sampler index
  Id dest = 0;
  Id src = LoadFromOperand(instr.operands[0]);
  assert_not_zero(src);

  uint32_t dim_idx = 0;
  switch (instr.dimension) {
    case TextureDimension::k1D: {
      dim_idx = 0;
    } break;
    case TextureDimension::k2D: {
      dim_idx = 1;
    } break;
    case TextureDimension::k3D: {
      dim_idx = 2;
    } break;
    case TextureDimension::kCube: {
      dim_idx = 3;
    } break;
    default:
      assert_unhandled_case(instr.dimension);
  }

  switch (instr.opcode) {
    case FetchOpcode::kTextureFetch: {
      auto texture_index = b.makeUintConstant(instr.operands[1].storage_index);
      auto texture_ptr =
          b.createAccessChain(spv::StorageClass::StorageClassUniformConstant,
                              tex_[dim_idx], std::vector<Id>({texture_index}));
      auto texture = b.createLoad(texture_ptr);

      spv::Builder::TextureParameters params = {0};
      params.coords = src;
      params.sampler = texture;
      dest = b.createTextureCall(spv::NoPrecision, vec4_float_type_, false,
                                 false, false, false, false, params);
    } break;
    default:
      // TODO: the rest of these
      assert_always();
      break;
  }

  if (dest) {
    b.createStore(dest, pv_);
    StoreToResult(dest, instr.result);
  }
}

void SpirvShaderTranslator::ProcessAluInstruction(
    const ParsedAluInstruction& instr) {
  auto& b = *builder_;
  switch (instr.type) {
    case ParsedAluInstruction::Type::kNop:
      b.createNoResultOp(spv::Op::OpNop);
      break;
    case ParsedAluInstruction::Type::kVector:
      ProcessVectorAluInstruction(instr);
      break;
    case ParsedAluInstruction::Type::kScalar:
      ProcessScalarAluInstruction(instr);
      break;
  }
}

void SpirvShaderTranslator::ProcessVectorAluInstruction(
    const ParsedAluInstruction& instr) {
  auto& b = *builder_;

  // TODO: If we have identical operands, reuse previous one.
  Id sources[3] = {0};
  Id dest = 0;
  for (size_t i = 0; i < instr.operand_count; i++) {
    sources[i] = LoadFromOperand(instr.operands[i]);
  }

  // Close the open predicated block if this instr isn't predicated or the
  // conditions do not match.
  if (open_predicated_block_ &&
      (!instr.is_predicated ||
       instr.predicate_condition != predicated_block_cond_)) {
    b.createBranch(predicated_block_end_);
    b.setBuildPoint(predicated_block_end_);
    open_predicated_block_ = false;
    predicated_block_cond_ = false;
    predicated_block_end_ = nullptr;
  }

  if (!open_predicated_block_ && instr.is_predicated) {
    Id pred_cond =
        b.createBinOp(spv::Op::OpLogicalEqual, bool_type_, b.createLoad(p0_),
                      b.makeBoolConstant(instr.predicate_condition));
    auto block = &b.makeNewBlock();
    open_predicated_block_ = true;
    predicated_block_cond_ = instr.predicate_condition;
    predicated_block_end_ = &b.makeNewBlock();

    b.createSelectionMerge(predicated_block_end_,
                           spv::SelectionControlMaskNone);
    b.createConditionalBranch(pred_cond, block, predicated_block_end_);
    b.setBuildPoint(block);
  }

  bool close_predicated_block = false;
  switch (instr.vector_opcode) {
    case AluVectorOpcode::kAdd: {
      dest = b.createBinOp(spv::Op::OpFAdd, vec4_float_type_, sources[0],
                           sources[1]);
    } break;

    case AluVectorOpcode::kCndEq: {
      // dest = src0 == 0.0 ? src1 : src2;
      auto c = b.createBinOp(spv::Op::OpFOrdEqual, vec4_bool_type_, sources[0],
                             vec4_float_zero_);
      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c, sources[1],
                           sources[2]);
    } break;

    case AluVectorOpcode::kCndGe: {
      // dest = src0 >= 0.0 ? src1 : src2;
      auto c = b.createBinOp(spv::Op::OpFOrdGreaterThanEqual, vec4_bool_type_,
                             sources[0], vec4_float_zero_);
      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c, sources[1],
                           sources[2]);
    } break;

    case AluVectorOpcode::kCndGt: {
      // dest = src0 > 0.0 ? src1 : src2;
      auto c = b.createBinOp(spv::Op::OpFOrdGreaterThan, vec4_bool_type_,
                             sources[0], vec4_float_zero_);
      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c, sources[1],
                           sources[2]);
    } break;

    case AluVectorOpcode::kCube: {
      // TODO:
    } break;

    case AluVectorOpcode::kDst: {
      auto src0_y = b.createCompositeExtract(sources[0], float_type_, 1);
      auto src1_y = b.createCompositeExtract(sources[1], float_type_, 1);
      auto dst_y = b.createBinOp(spv::Op::OpFMul, float_type_, src0_y, src1_y);

      auto src0_z = b.createCompositeExtract(sources[0], float_type_, 3);
      auto src1_w = b.createCompositeExtract(sources[1], float_type_, 4);
      dest = b.createCompositeConstruct(
          vec4_float_type_,
          std::vector<Id>({b.makeFloatConstant(1.f), dst_y, src0_z, src1_w}));
    } break;

    case AluVectorOpcode::kDp2Add: {
      auto src0_xy = b.createOp(spv::Op::OpVectorShuffle, vec2_float_type_,
                                {sources[0], sources[0], 0, 1});
      auto src1_xy = b.createOp(spv::Op::OpVectorShuffle, vec2_float_type_,
                                {sources[1], sources[1], 0, 1});
      auto src2_x = b.createCompositeExtract(sources[2], float_type_, 0);
      dest = b.createBinOp(spv::Op::OpDot, float_type_, src0_xy, src1_xy);
      dest = b.createBinOp(spv::Op::OpFAdd, float_type_, dest, src2_x);
      dest = b.smearScalar(spv::NoPrecision, dest, vec4_float_type_);
    } break;

    case AluVectorOpcode::kDp3: {
      auto src0_xyz = b.createOp(spv::Op::OpVectorShuffle, vec3_float_type_,
                                 {sources[0], sources[0], 0, 1, 2});
      auto src1_xyz = b.createOp(spv::Op::OpVectorShuffle, vec3_float_type_,
                                 {sources[1], sources[1], 0, 1, 2});
      dest = b.createBinOp(spv::Op::OpDot, float_type_, src0_xyz, src1_xyz);
      dest = b.smearScalar(spv::NoPrecision, dest, vec4_float_type_);
    } break;

    case AluVectorOpcode::kDp4: {
      dest = b.createBinOp(spv::Op::OpDot, float_type_, sources[0], sources[1]);
      dest = b.smearScalar(spv::NoPrecision, dest, vec4_float_type_);
    } break;

    case AluVectorOpcode::kFloor: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, vec4_float_type_,
                                             spv::GLSLstd450::kFloor,
                                             {sources[0]});
    } break;

    case AluVectorOpcode::kFrc: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, vec4_float_type_,
                                             spv::GLSLstd450::kFract,
                                             {sources[0]});
    } break;

    case AluVectorOpcode::kKillEq: {
      auto continue_block = &b.makeNewBlock();
      auto kill_block = &b.makeNewBlock();
      auto cond = b.createBinOp(spv::Op::OpFOrdEqual, vec4_bool_type_,
                                sources[0], sources[1]);
      cond = b.createUnaryOp(spv::Op::OpAny, bool_type_, cond);
      b.createConditionalBranch(cond, kill_block, continue_block);

      b.setBuildPoint(kill_block);
      b.createNoResultOp(spv::Op::OpKill);

      b.setBuildPoint(continue_block);
      dest = vec4_float_zero_;
    } break;

    case AluVectorOpcode::kKillGe: {
      auto continue_block = &b.makeNewBlock();
      auto kill_block = &b.makeNewBlock();
      auto cond = b.createBinOp(spv::Op::OpFOrdGreaterThanEqual,
                                vec4_bool_type_, sources[0], sources[1]);
      cond = b.createUnaryOp(spv::Op::OpAny, bool_type_, cond);
      b.createConditionalBranch(cond, kill_block, continue_block);

      b.setBuildPoint(kill_block);
      b.createNoResultOp(spv::Op::OpKill);

      b.setBuildPoint(continue_block);
      dest = vec4_float_zero_;
    } break;

    case AluVectorOpcode::kKillGt: {
      auto continue_block = &b.makeNewBlock();
      auto kill_block = &b.makeNewBlock();
      auto cond = b.createBinOp(spv::Op::OpFOrdGreaterThan, vec4_bool_type_,
                                sources[0], sources[1]);
      cond = b.createUnaryOp(spv::Op::OpAny, bool_type_, cond);
      b.createConditionalBranch(cond, kill_block, continue_block);

      b.setBuildPoint(kill_block);
      b.createNoResultOp(spv::Op::OpKill);

      b.setBuildPoint(continue_block);
      dest = vec4_float_zero_;
    } break;

    case AluVectorOpcode::kKillNe: {
      auto continue_block = &b.makeNewBlock();
      auto kill_block = &b.makeNewBlock();
      auto cond = b.createBinOp(spv::Op::OpFOrdNotEqual, vec4_bool_type_,
                                sources[0], sources[1]);
      cond = b.createUnaryOp(spv::Op::OpAny, bool_type_, cond);
      b.createConditionalBranch(cond, kill_block, continue_block);

      b.setBuildPoint(kill_block);
      b.createNoResultOp(spv::Op::OpKill);

      b.setBuildPoint(continue_block);
      dest = vec4_float_zero_;
    } break;

    case AluVectorOpcode::kMad: {
      dest = b.createBinOp(spv::Op::OpFMul, vec4_float_type_, sources[0],
                           sources[1]);
      dest = b.createBinOp(spv::Op::OpFAdd, vec4_float_type_, dest, sources[2]);
    } break;

    case AluVectorOpcode::kMax4: {
      auto src0_x = b.createCompositeExtract(sources[0], float_type_, 0);
      auto src0_y = b.createCompositeExtract(sources[0], float_type_, 1);
      auto src0_z = b.createCompositeExtract(sources[0], float_type_, 2);
      auto src0_w = b.createCompositeExtract(sources[0], float_type_, 3);

      auto max_xy = CreateGlslStd450InstructionCall(
          spv::NoPrecision, float_type_, spv::GLSLstd450::kFMax,
          {src0_x, src0_y});
      auto max_zw = CreateGlslStd450InstructionCall(
          spv::NoPrecision, float_type_, spv::GLSLstd450::kFMax,
          {src0_z, src0_w});
      auto max_xyzw = CreateGlslStd450InstructionCall(
          spv::NoPrecision, float_type_, spv::GLSLstd450::kFMax,
          {max_xy, max_zw});

      // FIXME: Docs say this only updates pv.x?
      dest = b.smearScalar(spv::NoPrecision, max_xyzw, vec4_float_type_);
    } break;

    case AluVectorOpcode::kMaxA: {
      // a0 = clamp(floor(src0.w + 0.5), -256, 255)
      auto addr = b.createCompositeExtract(sources[0], float_type_, 3);
      addr = b.createBinOp(spv::Op::OpFAdd, float_type_, addr,
                           b.makeFloatConstant(0.5f));
      addr = b.createUnaryOp(spv::Op::OpConvertFToS, int_type_, addr);
      addr = CreateGlslStd450InstructionCall(
          spv::NoPrecision, int_type_, spv::GLSLstd450::kSClamp,
          {addr, b.makeIntConstant(-256), b.makeIntConstant(255)});
      b.createStore(addr, a0_);

      // dest = src0 >= src1 ? src0 : src1
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, vec4_float_type_,
                                             spv::GLSLstd450::kFMax,
                                             {sources[0], sources[1]});
    } break;

    case AluVectorOpcode::kMax: {
      if (sources[0] == sources[1]) {
        // mov dst, src
        dest = sources[0];
        break;
      }

      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, vec4_float_type_,
                                             spv::GLSLstd450::kFMax,
                                             {sources[0], sources[1]});
    } break;

    case AluVectorOpcode::kMin: {
      if (sources[0] == sources[1]) {
        // mov dst, src
        dest = sources[0];
        break;
      }

      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, vec4_float_type_,
                                             spv::GLSLstd450::kFMin,
                                             {sources[0], sources[1]});
    } break;

    case AluVectorOpcode::kMul: {
      dest = b.createBinOp(spv::Op::OpFMul, vec4_float_type_, sources[0],
                           sources[1]);
    } break;

    case AluVectorOpcode::kSetpEqPush: {
      auto c0 = b.createBinOp(spv::Op::OpFOrdEqual, vec4_bool_type_, sources[0],
                              vec4_float_zero_);
      auto c1 = b.createBinOp(spv::Op::OpFOrdEqual, vec4_bool_type_, sources[1],
                              vec4_float_zero_);
      auto c_and =
          b.createBinOp(spv::Op::OpLogicalAnd, vec4_bool_type_, c0, c1);
      auto c_and_x = b.createCompositeExtract(c_and, bool_type_, 0);
      c_and_x = b.smearScalar(spv::NoPrecision, c_and_x, vec4_bool_type_);
      auto c_and_w = b.createCompositeExtract(c_and, bool_type_, 3);

      // p0
      b.createStore(c_and_w, p0_);
      close_predicated_block = true;

      // dest
      auto s0_x = b.createCompositeExtract(sources[0], float_type_, 0);
      s0_x = b.createBinOp(spv::Op::OpFAdd, float_type_, s0_x,
                           b.makeFloatConstant(1.f));
      auto s0 = b.smearScalar(spv::NoPrecision, s0_x, vec4_float_type_);

      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c_and_x,
                           vec4_float_zero_, s0);
    } break;

    case AluVectorOpcode::kSetpGePush: {
      auto c0 = b.createBinOp(spv::Op::OpFOrdEqual, vec4_bool_type_, sources[0],
                              vec4_float_zero_);
      auto c1 = b.createBinOp(spv::Op::OpFOrdGreaterThanEqual, vec4_bool_type_,
                              sources[1], vec4_float_zero_);
      auto c_and =
          b.createBinOp(spv::Op::OpLogicalAnd, vec4_bool_type_, c0, c1);
      auto c_and_x = b.createCompositeExtract(c_and, bool_type_, 0);
      c_and_x = b.smearScalar(spv::NoPrecision, c_and_x, vec4_bool_type_);
      auto c_and_w = b.createCompositeExtract(c_and, bool_type_, 3);

      // p0
      b.createStore(c_and_w, p0_);
      close_predicated_block = true;

      // dest
      auto s0_x = b.createCompositeExtract(sources[0], float_type_, 0);
      s0_x = b.createBinOp(spv::Op::OpFAdd, float_type_, s0_x,
                           b.makeFloatConstant(1.f));
      auto s0 = b.smearScalar(spv::NoPrecision, s0_x, vec4_float_type_);

      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c_and_x,
                           vec4_float_zero_, s0);
    } break;

    case AluVectorOpcode::kSetpGtPush: {
      auto c0 = b.createBinOp(spv::Op::OpFOrdEqual, vec4_bool_type_, sources[0],
                              vec4_float_zero_);
      auto c1 = b.createBinOp(spv::Op::OpFOrdGreaterThan, vec4_bool_type_,
                              sources[1], vec4_float_zero_);
      auto c_and =
          b.createBinOp(spv::Op::OpLogicalAnd, vec4_bool_type_, c0, c1);
      auto c_and_x = b.createCompositeExtract(c_and, bool_type_, 0);
      c_and_x = b.smearScalar(spv::NoPrecision, c_and_x, vec4_bool_type_);
      auto c_and_w = b.createCompositeExtract(c_and, bool_type_, 3);

      // p0
      b.createStore(c_and_w, p0_);
      close_predicated_block = true;

      // dest
      auto s0_x = b.createCompositeExtract(sources[0], float_type_, 0);
      s0_x = b.createBinOp(spv::Op::OpFAdd, float_type_, s0_x,
                           b.makeFloatConstant(1.f));
      auto s0 = b.smearScalar(spv::NoPrecision, s0_x, vec4_float_type_);

      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c_and_x,
                           vec4_float_zero_, s0);
    } break;

    case AluVectorOpcode::kSetpNePush: {
      auto c0 = b.createBinOp(spv::Op::OpFOrdNotEqual, vec4_bool_type_,
                              sources[0], vec4_float_zero_);
      auto c1 = b.createBinOp(spv::Op::OpFOrdEqual, vec4_bool_type_, sources[1],
                              vec4_float_zero_);
      auto c_and =
          b.createBinOp(spv::Op::OpLogicalAnd, vec4_bool_type_, c0, c1);
      auto c_and_x = b.createCompositeExtract(c_and, bool_type_, 0);
      c_and_x = b.smearScalar(spv::NoPrecision, c_and_x, vec4_bool_type_);
      auto c_and_w = b.createCompositeExtract(c_and, bool_type_, 3);

      // p0
      b.createStore(c_and_w, p0_);
      close_predicated_block = true;

      // dest
      auto s0_x = b.createCompositeExtract(sources[0], float_type_, 0);
      s0_x = b.createBinOp(spv::Op::OpFAdd, float_type_, s0_x,
                           b.makeFloatConstant(1.f));
      auto s0 = b.smearScalar(spv::NoPrecision, s0_x, vec4_float_type_);

      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c_and_x,
                           vec4_float_zero_, s0);
    } break;

    case AluVectorOpcode::kSeq: {
      // foreach(el) src0 == src1 ? 1.0 : 0.0
      auto c = b.createBinOp(spv::Op::OpFOrdEqual, vec4_bool_type_, sources[0],
                             sources[1]);
      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c,
                           vec4_float_one_, vec4_float_zero_);
    } break;

    case AluVectorOpcode::kSge: {
      // foreach(el) src0 >= src1 ? 1.0 : 0.0
      auto c = b.createBinOp(spv::Op::OpFOrdGreaterThanEqual, vec4_bool_type_,
                             sources[0], sources[1]);
      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c,
                           vec4_float_one_, vec4_float_zero_);
    } break;

    case AluVectorOpcode::kSgt: {
      // foreach(el) src0 > src1 ? 1.0 : 0.0
      auto c = b.createBinOp(spv::Op::OpFOrdGreaterThan, vec4_bool_type_,
                             sources[0], sources[1]);
      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c,
                           vec4_float_one_, vec4_float_zero_);
    } break;

    case AluVectorOpcode::kSne: {
      // foreach(el) src0 != src1 ? 1.0 : 0.0
      auto c = b.createBinOp(spv::Op::OpFOrdNotEqual, vec4_bool_type_,
                             sources[0], sources[1]);
      dest = b.createTriOp(spv::Op::OpSelect, vec4_float_type_, c,
                           vec4_float_one_, vec4_float_zero_);
    } break;

    case AluVectorOpcode::kTrunc: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, vec4_float_type_,
                                             GLSLstd450::kTrunc, {sources[0]});
    } break;

    default:
      assert_unhandled_case(instr.vector_opcode);
      break;
  }

  assert_not_zero(dest);
  if (dest) {
    b.createStore(dest, pv_);
    StoreToResult(dest, instr.result);
  }

  if (close_predicated_block && open_predicated_block_) {
    b.createBranch(predicated_block_end_);
    b.setBuildPoint(predicated_block_end_);
    open_predicated_block_ = false;
    predicated_block_cond_ = false;
    predicated_block_end_ = nullptr;
  }
}

void SpirvShaderTranslator::ProcessScalarAluInstruction(
    const ParsedAluInstruction& instr) {
  auto& b = *builder_;

  // TODO: If we have identical operands, reuse previous one.
  Id sources[3] = {0};
  Id dest = 0;
  for (size_t i = 0, x = 0; i < instr.operand_count; i++) {
    auto src = LoadFromOperand(instr.operands[i]);

    // Pull components out of the vector operands and use them as sources.
    for (size_t j = 0; j < instr.operands[i].component_count; j++) {
      uint32_t component = 0;
      switch (instr.operands[i].components[j]) {
        case SwizzleSource::kX:
          component = 0;
          break;
        case SwizzleSource::kY:
          component = 1;
          break;
        case SwizzleSource::kZ:
          component = 2;
          break;
        case SwizzleSource::kW:
          component = 3;
          break;
        case SwizzleSource::k0:
        case SwizzleSource::k1:
          // Don't believe this can happen.
          assert_always();
          break;
        default:
          assert_always();
          break;
      }

      sources[x++] = b.createCompositeExtract(src, float_type_, component);
    }
  }

  // Close the open predicated block if this instr isn't predicated or the
  // conditions do not match.
  if (open_predicated_block_ &&
      (!instr.is_predicated ||
       instr.predicate_condition != predicated_block_cond_)) {
    b.createBranch(predicated_block_end_);
    b.setBuildPoint(predicated_block_end_);
    open_predicated_block_ = false;
    predicated_block_cond_ = false;
    predicated_block_end_ = nullptr;
  }

  if (!open_predicated_block_ && instr.is_predicated) {
    Id pred_cond =
        b.createBinOp(spv::Op::OpLogicalEqual, bool_type_, b.createLoad(p0_),
                      b.makeBoolConstant(instr.predicate_condition));
    auto block = &b.makeNewBlock();
    open_predicated_block_ = true;
    predicated_block_cond_ = instr.predicate_condition;
    predicated_block_end_ = &b.makeNewBlock();

    b.createSelectionMerge(predicated_block_end_,
                           spv::SelectionControlMaskNone);
    b.createConditionalBranch(pred_cond, block, predicated_block_end_);
    b.setBuildPoint(block);
  }

  bool close_predicated_block = false;
  switch (instr.scalar_opcode) {
    case AluScalarOpcode::kAdds:
    case AluScalarOpcode::kAddsc0:
    case AluScalarOpcode::kAddsc1: {
      // dest = src0 + src1
      dest =
          b.createBinOp(spv::Op::OpFAdd, float_type_, sources[0], sources[1]);
    } break;

    case AluScalarOpcode::kAddsPrev: {
      // dest = src0 + ps
      dest = b.createBinOp(spv::Op::OpFAdd, float_type_, sources[0],
                           b.createLoad(ps_));
    } break;

    case AluScalarOpcode::kCos: {
      // dest = cos(src0)
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             GLSLstd450::kCos, {sources[0]});
    } break;

    case AluScalarOpcode::kExp: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             GLSLstd450::kExp2, {sources[0]});
    } break;

    case AluScalarOpcode::kFloors: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             GLSLstd450::kFloor, {sources[0]});
    } break;

    case AluScalarOpcode::kFrcs: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             GLSLstd450::kFract, {sources[0]});
    } break;

    case AluScalarOpcode::kKillsEq: {
      auto continue_block = &b.makeNewBlock();
      auto kill_block = &b.makeNewBlock();
      auto cond = b.createBinOp(spv::Op::OpFOrdEqual, bool_type_, sources[0],
                                b.makeFloatConstant(0.f));
      b.createConditionalBranch(cond, kill_block, continue_block);

      b.setBuildPoint(kill_block);
      b.createNoResultOp(spv::Op::OpKill);

      b.setBuildPoint(continue_block);
      dest = b.makeFloatConstant(0.f);
    } break;

    case AluScalarOpcode::kKillsGe: {
      auto continue_block = &b.makeNewBlock();
      auto kill_block = &b.makeNewBlock();
      auto cond = b.createBinOp(spv::Op::OpFOrdGreaterThanEqual, bool_type_,
                                sources[0], b.makeFloatConstant(0.f));
      b.createConditionalBranch(cond, kill_block, continue_block);

      b.setBuildPoint(kill_block);
      b.createNoResultOp(spv::Op::OpKill);

      b.setBuildPoint(continue_block);
      dest = b.makeFloatConstant(0.f);
    } break;

    case AluScalarOpcode::kKillsGt: {
      auto continue_block = &b.makeNewBlock();
      auto kill_block = &b.makeNewBlock();
      auto cond = b.createBinOp(spv::Op::OpFOrdGreaterThan, bool_type_,
                                sources[0], b.makeFloatConstant(0.f));
      b.createConditionalBranch(cond, kill_block, continue_block);

      b.setBuildPoint(kill_block);
      b.createNoResultOp(spv::Op::OpKill);

      b.setBuildPoint(continue_block);
      dest = b.makeFloatConstant(0.f);
    } break;

    case AluScalarOpcode::kKillsNe: {
      auto continue_block = &b.makeNewBlock();
      auto kill_block = &b.makeNewBlock();
      auto cond = b.createBinOp(spv::Op::OpFOrdNotEqual, bool_type_, sources[0],
                                b.makeFloatConstant(0.f));
      b.createConditionalBranch(cond, kill_block, continue_block);

      b.setBuildPoint(kill_block);
      b.createNoResultOp(spv::Op::OpKill);

      b.setBuildPoint(continue_block);
      dest = b.makeFloatConstant(0.f);
    } break;

    case AluScalarOpcode::kKillsOne: {
      auto continue_block = &b.makeNewBlock();
      auto kill_block = &b.makeNewBlock();
      auto cond = b.createBinOp(spv::Op::OpFOrdEqual, bool_type_, sources[0],
                                b.makeFloatConstant(1.f));
      b.createConditionalBranch(cond, kill_block, continue_block);

      b.setBuildPoint(kill_block);
      b.createNoResultOp(spv::Op::OpKill);

      b.setBuildPoint(continue_block);
      dest = b.makeFloatConstant(0.f);
    } break;

    case AluScalarOpcode::kLogc: {
      auto t = CreateGlslStd450InstructionCall(
          spv::NoPrecision, float_type_, spv::GLSLstd450::kLog2, {sources[0]});

      // FIXME: We don't check to see if t == -INF, we just check for INF
      auto c = b.createUnaryOp(spv::Op::OpIsInf, bool_type_, t);
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, c,
                           b.makeFloatConstant(-FLT_MAX), t);
    } break;

    case AluScalarOpcode::kLog: {
      dest = CreateGlslStd450InstructionCall(
          spv::NoPrecision, float_type_, spv::GLSLstd450::kLog2, {sources[0]});
    } break;

    case AluScalarOpcode::kMaxAsf: {
      auto addr =
          b.createUnaryOp(spv::Op::OpConvertFToS, int_type_, sources[0]);
      addr = CreateGlslStd450InstructionCall(
          spv::NoPrecision, int_type_, spv::GLSLstd450::kSClamp,
          {addr, b.makeIntConstant(-256), b.makeIntConstant(255)});
      b.createStore(addr, a0_);

      // dest = src0 >= src1 ? src0 : src1
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             spv::GLSLstd450::kFMax,
                                             {sources[0], sources[1]});
    } break;

    case AluScalarOpcode::kMaxAs: {
      // a0 = clamp(floor(src0 + 0.5), -256, 255)
      auto addr = b.createBinOp(spv::Op::OpFAdd, float_type_, sources[0],
                                b.makeFloatConstant(0.5f));
      addr = b.createUnaryOp(spv::Op::OpConvertFToS, int_type_, addr);
      addr = CreateGlslStd450InstructionCall(
          spv::NoPrecision, int_type_, spv::GLSLstd450::kSClamp,
          {addr, b.makeIntConstant(-256), b.makeIntConstant(255)});
      b.createStore(addr, a0_);

      // dest = src0 >= src1 ? src0 : src1
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             spv::GLSLstd450::kFMax,
                                             {sources[0], sources[1]});
    } break;

    case AluScalarOpcode::kMaxs: {
      // dest = max(src0, src1)
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             GLSLstd450::kFMax,
                                             {sources[0], sources[1]});
    } break;

    case AluScalarOpcode::kMins: {
      // dest = min(src0, src1)
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             GLSLstd450::kFMin,
                                             {sources[0], sources[1]});
    } break;

    case AluScalarOpcode::kMuls:
    case AluScalarOpcode::kMulsc0:
    case AluScalarOpcode::kMulsc1: {
      // dest = src0 * src1
      dest =
          b.createBinOp(spv::Op::OpFMul, float_type_, sources[0], sources[1]);
    } break;

    case AluScalarOpcode::kMulsPrev: {
      // dest = src0 * ps
      dest = b.createBinOp(spv::Op::OpFMul, float_type_, sources[0],
                           b.createLoad(ps_));
    } break;

    case AluScalarOpcode::kMulsPrev2: {
      // TODO: Uh... see GLSL translator for impl.
    } break;

    case AluScalarOpcode::kRcpc: {
      dest = b.createBinOp(spv::Op::OpFDiv, float_type_,
                           b.makeFloatConstant(1.f), sources[0]);
      dest = CreateGlslStd450InstructionCall(
          spv::NoPrecision, float_type_, spv::GLSLstd450::kFClamp,
          {dest, b.makeFloatConstant(-FLT_MAX), b.makeFloatConstant(FLT_MAX)});
    } break;

    case AluScalarOpcode::kRcpf: {
      dest = b.createBinOp(spv::Op::OpFDiv, float_type_,
                           b.makeFloatConstant(1.f), sources[0]);
      auto c = b.createUnaryOp(spv::Op::OpIsInf, bool_type_, dest);
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, c,
                           b.makeFloatConstant(0.f), dest);
    } break;

    case AluScalarOpcode::kRcp: {
      // dest = src0 != 0.0 ? 1.0 / src0 : 0.0;
      auto c = b.createBinOp(spv::Op::OpFOrdEqual, float_type_, sources[0],
                             b.makeFloatConstant(0.f));
      auto d = b.createBinOp(spv::Op::OpFDiv, float_type_,
                             b.makeFloatConstant(1.f), sources[0]);
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, c,
                           b.makeFloatConstant(0.f), d);
    } break;

    case AluScalarOpcode::kRsqc: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             spv::GLSLstd450::kInverseSqrt,
                                             {sources[0]});
      dest = CreateGlslStd450InstructionCall(
          spv::NoPrecision, float_type_, spv::GLSLstd450::kFClamp,
          {dest, b.makeFloatConstant(-FLT_MAX), b.makeFloatConstant(FLT_MAX)});
    } break;

    case AluScalarOpcode::kRsqf: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             spv::GLSLstd450::kInverseSqrt,
                                             {sources[0]});
      auto c = b.createUnaryOp(spv::Op::OpIsInf, bool_type_, dest);
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, c,
                           b.makeFloatConstant(0.f), dest);
    } break;

    case AluScalarOpcode::kRsq: {
      // dest = src0 != 0.0 ? inversesqrt(src0) : 0.0;
      auto c = b.createBinOp(spv::Op::OpFOrdEqual, bool_type_, sources[0],
                             b.makeFloatConstant(0.f));
      auto d = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                               spv::GLSLstd450::kInverseSqrt,
                                               {sources[0]});
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, c,
                           b.makeFloatConstant(0.f), d);
    } break;

    case AluScalarOpcode::kSeqs: {
      // dest = src0 == 0.0 ? 1.0 : 0.0;
      auto cond = b.createBinOp(spv::Op::OpFOrdEqual, bool_type_, sources[0],
                                b.makeFloatConstant(0.f));
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, cond,
                           b.makeFloatConstant(1.f), b.makeFloatConstant(0.f));
    } break;

    case AluScalarOpcode::kSges: {
      // dest = src0 >= 0.0 ? 1.0 : 0.0;
      auto cond = b.createBinOp(spv::Op::OpFOrdGreaterThanEqual, bool_type_,
                                sources[0], b.makeFloatConstant(0.f));
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, cond,
                           b.makeFloatConstant(1.f), b.makeFloatConstant(0.f));
    } break;

    case AluScalarOpcode::kSgts: {
      // dest = src0 > 0.0 ? 1.0 : 0.0;
      auto cond = b.createBinOp(spv::Op::OpFOrdGreaterThan, bool_type_,
                                sources[0], b.makeFloatConstant(0.f));
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, cond,
                           b.makeFloatConstant(1.f), b.makeFloatConstant(0.f));
    } break;

    case AluScalarOpcode::kSnes: {
      // dest = src0 != 0.0 ? 1.0 : 0.0;
      auto cond = b.createBinOp(spv::Op::OpFOrdNotEqual, bool_type_, sources[0],
                                b.makeFloatConstant(0.f));
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, cond,
                           b.makeFloatConstant(1.f), b.makeFloatConstant(0.f));
    } break;

    case AluScalarOpcode::kSetpClr: {
      b.createStore(b.makeBoolConstant(false), p0_);
      close_predicated_block = true;
      dest = b.makeFloatConstant(FLT_MAX);
    } break;

    case AluScalarOpcode::kSetpEq: {
      auto cond = b.createBinOp(spv::Op::OpFOrdEqual, bool_type_, sources[0],
                                b.makeFloatConstant(0.f));
      // p0 = cond
      b.createStore(cond, p0_);
      close_predicated_block = true;

      // dest = cond ? 0.f : 1.f;
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, cond,
                           b.makeFloatConstant(0.f), b.makeFloatConstant(1.f));
    } break;

    case AluScalarOpcode::kSetpGe: {
      auto cond = b.createBinOp(spv::Op::OpFOrdGreaterThanEqual, bool_type_,
                                sources[0], b.makeFloatConstant(0.f));
      // p0 = cond
      b.createStore(cond, p0_);
      close_predicated_block = true;

      // dest = cond ? 0.f : 1.f;
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, cond,
                           b.makeFloatConstant(0.f), b.makeFloatConstant(1.f));
    } break;

    case AluScalarOpcode::kSetpGt: {
      auto cond = b.createBinOp(spv::Op::OpFOrdGreaterThan, bool_type_,
                                sources[0], b.makeFloatConstant(0.f));
      // p0 = cond
      b.createStore(cond, p0_);
      close_predicated_block = true;

      // dest = cond ? 0.f : 1.f;
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, cond,
                           b.makeFloatConstant(0.f), b.makeFloatConstant(1.f));
    } break;

    case AluScalarOpcode::kSetpInv: {
      // p0 = src0 == 1.0
      auto cond = b.createBinOp(spv::Op::OpFOrdEqual, bool_type_, sources[0],
                                b.makeFloatConstant(1.f));
      b.createStore(cond, p0_);
      close_predicated_block = true;

      // if (!cond) dest = src0 == 0.0 ? 1.0 : src0;
      auto dst_cond = b.createBinOp(spv::Op::OpFOrdEqual, bool_type_,
                                    sources[0], b.makeFloatConstant(0.f));
      auto dst_false = b.createTriOp(spv::Op::OpSelect, float_type_, dst_cond,
                                     b.makeFloatConstant(1.f), sources[0]);
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, cond,
                           b.makeFloatConstant(0.f), dst_false);
    } break;

    case AluScalarOpcode::kSetpNe: {
      auto cond = b.createBinOp(spv::Op::OpFOrdNotEqual, bool_type_, sources[0],
                                b.makeFloatConstant(0.f));

      // p0 = cond
      b.createStore(cond, p0_);
      close_predicated_block = true;

      // dest = cond ? 0.f : 1.f;
      dest = b.createTriOp(spv::Op::OpSelect, float_type_, cond,
                           b.makeFloatConstant(0.f), b.makeFloatConstant(1.f));
    } break;

    case AluScalarOpcode::kSetpPop: {
      auto src = b.createBinOp(spv::Op::OpFSub, float_type_, sources[0],
                               b.makeFloatConstant(1.f));
      auto c = b.createBinOp(spv::Op::OpFOrdLessThanEqual, bool_type_, src,
                             b.makeFloatConstant(0.f));
      b.createStore(c, p0_);
      close_predicated_block = true;

      dest = CreateGlslStd450InstructionCall(
          spv::NoPrecision, float_type_, GLSLstd450::kFMax,
          {sources[0], b.makeFloatConstant(0.f)});
    } break;

    case AluScalarOpcode::kSetpRstr: {
      auto c = b.createBinOp(spv::Op::OpFOrdEqual, bool_type_, sources[0],
                             b.makeFloatConstant(0.f));
      b.createStore(c, p0_);
      close_predicated_block = true;
      dest = sources[0];
    } break;

    case AluScalarOpcode::kSin: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             GLSLstd450::kSin, {sources[0]});
    } break;

    case AluScalarOpcode::kSqrt: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             GLSLstd450::kSqrt, {sources[0]});
    } break;

    case AluScalarOpcode::kSubs:
    case AluScalarOpcode::kSubsc0:
    case AluScalarOpcode::kSubsc1: {
      dest =
          b.createBinOp(spv::Op::OpFSub, float_type_, sources[0], sources[1]);
    } break;

    case AluScalarOpcode::kSubsPrev: {
      dest = b.createBinOp(spv::Op::OpFSub, float_type_, sources[0],
                           b.createLoad(ps_));
    } break;

    case AluScalarOpcode::kTruncs: {
      dest = CreateGlslStd450InstructionCall(spv::NoPrecision, float_type_,
                                             GLSLstd450::kTrunc, {sources[0]});
    } break;

    default:
      assert_unhandled_case(instr.scalar_opcode);
      break;
  }

  assert_not_zero(dest);
  if (dest) {
    b.createStore(dest, ps_);
    StoreToResult(dest, instr.result);
  }

  if (close_predicated_block && open_predicated_block_) {
    b.createBranch(predicated_block_end_);
    b.setBuildPoint(predicated_block_end_);
    open_predicated_block_ = false;
    predicated_block_cond_ = false;
    predicated_block_end_ = nullptr;
  }
}

Id SpirvShaderTranslator::CreateGlslStd450InstructionCall(
    spv::Decoration precision, Id result_type, GLSLstd450 instruction_ordinal,
    std::vector<Id> args) {
  return builder_->createBuiltinCall(result_type, glsl_std_450_instruction_set_,
                                     static_cast<int>(instruction_ordinal),
                                     args);
}

Id SpirvShaderTranslator::LoadFromOperand(const InstructionOperand& op) {
  auto& b = *builder_;

  Id storage_pointer = 0;
  Id storage_type = vec4_float_type_;
  spv::StorageClass storage_class;
  Id storage_index = 0;             // Storage index at lowest level
  std::vector<Id> storage_offsets;  // Offsets in nested arrays -> storage

  // Out of the 512 constant registers pixel shaders get the last 256.
  uint32_t storage_base = 0;
  if (op.storage_source == InstructionStorageSource::kConstantFloat) {
    storage_base = is_pixel_shader() ? 256 : 0;
  }

  switch (op.storage_addressing_mode) {
    case InstructionStorageAddressingMode::kStatic: {
      storage_index = b.makeUintConstant(storage_base + op.storage_index);
    } break;
    case InstructionStorageAddressingMode::kAddressAbsolute: {
      // storage_index + a0
      storage_index =
          b.createBinOp(spv::Op::OpIAdd, uint_type_, b.createLoad(a0_),
                        b.makeUintConstant(storage_base + op.storage_index));
    } break;
    case InstructionStorageAddressingMode::kAddressRelative: {
      // TODO: Based on loop index
      // storage_index + aL.x
      storage_index =
          b.createBinOp(spv::Op::OpIAdd, uint_type_, b.makeUintConstant(0),
                        b.makeUintConstant(storage_base + op.storage_index));
    } break;
    default:
      assert_always();
      break;
  }

  switch (op.storage_source) {
    case InstructionStorageSource::kRegister:
      storage_pointer = registers_ptr_;
      storage_class = spv::StorageClass::StorageClassFunction;
      storage_type = vec4_float_type_;
      storage_offsets.push_back(storage_index);
      break;
    case InstructionStorageSource::kConstantFloat:
      storage_pointer = consts_;
      storage_class = spv::StorageClass::StorageClassUniform;
      storage_type = vec4_float_type_;
      storage_offsets.push_back(b.makeUintConstant(0));
      storage_offsets.push_back(storage_index);
      break;
    case InstructionStorageSource::kVertexFetchConstant:
    case InstructionStorageSource::kTextureFetchConstant:
      // Should not reach this.
      assert_always();
      break;
    default:
      assert_always();
      break;
  }

  if (!storage_pointer) {
    return b.createUndefined(vec4_float_type_);
  }

  storage_pointer =
      b.createAccessChain(storage_class, storage_pointer, storage_offsets);
  auto storage_value = b.createLoad(storage_pointer);
  assert_true(b.getTypeId(storage_value) == vec4_float_type_);

  if (op.is_absolute_value) {
    storage_value = CreateGlslStd450InstructionCall(
        spv::NoPrecision, storage_type, GLSLstd450::kFAbs, {storage_value});
  }
  if (op.is_negated) {
    storage_value =
        b.createUnaryOp(spv::Op::OpFNegate, storage_type, storage_value);
  }

  // swizzle
  if (!op.is_standard_swizzle()) {
    std::vector<uint32_t> operands;
    operands.push_back(storage_value);
    operands.push_back(b.makeCompositeConstant(
        vec2_float_type_,
        std::vector<Id>({b.makeFloatConstant(0.f), b.makeFloatConstant(1.f)})));

    // Components start from left and are duplicated rightwards
    // e.g. count = 1, xxxx / count = 2, xyyy ...
    for (int i = 0; i < 4; i++) {
      auto swiz = op.components[i];
      if (i > op.component_count - 1) {
        swiz = op.components[op.component_count - 1];
      }

      switch (swiz) {
        case SwizzleSource::kX:
          operands.push_back(0);
          break;
        case SwizzleSource::kY:
          operands.push_back(1);
          break;
        case SwizzleSource::kZ:
          operands.push_back(2);
          break;
        case SwizzleSource::kW:
          operands.push_back(3);
          break;
        case SwizzleSource::k0:
          operands.push_back(4);
          break;
        case SwizzleSource::k1:
          operands.push_back(5);
          break;
      }
    }

    storage_value =
        b.createOp(spv::Op::OpVectorShuffle, storage_type, operands);
  }

  return storage_value;
}

void SpirvShaderTranslator::StoreToResult(Id source_value_id,
                                          const InstructionResult& result) {
  auto& b = *builder_;

  if (result.storage_target == InstructionStorageTarget::kNone) {
    // No-op?
    return;
  }

  if (!result.has_any_writes()) {
    return;
  }

  Id storage_pointer = 0;
  Id storage_type = vec4_float_type_;
  spv::StorageClass storage_class;
  Id storage_index = 0;             // Storage index at lowest level
  std::vector<Id> storage_offsets;  // Offsets in nested arrays -> storage

  switch (result.storage_addressing_mode) {
    case InstructionStorageAddressingMode::kStatic: {
      storage_index = b.makeUintConstant(result.storage_index);
    } break;
    case InstructionStorageAddressingMode::kAddressAbsolute: {
      // storage_index + a0
      storage_index =
          b.createBinOp(spv::Op::OpIAdd, uint_type_, b.createLoad(a0_),
                        b.makeUintConstant(result.storage_index));
    } break;
    case InstructionStorageAddressingMode::kAddressRelative: {
      // storage_index + aL.x
      // TODO
    } break;
    default:
      assert_always();
      return;
  }

  bool storage_array;
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      storage_pointer = registers_ptr_;
      storage_class = spv::StorageClass::StorageClassFunction;
      storage_type = vec4_float_type_;
      storage_offsets.push_back(storage_index);
      storage_array = true;
      break;
    case InstructionStorageTarget::kInterpolant:
      assert_true(is_vertex_shader());
      storage_pointer = interpolators_;
      storage_class = spv::StorageClass::StorageClassOutput;
      storage_type = vec4_float_type_;
      storage_offsets.push_back(storage_index);
      storage_array = true;
      break;
    case InstructionStorageTarget::kPosition:
      assert_true(is_vertex_shader());
      assert_not_zero(pos_);
      storage_pointer = pos_;
      storage_class = spv::StorageClass::StorageClassOutput;
      storage_type = vec4_float_type_;
      storage_offsets.push_back(0);
      storage_array = false;
      break;
    case InstructionStorageTarget::kPointSize:
      assert_true(is_vertex_shader());
      // TODO(benvanik): result.storage_index
      break;
    case InstructionStorageTarget::kColorTarget:
      assert_true(is_pixel_shader());
      assert_not_zero(frag_outputs_);
      storage_pointer = frag_outputs_;
      storage_class = spv::StorageClass::StorageClassOutput;
      storage_type = vec4_float_type_;
      storage_offsets.push_back(storage_index);
      storage_array = true;
      break;
    case InstructionStorageTarget::kDepth:
      assert_true(is_pixel_shader());
      storage_pointer = frag_depth_;
      storage_class = spv::StorageClass::StorageClassOutput;
      storage_type = float_type_;
      storage_offsets.push_back(0);
      storage_array = false;
      break;
    case InstructionStorageTarget::kNone:
      assert_unhandled_case(result.storage_target);
      break;
  }

  if (!storage_pointer) {
    // assert_always();
    return;
  }

  if (storage_array) {
    storage_pointer =
        b.createAccessChain(storage_class, storage_pointer, storage_offsets);
  }

  // Only load from storage if we need it later.
  Id storage_value = 0;
  if (!result.has_all_writes()) {
    storage_value = b.createLoad(storage_pointer);
  }

  // Clamp the input value.
  if (result.is_clamped) {
    source_value_id = CreateGlslStd450InstructionCall(
        spv::NoPrecision, b.getTypeId(source_value_id),
        spv::GLSLstd450::kFClamp,
        {source_value_id, b.makeFloatConstant(0.0), b.makeFloatConstant(1.0)});
  }

  // Convert to the appropriate type, if needed.
  if (b.getTypeId(source_value_id) != storage_type) {
    std::vector<Id> constituents;
    auto n_el = b.getNumComponents(source_value_id);
    auto n_dst = b.getNumTypeComponents(storage_type);
    assert_true(n_el < n_dst);

    if (n_el == 1) {
      // Smear scalar.
      for (int i = 0; i < n_dst; i++) {
        constituents.push_back(source_value_id);
      }
    } else {
      // FIXME: This may not work as intended.
      constituents.push_back(source_value_id);
      for (int i = n_el; i < n_dst; i++) {
        // Pad with zeroes.
        constituents.push_back(b.makeFloatConstant(0.f));
      }
    }

    source_value_id =
        b.createConstructor(spv::NoPrecision, constituents, storage_type);
  }

  // swizzle
  if (!result.is_standard_swizzle()) {
    std::vector<uint32_t> operands;
    operands.push_back(source_value_id);
    operands.push_back(b.makeCompositeConstant(
        vec2_float_type_,
        std::vector<Id>({b.makeFloatConstant(0.f), b.makeFloatConstant(1.f)})));

    // Components start from left and are duplicated rightwards
    // e.g. count = 1, xxxx / count = 2, xyyy ...
    for (int i = 0; i < b.getNumTypeComponents(storage_type); i++) {
      auto swiz = result.components[i];
      if (!result.write_mask[i]) {
        // Undefined / don't care.
        operands.push_back(0);
        continue;
      }

      switch (swiz) {
        case SwizzleSource::kX:
          operands.push_back(0);
          break;
        case SwizzleSource::kY:
          operands.push_back(1);
          break;
        case SwizzleSource::kZ:
          operands.push_back(2);
          break;
        case SwizzleSource::kW:
          operands.push_back(3);
          break;
        case SwizzleSource::k0:
          operands.push_back(4);
          break;
        case SwizzleSource::k1:
          operands.push_back(5);
          break;
      }
    }

    source_value_id =
        b.createOp(spv::Op::OpVectorShuffle, storage_type, operands);
  }

  // write mask
  if (!result.has_all_writes()) {
    std::vector<uint32_t> operands;
    operands.push_back(source_value_id);
    operands.push_back(storage_value);

    for (int i = 0; i < b.getNumTypeComponents(storage_type); i++) {
      operands.push_back(
          result.write_mask[i] ? i : b.getNumComponents(source_value_id) + i);
    }

    source_value_id =
        b.createOp(spv::Op::OpVectorShuffle, storage_type, operands);
  }

  // Perform store into the pointer.
  assert_true(b.getNumComponents(source_value_id) ==
              b.getNumTypeComponents(storage_type));

  assert_true(b.getTypeId(source_value_id) ==
              b.getDerefTypeId(storage_pointer));
  b.createStore(source_value_id, storage_pointer);
}

}  // namespace gpu
}  // namespace xe
