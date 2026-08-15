#include "../rhi/RHIDevice.h"
#include "../rhi/RHIFactory.h"
#include "../rhi/RHISurface.h"

#include <cassert>
#include <stdexcept>
#include <string_view>

int main()
{
  using demo::rhi::BackendType;

#if defined(DEMO_DEFAULT_RHI_METAL)
  constexpr BackendType expectedDefaultBackend = BackendType::metal;
  constexpr std::string_view expectedDefaultApi = "Metal 4";
#else
  constexpr BackendType expectedDefaultBackend = BackendType::vulkan;
  constexpr std::string_view expectedDefaultApi = "Vulkan";
#endif

  static_assert(demo::rhi::defaultBackend() == expectedDefaultBackend);
  assert(std::string_view(demo::rhi::toString(BackendType::vulkan)) == "vulkan");
  assert(std::string_view(demo::rhi::toString(BackendType::d3d12)) == "d3d12");
  assert(std::string_view(demo::rhi::toString(BackendType::metal)) == "metal");

  auto defaultDevice = demo::rhi::createDevice();
  auto defaultSurface = demo::rhi::createSurface();
  assert(defaultDevice != nullptr);
  assert(defaultSurface != nullptr);
  assert(defaultDevice->getBackendInfo().type == expectedDefaultBackend);
  assert(std::string_view(defaultDevice->getBackendInfo().apiName) == expectedDefaultApi);

#if defined(DEMO_HAS_VULKAN)
  assert(demo::rhi::isBackendAvailable(BackendType::vulkan));
#else
  assert(!demo::rhi::isBackendAvailable(BackendType::vulkan));
  bool rejectedVulkan = false;
  try
  {
    (void)demo::rhi::createDevice(BackendType::vulkan);
  }
  catch(const std::runtime_error&)
  {
    rejectedVulkan = true;
  }
  assert(rejectedVulkan);
#endif

#if defined(_WIN32) && defined(DEMO_HAS_D3D12)
  assert(demo::rhi::isBackendAvailable(BackendType::d3d12));
  auto d3d12Device = demo::rhi::createDevice(BackendType::d3d12);
  assert(d3d12Device != nullptr);
  assert(d3d12Device->getBackendInfo().type == BackendType::d3d12);
  assert(std::string_view(d3d12Device->getBackendInfo().apiName) == "Direct3D 12");
  assert(demo::rhi::createSurface(BackendType::d3d12) != nullptr);
#else
  assert(!demo::rhi::isBackendAvailable(BackendType::d3d12));
  bool rejectedUnavailableBackend = false;
  try
  {
    (void)demo::rhi::createDevice(BackendType::d3d12);
  }
  catch(const std::runtime_error&)
  {
    rejectedUnavailableBackend = true;
  }
  assert(rejectedUnavailableBackend);
#endif

#if defined(__APPLE__) && defined(DEMO_HAS_METAL4)
  assert(demo::rhi::isBackendAvailable(BackendType::metal));
  auto metalDevice = demo::rhi::createDevice(BackendType::metal);
  assert(metalDevice != nullptr);
  assert(metalDevice->getBackendInfo().type == BackendType::metal);
  assert(std::string_view(metalDevice->getBackendInfo().apiName) == "Metal 4");
  assert(demo::rhi::createSurface(BackendType::metal) != nullptr);
#else
  assert(!demo::rhi::isBackendAvailable(BackendType::metal));
  bool rejectedMetal = false;
  try
  {
    (void)demo::rhi::createDevice(BackendType::metal);
  }
  catch(const std::runtime_error&)
  {
    rejectedMetal = true;
  }
  assert(rejectedMetal);
#endif
  return 0;
}
