cmake_minimum_required(VERSION 3.24)

project(PiRenderDocTexturedQuad LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "The Pi RenderDoc V0.1 demo requires Win32 and D3D11.")
endif()

add_executable(pi_renderdoc_textured_quad WIN32 main.cpp)
target_compile_features(pi_renderdoc_textured_quad PRIVATE cxx_std_20)
target_compile_definitions(pi_renderdoc_textured_quad PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
target_link_libraries(pi_renderdoc_textured_quad PRIVATE
  d3d11
  dxgi
  d3dcompiler
  dxguid
  user32
)

if(MSVC)
  target_compile_options(pi_renderdoc_textured_quad PRIVATE /W4 /permissive- /EHsc)
endif()

set_target_properties(pi_renderdoc_textured_quad PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
