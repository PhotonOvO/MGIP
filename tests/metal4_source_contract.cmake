if(NOT DEFINED SOURCE_ROOT OR NOT IS_DIRECTORY "${SOURCE_ROOT}/rhi/metal")
  message(FATAL_ERROR "SOURCE_ROOT does not contain rhi/metal")
endif()

file(GLOB_RECURSE FORBIDDEN_METAL4_SOURCE_FILES
  "${SOURCE_ROOT}/rhi/metal/*.metal"
  "${SOURCE_ROOT}/rhi/metal/*.mm")
if(FORBIDDEN_METAL4_SOURCE_FILES)
  list(JOIN FORBIDDEN_METAL4_SOURCE_FILES "\n  " FORBIDDEN_METAL4_SOURCE_LIST)
  message(FATAL_ERROR
    "Metal RHI shader/API sources must be Slang and C++/metal-cpp only:\n  ${FORBIDDEN_METAL4_SOURCE_LIST}")
endif()

file(GLOB_RECURSE METAL4_CONTRACT_FILES
  "${SOURCE_ROOT}/rhi/metal/*.h"
  "${SOURCE_ROOT}/rhi/metal/*.hpp"
  "${SOURCE_ROOT}/rhi/metal/*.cpp")

set(FORBIDDEN_METAL_COMMAND_APIS
  "#import <Metal/Metal.h>"
  "id<MTL"
  "MTL::CommandQueue"
  "MTL::CommandBuffer"
  "MTL::RenderCommandEncoder"
  "MTL::ComputeCommandEncoder"
  "MTL::BlitCommandEncoder"
  "id<MTLCommandQueue>"
  "id<MTLCommandBuffer>"
  "id<MTLRenderCommandEncoder>"
  "id<MTLComputeCommandEncoder>"
  "id<MTLBlitCommandEncoder>"
  "objc_msgSend"
  "objc_getProtocol"
  "MTL4::LibraryDescriptor"
  "setSource("
  "#include <metal_stdlib>"
  "[[kernel]]"
  "presentDrawable")

set(ALL_METAL4_SOURCE "")
foreach(FILE_PATH IN LISTS METAL4_CONTRACT_FILES)
  file(READ "${FILE_PATH}" FILE_CONTENT)
  string(APPEND ALL_METAL4_SOURCE "\n${FILE_CONTENT}")
  foreach(FORBIDDEN_API IN LISTS FORBIDDEN_METAL_COMMAND_APIS)
    string(FIND "${FILE_CONTENT}" "${FORBIDDEN_API}" FORBIDDEN_OFFSET)
    if(NOT FORBIDDEN_OFFSET EQUAL -1)
      message(FATAL_ERROR
        "Forbidden legacy Metal command API '${FORBIDDEN_API}' appears in ${FILE_PATH}")
    endif()
  endforeach()
endforeach()

set(REQUIRED_METAL4_APIS
  "MTL4::CommandQueue"
  "MTL4::CommandAllocator"
  "MTL4::CommandBuffer"
  "MTL4::RenderCommandEncoder"
  "MTL4::ComputeCommandEncoder"
  "MTL4::ArgumentTable"
  "MTL4::Compiler"
  "signalDrawable"
  "->present()")
foreach(REQUIRED_API IN LISTS REQUIRED_METAL4_APIS)
  string(FIND "${ALL_METAL4_SOURCE}" "${REQUIRED_API}" REQUIRED_OFFSET)
  if(REQUIRED_OFFSET EQUAL -1)
    message(FATAL_ERROR "Required Metal 4 API '${REQUIRED_API}' is missing")
  endif()
endforeach()

file(READ "${SOURCE_ROOT}/rhi/metal/MetalSwapchain.cpp" METAL4_SWAPCHAIN_SOURCE)
foreach(FORBIDDEN_DRAWABLE_RENDER_API IN ITEMS
    "renderCommandEncoder"
    "drawPrimitives"
    "drawIndexedPrimitives")
  string(FIND "${METAL4_SWAPCHAIN_SOURCE}"
    "${FORBIDDEN_DRAWABLE_RENDER_API}" FORBIDDEN_DRAWABLE_RENDER_OFFSET)
  if(NOT FORBIDDEN_DRAWABLE_RENDER_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Drawable presentation uses forbidden render API '${FORBIDDEN_DRAWABLE_RENDER_API}'")
  endif()
endforeach()
string(FIND "${METAL4_SWAPCHAIN_SOURCE}" "copyFromTexture"
  DRAWABLE_COMPUTE_COPY_OFFSET)
if(DRAWABLE_COMPUTE_COPY_OFFSET EQUAL -1)
  message(FATAL_ERROR "Drawable presentation is missing its Metal 4 compute copy")
endif()
foreach(DRAWABLE_RESIDENCY_API IN ITEMS
    "newResidencySet"
    "addAllocation(composition->nativeTexture)"
    "useResidencySet(image.presentResidencySet)"
    "m_layer->residencySet()"
    "useResidencySet(drawableResidencySet)")
  string(FIND "${METAL4_SWAPCHAIN_SOURCE}" "${DRAWABLE_RESIDENCY_API}"
    DRAWABLE_RESIDENCY_OFFSET)
  if(DRAWABLE_RESIDENCY_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Drawable presentation residency is missing '${DRAWABLE_RESIDENCY_API}'")
  endif()
endforeach()

file(READ "${SOURCE_ROOT}/rhi/metal/MetalSurface.cpp" METAL4_SURFACE_SOURCE)
string(FIND "${METAL4_SURFACE_SOURCE}" "setAllowsNextDrawableTimeout(false)"
  DRAWABLE_BLOCKING_ACQUIRE_OFFSET)
if(DRAWABLE_BLOCKING_ACQUIRE_OFFSET EQUAL -1)
  message(FATAL_ERROR
    "CAMetalLayer must use blocking drawable acquisition during live resize")
endif()

file(READ "${SOURCE_ROOT}/rhi/metal/MetalQueue.cpp" METAL4_QUEUE_SOURCE)
string(FIND "${METAL4_QUEUE_SOURCE}" "m_queue->signalDrawable(drawable)"
  DRAWABLE_SIGNAL_OFFSET)
string(FIND "${METAL4_QUEUE_SOURCE}" "m_queue->signalEvent(m_completionEvent, submissionValue)"
  PRESENT_COMPLETION_OFFSET REVERSE)
if(DRAWABLE_SIGNAL_OFFSET EQUAL -1 OR PRESENT_COMPLETION_OFFSET EQUAL -1
   OR NOT DRAWABLE_SIGNAL_OFFSET LESS PRESENT_COMPLETION_OFFSET)
  message(FATAL_ERROR
    "Presentation completion must be signaled after signalDrawable")
endif()

# Slang's Metal target assigns direct buffer/texture/sampler indices from
# declaration order. The RHI lowers argument layouts in logical binding order,
# so this mixed-resource shader must keep its first binding declarations sorted.
file(READ "${SOURCE_ROOT}/shaders/ddgi_flax_trace_rays.slang"
  FLAX_TRACE_SHADER_SOURCE)
set(PREVIOUS_FLAX_BINDING_OFFSET -1)
foreach(BINDING RANGE 0 15)
  set(BINDING_TOKEN "[[vk::binding(${BINDING}, 0)]]")
  string(FIND "${FLAX_TRACE_SHADER_SOURCE}" "${BINDING_TOKEN}"
    FLAX_BINDING_OFFSET)
  if(FLAX_BINDING_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Flax DDGI trace shader is missing logical binding ${BINDING}")
  endif()
  if(FLAX_BINDING_OFFSET LESS_EQUAL PREVIOUS_FLAX_BINDING_OFFSET)
    message(FATAL_ERROR
      "Flax DDGI trace shader bindings are not declared in logical order at binding ${BINDING}")
  endif()
  set(PREVIOUS_FLAX_BINDING_OFFSET ${FLAX_BINDING_OFFSET})
endforeach()

# The deferred-lighting module shares the same declaration-order ABI. Resource
# classes receive independent Metal index spaces, so its set-0 buffer bindings
# must be monotonically declared even though textures and samplers appear first.
file(READ "${SOURCE_ROOT}/shaders/shader.light.slang"
  LIGHTING_SHADER_SOURCE)
set(PREVIOUS_LIGHTING_BUFFER_OFFSET -1)
foreach(BINDING RANGE 2 8)
  set(BINDING_TOKEN "[[vk::binding(${BINDING}, 0)]]")
  string(FIND "${LIGHTING_SHADER_SOURCE}" "${BINDING_TOKEN}"
    LIGHTING_BUFFER_OFFSET)
  if(LIGHTING_BUFFER_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Deferred-lighting shader is missing set-0 buffer binding ${BINDING}")
  endif()
  if(LIGHTING_BUFFER_OFFSET LESS_EQUAL PREVIOUS_LIGHTING_BUFFER_OFFSET)
    message(FATAL_ERROR
      "Deferred-lighting buffer bindings are not declared in logical order at binding ${BINDING}")
  endif()
  set(PREVIOUS_LIGHTING_BUFFER_OFFSET ${LIGHTING_BUFFER_OFFSET})
endforeach()

# Metal and D3D-style viewports map positive NDC Y toward the top of a
# top-left-origin framebuffer. Keep every screen/depth reconstruction path on
# the shared conversion helper so deferred shadow lookups use the sampled
# GBuffer pixel's actual world position.
file(READ "${SOURCE_ROOT}/shaders/shader_io.h" SHADER_IO_SOURCE)
foreach(FRAMEBUFFER_CONTRACT_TOKEN IN ITEMS
    "defined(DEMO_D3D12) || defined(DEMO_METAL)"
    "#define DEMO_FRAMEBUFFER_Y_FLIP 1"
    "demoFramebufferUvToNdc"
    "demoNdcToFramebufferUv")
  string(FIND "${SHADER_IO_SOURCE}" "${FRAMEBUFFER_CONTRACT_TOKEN}"
    FRAMEBUFFER_CONTRACT_OFFSET)
  if(FRAMEBUFFER_CONTRACT_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Shared framebuffer coordinate contract is missing '${FRAMEBUFFER_CONTRACT_TOKEN}'")
  endif()
endforeach()
string(FIND "${LIGHTING_SHADER_SOURCE}"
  "demoNdcToFramebufferUv(shadowNdc.xy)" METAL_SHADOW_Y_FLIP_OFFSET)
if(METAL_SHADOW_Y_FLIP_OFFSET EQUAL -1)
  message(FATAL_ERROR
    "Deferred Metal shadow lookup bypasses the shared NDC-to-framebuffer conversion")
endif()

file(READ "${SOURCE_ROOT}/rhi/metal/MetalBuiltins.slang"
  METAL4_BUILTIN_SHADER_SOURCE)
foreach(REQUIRED_SLANG_TOKEN IN ITEMS
    "__target_intrinsic(metal, \"command_buffer\")"
    "__target_intrinsic(metal, \"primitive_type\")"
    "__target_intrinsic(metal, \"render_command\")"
    "ParameterBlock<MetalIndexedIndirectCommandBufferArguments>"
    "__intrinsic_asm \"render_command($0, $1)\""
    "primitive_type::triangle_strip : primitive_type::triangle"
    "mgipMetal4Blit2D"
    "mgipPrepareIndexedIndirectUInt32"
    "mgipPrepareIndexedIndirectUInt16")
  string(FIND "${METAL4_BUILTIN_SHADER_SOURCE}" "${REQUIRED_SLANG_TOKEN}"
    REQUIRED_SLANG_TOKEN_OFFSET)
  if(REQUIRED_SLANG_TOKEN_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Metal built-in Slang shader is missing '${REQUIRED_SLANG_TOKEN}'")
  endif()
endforeach()

file(READ "${SOURCE_ROOT}/rhi/metal/MetalCommandBuffer.cpp"
  METAL4_COMMAND_BUFFER_SOURCE)
foreach(REQUIRED_ICB_ABI_TOKEN IN ITEMS
    "toMetalShaderPrimitive"
    ".primitiveType = toMetalShaderPrimitive(desc.topology)")
  string(FIND "${METAL4_COMMAND_BUFFER_SOURCE}" "${REQUIRED_ICB_ABI_TOKEN}"
    REQUIRED_ICB_ABI_TOKEN_OFFSET)
  if(REQUIRED_ICB_ABI_TOKEN_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Metal ICB shader ABI mapping is missing '${REQUIRED_ICB_ABI_TOKEN}'")
  endif()
endforeach()

file(READ "${SOURCE_ROOT}/rhi/metal/MetalPipelines.cpp"
  METAL4_PIPELINE_SOURCE)
foreach(REQUIRED_METALLIB_TOKEN IN ITEMS
    "#include \"_autogen/MetalBuiltins.metallib.h\""
    "mgip_metal_builtins_lib"
    "device.newLibrary(payload, &error)"
    "mgipPrepareIndexedIndirectUInt32"
    "mgipPrepareIndexedIndirectUInt16")
  string(FIND "${METAL4_PIPELINE_SOURCE}" "${REQUIRED_METALLIB_TOKEN}"
    REQUIRED_METALLIB_TOKEN_OFFSET)
  if(REQUIRED_METALLIB_TOKEN_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Embedded Slang metallib path is missing '${REQUIRED_METALLIB_TOKEN}'")
  endif()
endforeach()

# Xcode 27 requires fragment resources used by ICB pipelines to live in an
# argument buffer. The renderer schema stays unchanged; Metal selects a Slang
# ICB variant and records an encoder for the nested material ParameterBlock.
foreach(REQUIRED_ICB_VARIANT_TOKEN IN ITEMS
    "MTL4::IndirectCommandBufferSupportStateEnabled"
    "desc.supportsGpuGeneratedCommands"
    "library->icbLibrary"
    "nestedArgumentBufferIndex"
    "nestedArgumentEncoder"
    "newArgumentEncoder")
  string(FIND "${METAL4_PIPELINE_SOURCE}" "${REQUIRED_ICB_VARIANT_TOKEN}"
    REQUIRED_ICB_VARIANT_OFFSET)
  if(REQUIRED_ICB_VARIANT_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Metal fragment-ICB variant path is missing '${REQUIRED_ICB_VARIANT_TOKEN}'")
  endif()
endforeach()

foreach(FORBIDDEN_ICB_FALLBACK_TOKEN IN ITEMS
    "Metal 3 ICB compatibility graphics pipeline"
    "compileMetal3IndirectGraphicsPipeline")
  string(FIND "${METAL4_PIPELINE_SOURCE}" "${FORBIDDEN_ICB_FALLBACK_TOKEN}"
    FORBIDDEN_ICB_FALLBACK_OFFSET)
  if(NOT FORBIDDEN_ICB_FALLBACK_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Metal pipeline source retained obsolete fallback '${FORBIDDEN_ICB_FALLBACK_TOKEN}'")
  endif()
endforeach()

file(READ "${SOURCE_ROOT}/rhi/metal/MetalCommandBuffer.cpp"
  METAL4_COMMAND_BUFFER_SOURCE)
foreach(REQUIRED_NESTED_ARGUMENT_TOKEN IN ITEMS
    "allocateArgumentData("
    "nestedEncoder->setArgumentBuffer("
    "nestedEncoder->setTexture("
    "nestedEncoder->setSamplerState("
    "nestedArguments.gpuAddress")
  string(FIND "${METAL4_COMMAND_BUFFER_SOURCE}" "${REQUIRED_NESTED_ARGUMENT_TOKEN}"
    REQUIRED_NESTED_ARGUMENT_OFFSET)
  if(REQUIRED_NESTED_ARGUMENT_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Metal nested argument-buffer encoding is missing '${REQUIRED_NESTED_ARGUMENT_TOKEN}'")
  endif()
endforeach()

file(READ "${SOURCE_ROOT}/cmake/CompilerSlangShader.cmake"
  METAL_SLANG_COMPILER_SOURCE)
foreach(REQUIRED_ICB_LIBRARY_TOKEN IN ITEMS
    "ICB_VARIANT_SHADERS"
    "-DDEMO_METAL_ICB=1"
    "_icb_metal_source"
    "-std=metal4.0"
    "findEmbeddedMetalICBVariantLibrary")
  string(FIND "${METAL_SLANG_COMPILER_SOURCE}" "${REQUIRED_ICB_LIBRARY_TOKEN}"
    REQUIRED_ICB_LIBRARY_OFFSET)
  if(REQUIRED_ICB_LIBRARY_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Metal ICB compatibility metallib build path is missing '${REQUIRED_ICB_LIBRARY_TOKEN}'")
  endif()
endforeach()

string(FIND "${METAL_SLANG_COMPILER_SOURCE}" "-std=metal3.2"
  FORBIDDEN_METAL32_OFFSET)
if(NOT FORBIDDEN_METAL32_OFFSET EQUAL -1)
  message(FATAL_ERROR "Metal ICB shaders must not use the old Metal 3.2 workaround")
endif()

foreach(ICB_SHADER IN ITEMS
    shader.depth_prepass.slang
    shader.gbuffer.slang
    shader.shadow.slang
    shader.forward.slang)
  file(READ "${SOURCE_ROOT}/shaders/${ICB_SHADER}" ICB_SHADER_SOURCE)
  foreach(REQUIRED_SHADER_TOKEN IN ITEMS
      "defined(DEMO_METAL_ICB)"
      "ParameterBlock<MetalIcbMaterialResources>"
      "Texture2D<float4> textures[LMaterialTextureCapacity]"
      "SamplerState sampler")
    string(FIND "${ICB_SHADER_SOURCE}" "${REQUIRED_SHADER_TOKEN}"
      REQUIRED_SHADER_OFFSET)
    if(REQUIRED_SHADER_OFFSET EQUAL -1)
      message(FATAL_ERROR
        "${ICB_SHADER} is missing ICB ParameterBlock token '${REQUIRED_SHADER_TOKEN}'")
    endif()
  endforeach()
endforeach()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" METAL4_CMAKE_SOURCE)
foreach(REQUIRED_SLANG_BUILD_TOKEN IN ITEMS
    "compile_slang_metal_embedded("
    "rhi/metal/MetalBuiltins.slang")
  string(FIND "${METAL4_CMAKE_SOURCE}" "${REQUIRED_SLANG_BUILD_TOKEN}"
    REQUIRED_SLANG_BUILD_TOKEN_OFFSET)
  if(REQUIRED_SLANG_BUILD_TOKEN_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Metal Slang build path is missing '${REQUIRED_SLANG_BUILD_TOKEN}'")
  endif()
endforeach()

message(STATUS "Metal 4 source contract passed")
