#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kFramesToRender = 120;
constexpr wchar_t kWindowClass[] = L"PiRenderDocTexturedQuadWindow";

constexpr char kShaderSource[] = R"hlsl(
Texture2D BaseColorTexture : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer MaterialConstants : register(b0)
{
    float4 BaseColorFactor;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    static const float2 positions[6] =
    {
        float2(-0.75, -0.75),
        float2(-0.75,  0.75),
        float2( 0.75,  0.75),
        float2(-0.75, -0.75),
        float2( 0.75,  0.75),
        float2( 0.75, -0.75)
    };
    static const float2 uvs[6] =
    {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 0.0),
        float2(1.0, 1.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    const float4 texColor = BaseColorTexture.Sample(LinearSampler, input.uv);
    return texColor * BaseColorFactor;
}
)hlsl";

struct alignas(16) MaterialConstants {
  std::array<float, 4> BaseColorFactor = {1.0F, 1.0F, 1.0F, 1.0F};
};

void throwIfFailed(HRESULT result, const char* operation) {
  if (SUCCEEDED(result)) {
    return;
  }
  throw std::runtime_error(
      std::string(operation) + " failed with HRESULT "
      + std::to_string(static_cast<std::uint32_t>(result)));
}

void setDebugName(ID3D11DeviceChild* object, const char* name) {
  if (object == nullptr) {
    return;
  }
  object->SetPrivateData(
      WKPDID_D3DDebugObjectName,
      static_cast<UINT>(std::strlen(name)),
      name);
}

ComPtr<ID3DBlob> compileShader(const char* entryPoint, const char* target) {
  constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS
      | D3DCOMPILE_DEBUG
      | D3DCOMPILE_SKIP_OPTIMIZATION;
  ComPtr<ID3DBlob> bytecode;
  ComPtr<ID3DBlob> errors;
  const HRESULT result = D3DCompile(
      kShaderSource,
      sizeof(kShaderSource) - 1,
      "textured_quad.hlsl",
      nullptr,
      nullptr,
      entryPoint,
      target,
      flags,
      0,
      &bytecode,
      &errors);
  if (FAILED(result)) {
    const char* message = errors
        ? static_cast<const char*>(errors->GetBufferPointer())
        : "D3DCompile failed without diagnostics";
    throw std::runtime_error(message);
  }
  return bytecode;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wParam, lParam);
  }
}

class TexturedQuadDemo {
 public:
  explicit TexturedQuadDemo(HWND window) {
    createDeviceAndSwapChain(window);
    createRenderTarget();
    createShaders();
    createTexture();
    createMaterialConstants();
    createSampler();
    context_.As(&annotation_);
  }

  void render() {
    constexpr std::array<float, 4> clearColor = {0.08F, 0.10F, 0.12F, 1.0F};
    context_->ClearRenderTargetView(renderTarget_.Get(), clearColor.data());

    ID3D11RenderTargetView* renderTargets[] = {renderTarget_.Get()};
    context_->OMSetRenderTargets(1, renderTargets, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(kWidth);
    viewport.Height = static_cast<float>(kHeight);
    viewport.MaxDepth = 1.0F;
    context_->RSSetViewports(1, &viewport);

    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);

    ID3D11ShaderResourceView* resources[] = {baseColorView_.Get()};
    context_->PSSetShaderResources(0, 1, resources);
    ID3D11SamplerState* samplers[] = {linearSampler_.Get()};
    context_->PSSetSamplers(0, 1, samplers);
    ID3D11Buffer* constants[] = {materialConstants_.Get()};
    context_->PSSetConstantBuffers(0, 1, constants);

    if (annotation_) {
      annotation_->BeginEvent(L"TexturedQuad/Main");
    }
    context_->Draw(6, 0);
    if (annotation_) {
      annotation_->EndEvent();
    }

    throwIfFailed(swapChain_->Present(1, 0), "IDXGISwapChain::Present");
  }

 private:
  void createDeviceAndSwapChain(HWND window) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = kWidth;
    description.BufferDesc.Height = kHeight;
    description.BufferDesc.RefreshRate.Numerator = 60;
    description.BufferDesc.RefreshRate.Denominator = 1;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL requestedLevel = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL selectedLevel{};
    throwIfFailed(
        D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &requestedLevel,
            1,
            D3D11_SDK_VERSION,
            &description,
            &swapChain_,
            &device_,
            &selectedLevel,
            &context_),
        "D3D11CreateDeviceAndSwapChain");
    if (selectedLevel != D3D_FEATURE_LEVEL_11_0) {
      throw std::runtime_error("D3D feature level 11.0 is required");
    }
  }

  void createRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    throwIfFailed(
        swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)),
        "IDXGISwapChain::GetBuffer");
    setDebugName(backBuffer.Get(), "TexturedQuad/BackBuffer");
    throwIfFailed(
        device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_),
        "ID3D11Device::CreateRenderTargetView");
    setDebugName(renderTarget_.Get(), "TexturedQuad/ColorTarget");
  }

  void createShaders() {
    const ComPtr<ID3DBlob> vertexBytecode = compileShader("VSMain", "vs_5_0");
    throwIfFailed(
        device_->CreateVertexShader(
            vertexBytecode->GetBufferPointer(),
            vertexBytecode->GetBufferSize(),
            nullptr,
            &vertexShader_),
        "ID3D11Device::CreateVertexShader");
    setDebugName(vertexShader_.Get(), "TexturedQuad/VSMain");

    const ComPtr<ID3DBlob> pixelBytecode = compileShader("PSMain", "ps_5_0");
    throwIfFailed(
        device_->CreatePixelShader(
            pixelBytecode->GetBufferPointer(),
            pixelBytecode->GetBufferSize(),
            nullptr,
            &pixelShader_),
        "ID3D11Device::CreatePixelShader");
    setDebugName(pixelShader_.Get(), "TexturedQuad/PSMain");
  }

  void createTexture() {
    constexpr std::array<std::uint8_t, 4> redPixel = {255, 0, 0, 255};
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1;
    description.Height = 1;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = redPixel.data();
    initialData.SysMemPitch = static_cast<UINT>(redPixel.size());
    throwIfFailed(
        device_->CreateTexture2D(&description, &initialData, &baseColorTexture_),
        "ID3D11Device::CreateTexture2D");
    setDebugName(baseColorTexture_.Get(), "BaseColorTexture.Red1x1");
    throwIfFailed(
        device_->CreateShaderResourceView(baseColorTexture_.Get(), nullptr, &baseColorView_),
        "ID3D11Device::CreateShaderResourceView");
    setDebugName(baseColorView_.Get(), "BaseColorTexture.SRV");
  }

  void createMaterialConstants() {
    const MaterialConstants constants{};
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(constants);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = &constants;
    throwIfFailed(
        device_->CreateBuffer(&description, &initialData, &materialConstants_),
        "ID3D11Device::CreateBuffer");
    setDebugName(materialConstants_.Get(), "MaterialConstants");
  }

  void createSampler() {
    D3D11_SAMPLER_DESC description{};
    description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.MaxLOD = D3D11_FLOAT32_MAX;
    throwIfFailed(
        device_->CreateSamplerState(&description, &linearSampler_),
        "ID3D11Device::CreateSamplerState");
    setDebugName(linearSampler_.Get(), "LinearSampler");
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain> swapChain_;
  ComPtr<ID3D11RenderTargetView> renderTarget_;
  ComPtr<ID3D11VertexShader> vertexShader_;
  ComPtr<ID3D11PixelShader> pixelShader_;
  ComPtr<ID3D11Texture2D> baseColorTexture_;
  ComPtr<ID3D11ShaderResourceView> baseColorView_;
  ComPtr<ID3D11Buffer> materialConstants_;
  ComPtr<ID3D11SamplerState> linearSampler_;
  ComPtr<ID3DUserDefinedAnnotation> annotation_;
};

HWND createWindow(HINSTANCE instance) {
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = windowProc;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.lpszClassName = kWindowClass;
  if (RegisterClassExW(&windowClass) == 0) {
    throw std::runtime_error("RegisterClassExW failed");
  }

  RECT windowRect{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
  throwIfFailed(
      AdjustWindowRectEx(&windowRect, WS_OVERLAPPEDWINDOW, FALSE, 0) ? S_OK : E_FAIL,
      "AdjustWindowRectEx");
  HWND window = CreateWindowExW(
      0,
      kWindowClass,
      L"Pi + RenderDoc V0.1 Textured Quad",
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      windowRect.right - windowRect.left,
      windowRect.bottom - windowRect.top,
      nullptr,
      nullptr,
      instance,
      nullptr);
  if (window == nullptr) {
    throw std::runtime_error("CreateWindowExW failed");
  }
  return window;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  SetProcessDPIAware();
  HWND window = nullptr;
  try {
    window = createWindow(instance);
    TexturedQuadDemo demo(window);
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    bool running = true;
    for (UINT frame = 0; frame < kFramesToRender && running; ++frame) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
          running = false;
          break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (running) {
        demo.render();
      }
    }

    if (IsWindow(window)) {
      DestroyWindow(window);
    }
    UnregisterClassW(kWindowClass, instance);
    return 0;
  } catch (const std::exception& error) {
    std::string message = std::string("Pi RenderDoc demo failed: ") + error.what() + "\n";
    OutputDebugStringA(message.c_str());
    if (window != nullptr && IsWindow(window)) {
      DestroyWindow(window);
    }
    UnregisterClassW(kWindowClass, instance);
    return 1;
  }
}
