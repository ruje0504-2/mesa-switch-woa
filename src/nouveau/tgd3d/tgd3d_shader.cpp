/* SPDX-License-Identifier: MIT
 *
 * Shader Model 4 bytecode to Vulkan shader objects.
 *
 * The D3D runtime hands a driver the SHDR token stream and a signature
 * summary (system value, register, mask), not the DXBC container the
 * application compiled. dxbc-spirv consumes containers, so a minimal one is
 * rebuilt here: ISGN and OSGN are restated from the DDI entries and the
 * token stream is embedded unchanged. The DDI summary has no component
 * types; those only matter where Vulkan requires them to agree with a
 * format, which is why vertex shaders have one variant per input-layout
 * numeric class and pixel shaders one per render-target numeric class.
 *
 * Each stage is an independent VkShaderEXT created against one fixed
 * pipeline layout, so shaders compile once, at creation, and binding a
 * different pixel shader never recompiles the vertex shader.
 */
#include "tgd3d.h"

#include "dxbc/dxbc_api.h"
#include "ir/passes/ir_pass_lower_io.h"
#include "spirv/spirv_builder.h"
#include "spirv/spirv_mapping.h"
#include "util/util_log.h"

#include <string.h>

namespace tgd3d {

using namespace dxbc_spv;

namespace {

class TraceLogger : public util::Logger {
public:
   util::LogLevel getMinimumSeverity() override { return util::LogLevel::eError; }
   void message(util::LogLevel, const char *text) override { trace_message(text); }
};

class Mapping : public spirv::ResourceMapping {
public:
   Mapping(Stage stage, uint32_t cb_slots, const uint32_t *buffer_mask)
   : m_stage(stage), m_cbSlots(cb_slots), m_bufferMask(buffer_mask) { }

   spirv::DescriptorBinding mapDescriptor(ir::ScalarType type, uint32_t space,
                                          uint32_t index) override
   {
      spirv::DescriptorBinding binding = {};
      if (space)
         failed = true;
      switch (type) {
      case ir::ScalarType::eCbv:
         failed |= index >= m_cbSlots;
         binding.set = SET_CB;
         binding.binding = m_stage * m_cbSlots + index;
         break;
      case ir::ScalarType::eSampler:
         failed |= index >= D3D_SAMPLER_SLOTS;
         binding.set = SET_TEX;
         binding.binding = m_stage * SET1_STRIDE + index;
         break;
      case ir::ScalarType::eSrv:
         failed |= index >= D3D_SRV_SLOTS;
         if (index < D3D_SRV_SLOTS && (m_bufferMask[index / 32] & (1u << (index % 32)))) {
            binding.set = SET_TBUF;
            binding.binding = m_stage * D3D_SRV_SLOTS + index;
         } else {
            binding.set = SET_TEX;
            binding.binding = m_stage * SET1_STRIDE + D3D_SAMPLER_SLOTS + index;
         }
         break;
      default:
         failed = true;   /* unordered access does not exist in Direct3D 10 */
         break;
      }
      return binding;
   }

   uint32_t mapPushData(ir::ShaderStageMask) override { return 0; }

   bool failed = false;

private:
   Stage m_stage;
   uint32_t m_cbSlots;
   const uint32_t *m_bufferMask;
};

void put32(std::vector<uint8_t> &out, uint32_t value)
{
   const uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
                             (uint8_t)(value >> 24)};
   out.insert(out.end(), bytes, bytes + 4);
}

/* ISGN/OSGN body. `classes` holds two bits per register: 0 float, 1 uint,
 * 2 sint. Every entry shares one placeholder semantic name; stages link by
 * register, exactly as Direct3D 10 does. */
std::vector<uint8_t> signature_chunk(const std::vector<D3D10DDIARG_SIGNATURE_ENTRY> &entries,
                                     bool input, uint32_t classes)
{
   std::vector<uint8_t> out;
   put32(out, (uint32_t)entries.size());
   put32(out, 8);
   const uint32_t name_offset = 8 + (uint32_t)entries.size() * 24;
   for (size_t i = 0; i < entries.size(); i++) {
      const auto &e = entries[i];
      uint32_t type = 3;
      if (e.Register < 16) {
         const uint32_t c = (classes >> (2 * e.Register)) & 3;
         type = c == 1 ? 1 : c == 2 ? 2 : 3;
      }
      put32(out, name_offset);
      put32(out, (uint32_t)i);
      put32(out, (uint32_t)e.SystemValue);
      put32(out, type);
      put32(out, e.Register);
      put32(out, (uint32_t)e.Mask | (input ? (uint32_t)e.Mask << 8 : 0));
   }
   static const char name[] = "TGIO\0\0\0";
   out.insert(out.end(), name, name + 8);
   return out;
}

std::vector<uint8_t> build_container(const Shader *shader, uint32_t in_classes, uint32_t out_classes)
{
   const std::vector<uint8_t> isgn = signature_chunk(shader->inputs, true, in_classes);
   const std::vector<uint8_t> osgn = signature_chunk(shader->outputs, false, out_classes);
   const uint32_t code_bytes = (uint32_t)shader->tokens.size() * 4;
   const uint32_t header = 32 + 3 * 4;
   std::vector<uint8_t> out;
   out.insert(out.end(), {'D', 'X', 'B', 'C'});
   out.resize(20);           /* the checksum is not consulted by the converter */
   put32(out, 1);
   put32(out, header + 3 * 8 + (uint32_t)isgn.size() + (uint32_t)osgn.size() + code_bytes);
   put32(out, 3);
   put32(out, header);
   put32(out, header + 8 + (uint32_t)isgn.size());
   put32(out, header + 16 + (uint32_t)isgn.size() + (uint32_t)osgn.size());
   out.insert(out.end(), {'I', 'S', 'G', 'N'});
   put32(out, (uint32_t)isgn.size());
   out.insert(out.end(), isgn.begin(), isgn.end());
   out.insert(out.end(), {'O', 'S', 'G', 'N'});
   put32(out, (uint32_t)osgn.size());
   out.insert(out.end(), osgn.begin(), osgn.end());
   out.insert(out.end(), {'S', 'H', 'D', 'R'});
   put32(out, code_bytes);
   const uint8_t *code = reinterpret_cast<const uint8_t *>(shader->tokens.data());
   out.insert(out.end(), code, code + code_bytes);
   return out;
}

void reflect(Shader *shader, const ir::Builder &builder)
{
   auto [begin, end] = builder.getDeclarations();
   for (auto iter = begin; iter != end; iter++) {
      const ir::Op &op = *iter;
      switch (op.getOpCode()) {
      case ir::OpCode::eDclCbv: {
         const uint32_t index = uint32_t(op.getOperand(2u));
         if (index < D3D_CB_SLOTS)
            shader->cbMask |= 1u << index;
         break;
      }
      case ir::OpCode::eDclSampler: {
         const uint32_t index = uint32_t(op.getOperand(2u));
         if (index < D3D_SAMPLER_SLOTS)
            shader->samplerMask |= 1u << index;
         break;
      }
      case ir::OpCode::eDclSrv: {
         const uint32_t index = uint32_t(op.getOperand(2u));
         const auto kind = ir::ResourceKind(op.getOperand(4u));
         if (index >= D3D_SRV_SLOTS)
            break;
         shader->srvMask[index / 32] |= 1u << (index % 32);
         shader->srvKind[index] = (uint8_t)kind;
         if (kind == ir::ResourceKind::eBufferTyped || kind == ir::ResourceKind::eBufferRaw ||
             kind == ir::ResourceKind::eBufferStructured) {
            shader->srvBufferMask[index / 32] |= 1u << (index % 32);
            shader->usesTexelBuffers = true;
         }
         break;
      }
      case ir::OpCode::eDclInput:
         if (shader->stage == STAGE_VS) {
            const uint32_t location = uint32_t(op.getOperand(1u));
            if (location < 16)
               shader->inputRegisters |= 1u << location;
         }
         break;
      case ir::OpCode::eDclOutput:
         if (shader->stage == STAGE_PS) {
            const uint32_t location = uint32_t(op.getOperand(1u));
            if (location < 8)
               shader->outputRegisters |= 1u << location;
         }
         break;
      default:
         break;
      }
   }
   shader->reflected = true;
}

VkShaderEXT compile(Shader *shader, uint32_t key)
{
   Device *dev = shader->dev;
   const VulkanCaps &caps = *dev->caps;
   const uint64_t begin = qpc_now();
   const uint64_t identity = (uintptr_t)shader;
   const uint64_t frontend = (UINT64_C(1) << 32) | shader->stage;
   const uint64_t backend = (UINT64_C(2) << 32) | shader->stage;
   trace(TGD3D_TRACE_SHADER_PHASE, E_PENDING, frontend, identity);
   TraceLogger logger;

   uint32_t in_classes = 0, out_classes = 0;
   bool dual_source = false;
   if (shader->stage == STAGE_VS)
      in_classes = key;
   if (shader->stage == STAGE_PS) {
      out_classes = key & 0xffff;
      dual_source = (key >> 31) != 0;
   }
   const std::vector<uint8_t> container = build_container(shader, in_classes, out_classes);

   dxbc::Converter::Options convert = {};
   convert.includeDebugNames = false;
   ir::CompileOptions options = {};
   auto ir = dxbc::compileShaderToLegalizedIr(container.data(), container.size(), convert, options);
   if (!ir) {
      shader->error = E_INVALIDARG;
      trace(TGD3D_TRACE_SHADER_PHASE, shader->error, frontend, identity);
      return VK_NULL_HANDLE;
   }
   {
      ir::LowerIoPass pass(*ir);
      pass.lowerSampleCountToSpecConstant(0u);
   }
   if (!shader->reflected)
      reflect(shader, *ir);

   Mapping mapping(shader->stage, caps.cbSlots, shader->srvBufferMask);
   spirv::SpirvBuilder::Options spv = {};
   spv.includeDebugNames = false;
   spv.dualSourceBlending = dual_source;
   spv.floatControls2 = false;
   if (caps.roundRTE)
      spv.supportedRoundModesF32 |= ir::RoundMode::eNearestEven;
   if (caps.roundRTZ)
      spv.supportedRoundModesF32 |= ir::RoundMode::eZero;
   if (caps.denormPreserve)
      spv.supportedDenormModesF32 |= ir::DenormMode::ePreserve;
   if (caps.denormFlush)
      spv.supportedDenormModesF32 |= ir::DenormMode::eFlush;
   spv.supportsZeroInfNanPreserveF32 = caps.zeroInfNanPreserve;
   spirv::SpirvBuilder builder(*ir, mapping, spv);
   builder.buildSpirvBinary();
   const std::vector<uint32_t> code = builder.getSpirvBinary();
   if (mapping.failed || code.empty()) {
      /* A register this layout cannot address; nothing is remapped silently. */
      shader->error = DXGI_ERROR_UNSUPPORTED;
      trace(TGD3D_TRACE_SHADER_PHASE, shader->error, frontend, identity);
      return VK_NULL_HANDLE;
   }
   trace(TGD3D_TRACE_SHADER_PHASE, S_OK, frontend, identity);

   VkShaderCreateInfoEXT info = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
   switch (shader->stage) {
   case STAGE_VS:
      info.stage = VK_SHADER_STAGE_VERTEX_BIT;
      info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT |
                       (caps.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
      break;
   case STAGE_GS:
      info.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
      info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
      break;
   default:
      info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      break;
   }
   info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
   info.codeSize = code.size() * sizeof(uint32_t);
   info.pCode = code.data();
   info.pName = "main";
   info.setLayoutCount = SET_COUNT;
   info.pSetLayouts = dev->setLayouts;
   VkShaderEXT handle = VK_NULL_HANDLE;
   trace(TGD3D_TRACE_SHADER_PHASE, E_PENDING, backend, identity);
   const VkResult result = dev->vk.CreateShadersEXT(dev->vkdev, 1, &info, nullptr, &handle);
   trace(TGD3D_TRACE_SHADER_PHASE, result == VK_SUCCESS ? S_OK : E_FAIL, backend, identity);
   const uint64_t end = qpc_now();
   LARGE_INTEGER frequency;
   QueryPerformanceFrequency(&frequency);
   trace(TGD3D_TRACE_SHADER, result == VK_SUCCESS ? S_OK : E_FAIL,
         (uint64_t)shader->stage << 32 | (uint32_t)shader->tokens.size(),
         (end - begin) * 1000000 / (uint64_t)frequency.QuadPart);
   if (result != VK_SUCCESS) {
      shader->error = result == VK_ERROR_OUT_OF_HOST_MEMORY ? E_OUTOFMEMORY : E_FAIL;
      return VK_NULL_HANDLE;
   }
   return handle;
}

SIZE_T APIENTRY CalcPrivateShaderSize(D3D10DDI_HDEVICE, const UINT *, const D3D10DDIARG_STAGE_IO_SIGNATURES *)
{
   return sizeof(Shader);
}

void create_shader(D3D10DDI_HDEVICE hDevice, const UINT *code, D3D10DDI_HSHADER handle, Stage stage,
                   const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
   Device *dev = D(hDevice);
   Shader *shader = new (handle.pDrvPrivate) Shader();
   shader->dev = dev;
   shader->stage = stage;
   if (stage == STAGE_GS && !dev->caps->geometryShader) {
      shader->error = DXGI_ERROR_UNSUPPORTED;
      TG_ERROR(dev, shader->error);
      return;
   }
   /* Token 1 of a shader program is its length in 32-bit words. */
   const UINT length = code[1];
   shader->tokens.assign(code, code + length);
   if (signatures) {
      shader->inputs.assign(signatures->pInputSignature,
                            signatures->pInputSignature + signatures->NumInputSignatureEntries);
      shader->outputs.assign(signatures->pOutputSignature,
                             signatures->pOutputSignature + signatures->NumOutputSignatureEntries);
   }
   VkShaderEXT base = compile(shader, 0);
   if (!base) {
      TG_ERROR(dev, FAILED(shader->error) ? shader->error : E_FAIL);
      return;
   }
   shader->variants.push_back({0, base});
}

void APIENTRY CreateVertexShader(D3D10DDI_HDEVICE hDevice, const UINT *code, D3D10DDI_HSHADER handle,
                                 D3D10DDI_HRTSHADER, const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
   create_shader(hDevice, code, handle, STAGE_VS, signatures);
}

void APIENTRY CreateGeometryShader(D3D10DDI_HDEVICE hDevice, const UINT *code, D3D10DDI_HSHADER handle,
                                   D3D10DDI_HRTSHADER, const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
   create_shader(hDevice, code, handle, STAGE_GS, signatures);
}

void APIENTRY CreatePixelShader(D3D10DDI_HDEVICE hDevice, const UINT *code, D3D10DDI_HSHADER handle,
                                D3D10DDI_HRTSHADER, const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
   create_shader(hDevice, code, handle, STAGE_PS, signatures);
}

SIZE_T APIENTRY CalcPrivateGeometryShaderWithStreamOutput(
   D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *,
   const D3D10DDIARG_STAGE_IO_SIGNATURES *)
{
   return sizeof(Shader);
}

void APIENTRY CreateGeometryShaderWithStreamOutput(
   D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *args,
   D3D10DDI_HSHADER handle, D3D10DDI_HRTSHADER, const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
   Device *dev = D(hDevice);
   if (args->NumEntries) {
      /* Stream output is not implemented: capturing needs transform
       * feedback, and DrawAuto needs an indirect draw that the Windows
       * submission transport cannot carry yet. The object stays unusable. */
      Shader *shader = new (handle.pDrvPrivate) Shader();
      shader->dev = dev;
      shader->stage = STAGE_GS;
      shader->error = E_NOTIMPL;
      TG_ERROR(dev, E_NOTIMPL);
      return;
   }
   create_shader(hDevice, args->pShaderCode, handle, STAGE_GS, signatures);
}

void APIENTRY DestroyShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER handle)
{
   Device *dev = D(hDevice);
   Shader *shader = static_cast<Shader *>(handle.pDrvPrivate);
   for (auto &stage : dev->stage) {
      if (stage.shader == shader) {
         stage.shader = nullptr;
         dev->dirty |= DIRTY_SHADERS;
      }
   }
   for (const auto &variant : shader->variants)
      defer(dev, DeferredType::Shader, (uint64_t)variant.shader, dev->lastSeq());
   shader->~Shader();
}

void set_shader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER handle, Stage stage)
{
   Device *dev = D(hDevice);
   Shader *shader = static_cast<Shader *>(handle.pDrvPrivate);
   if (dev->stage[stage].shader == shader)
      return;
   dev->stage[stage].shader = shader;
   /* A different shader reads a different set of slots. */
   dev->stage[stage].cbDirty = ~0u;
   dev->dirty |= DIRTY_SHADERS | DIRTY_TEXTURES | DIRTY_TEXEL_BUFFERS;
}

void APIENTRY VsSetShader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { set_shader(h, s, STAGE_VS); }
void APIENTRY GsSetShader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { set_shader(h, s, STAGE_GS); }
void APIENTRY PsSetShader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { set_shader(h, s, STAGE_PS); }

}

VkShaderEXT shader_variant(Shader *shader, uint32_t key)
{
   if (!shader || FAILED(shader->error))
      return VK_NULL_HANDLE;
   for (const auto &variant : shader->variants)
      if (variant.key == key)
         return variant.shader;
   VkShaderEXT handle = compile(shader, key);
   if (!handle) {
      TG_ERROR(shader->dev, FAILED(shader->error) ? shader->error : E_FAIL);
      shader->error = S_OK;       /* the base variant stays usable */
      return VK_NULL_HANDLE;
   }
   shader->variants.push_back({key, handle});
   return handle;
}

void shader_fill_table(D3D10DDI_DEVICEFUNCS *t)
{
   t->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
   t->pfnCreateVertexShader = CreateVertexShader;
   t->pfnCreateGeometryShader = CreateGeometryShader;
   t->pfnCreatePixelShader = CreatePixelShader;
   t->pfnCalcPrivateGeometryShaderWithStreamOutput = CalcPrivateGeometryShaderWithStreamOutput;
   t->pfnCreateGeometryShaderWithStreamOutput = CreateGeometryShaderWithStreamOutput;
   t->pfnDestroyShader = DestroyShader;
   t->pfnVsSetShader = VsSetShader;
   t->pfnGsSetShader = GsSetShader;
   t->pfnPsSetShader = PsSetShader;
}

}
