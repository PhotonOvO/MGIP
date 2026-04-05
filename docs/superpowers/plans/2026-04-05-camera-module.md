# Camera Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a first-person camera for scene preview with WASD+mouse controls.

**Architecture:** Camera class provides abstract move/rotate interfaces, App layer handles GLFW input and calls Camera methods. Camera data flows to Renderer as CameraUniforms struct (no Camera type dependency in Renderer).

**Tech Stack:** C++17, GLM for math, GLFW for input

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `render/Camera.h` | Create | Camera class declaration |
| `render/Camera.cpp` | Create | Camera class implementation |
| `render/Renderer.h` | Modify | Add `cameraUniforms` to RenderParams |
| `render/passes/GBufferPass.cpp` | Modify | Use cameraUniforms instead of hardcoded matrices |
| `app/MinimalLatestApp.h` | Modify | Add Camera member and input handling |

---

### Task 1: Create Camera.h

**Files:**
- Create: `render/Camera.h`

- [ ] **Step 1: Create Camera.h with class declaration**

```cpp
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace demo {

class Camera
{
public:
    Camera() = default;

    // Configuration
    void setPerspective(float fov, float aspectRatio, float nearPlane, float farPlane);
    void setPosition(const glm::vec3& position);
    void setYawPitch(float yaw, float pitch);

    // Input interfaces (abstract, platform-agnostic)
    void move(const glm::vec3& delta);              // Move in camera-relative space
    void rotate(float deltaYaw, float deltaPitch);  // Rotate view (degrees)

    // Per-frame update
    void update();

    // Getters for rendering
    const glm::mat4& getViewMatrix() const { return m_view; }
    const glm::mat4& getProjectionMatrix() const { return m_projection; }
    const glm::mat4& getViewProjectionMatrix() const { return m_viewProjection; }
    const glm::vec3& getPosition() const { return m_position; }

private:
    // Camera state
    glm::vec3 m_position{0.0f, 0.0f, 3.0f};
    float m_yaw{-90.0f};    // Yaw (left-right), initial facing -Z
    float m_pitch{0.0f};    // Pitch (up-down)

    // Projection parameters
    float m_fov{45.0f};
    float m_aspectRatio{16.0f / 9.0f};
    float m_nearPlane{0.1f};
    float m_farPlane{100.0f};

    // Derived vectors
    glm::vec3 m_front{0.0f, 0.0f, -1.0f};
    glm::vec3 m_up{0.0f, 1.0f, 0.0f};
    glm::vec3 m_right{1.0f, 0.0f, 0.0f};

    // Computed matrices
    glm::mat4 m_view{1.0f};
    glm::mat4 m_projection{1.0f};
    glm::mat4 m_viewProjection{1.0f};

    void updateVectors();
    void updateMatrices();
};

}  // namespace demo
```

- [ ] **Step 2: Build to verify header compiles**

Run: `cmake --build build --config Debug 2>&1 | head -20`
Expected: Build fails with unresolved external symbols (Camera methods not defined) or succeeds if header is not yet included

- [ ] **Step 3: Commit**

```bash
git add render/Camera.h
git commit -m "feat(render): add Camera class declaration

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 2: Create Camera.cpp

**Files:**
- Create: `render/Camera.cpp`

- [ ] **Step 1: Create Camera.cpp with implementation**

```cpp
#include "Camera.h"
#include <cmath>

namespace demo {

void Camera::setPerspective(float fov, float aspectRatio, float nearPlane, float farPlane)
{
    m_fov = fov;
    m_aspectRatio = aspectRatio;
    m_nearPlane = nearPlane;
    m_farPlane = farPlane;
    updateMatrices();
}

void Camera::setPosition(const glm::vec3& position)
{
    m_position = position;
}

void Camera::setYawPitch(float yaw, float pitch)
{
    m_yaw = yaw;
    m_pitch = pitch;
    updateVectors();
}

void Camera::move(const glm::vec3& delta)
{
    // Move relative to camera orientation
    m_position += m_front * delta.z;   // Forward/backward
    m_position += m_right * delta.x;   // Left/right
    m_position += m_up * delta.y;      // Up/down
}

void Camera::rotate(float deltaYaw, float deltaPitch)
{
    m_yaw += deltaYaw;
    m_pitch += deltaPitch;

    // Clamp pitch to prevent gimbal lock
    constexpr float maxPitch = 89.0f;
    m_pitch = std::clamp(m_pitch, -maxPitch, maxPitch);

    updateVectors();
}

void Camera::update()
{
    updateVectors();
    updateMatrices();
}

void Camera::updateVectors()
{
    // Calculate front vector from Euler angles
    glm::vec3 front;
    front.x = std::cos(glm::radians(m_yaw)) * std::cos(glm::radians(m_pitch));
    front.y = std::sin(glm::radians(m_pitch));
    front.z = std::sin(glm::radians(m_yaw)) * std::cos(glm::radians(m_pitch));
    m_front = glm::normalize(front);

    // Calculate right and up vectors
    m_right = glm::normalize(glm::cross(m_front, glm::vec3(0.0f, 1.0f, 0.0f)));
    m_up = glm::normalize(glm::cross(m_right, m_front));

    updateMatrices();
}

void Camera::updateMatrices()
{
    m_view = glm::lookAt(m_position, m_position + m_front, m_up);
    m_projection = glm::perspective(glm::radians(m_fov), m_aspectRatio, m_nearPlane, m_farPlane);
    m_viewProjection = m_projection * m_view;
}

}  // namespace demo
```

- [ ] **Step 2: Build to verify implementation**

Run: `cmake --build build --config Debug 2>&1 | tail -10`
Expected: Build succeeds, Camera.o generated

- [ ] **Step 3: Commit**

```bash
git add render/Camera.cpp
git commit -m "feat(render): implement Camera class methods

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 3: Add cameraUniforms to RenderParams

**Files:**
- Modify: `render/Renderer.h:41-51`

- [ ] **Step 1: Add cameraUniforms pointer to RenderParams struct**

Find the RenderParams struct (lines 41-51) and add the cameraUniforms member:

```cpp
struct RenderParams
{
  rhi::Extent2D                          viewportSize{};
  float                                  deltaTime{0.0F};
  float                                  timeSeconds{0.0F};
  MaterialHandle                         materialHandle{};
  rhi::ClearColorValue                   clearColor{0.2F, 0.2F, 0.3F, 1.0F};
  std::function<void(rhi::CommandList&)> recordUi;
  // glTF model data for rendering
  const GltfUploadResult*                gltfModel{nullptr};
  // Camera data (pointer to App-owned CameraUniforms)
  const shaderio::CameraUniforms*        cameraUniforms{nullptr};
};
```

Note: Need to add forward declaration or include for `shaderio::CameraUniforms`. The struct is defined in `shaders/shader_io.h` which is included via `common/Common.h`.

- [ ] **Step 2: Build to verify change**

Run: `cmake --build build --config Debug 2>&1 | tail -10`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add render/Renderer.h
git commit -m "feat(render): add cameraUniforms to RenderParams

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 4: Update GBufferPass to use cameraUniforms

**Files:**
- Modify: `render/passes/GBufferPass.cpp:122-128`

- [ ] **Step 1: Replace hardcoded camera with cameraUniforms from params**

Replace lines 122-128 (the hardcoded camera setup) with:

```cpp
// Setup camera matrices from params or fallback
shaderio::CameraUniforms cameraData{};
if(context.params->cameraUniforms != nullptr)
{
    cameraData = *context.params->cameraUniforms;
}
else
{
    // Fallback: default camera
    cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    cameraData.projection = glm::perspective(glm::radians(45.0f),
        static_cast<float>(extent.width) / static_cast<float>(extent.height), 0.1f, 100.0f);
    cameraData.viewProjection = cameraData.projection * cameraData.view;
    cameraData.cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
}
```

- [ ] **Step 2: Build to verify change**

Run: `cmake --build build --config Debug 2>&1 | tail -10`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add render/passes/GBufferPass.cpp
git commit -m "feat(render): use cameraUniforms from RenderParams in GBufferPass

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 5: Integrate Camera into MinimalLatestApp

**Files:**
- Modify: `app/MinimalLatestApp.h`

- [ ] **Step 1: Add include for Camera.h**

Add at the top of the file after existing includes:

```cpp
#include "../render/Camera.h"
```

- [ ] **Step 2: Add Camera members to MinimalLatestApp class**

Add these members to the private section (after `m_modelLoaded`):

```cpp
  // Camera
  demo::Camera m_camera;
  float m_moveSpeed{5.0f};       // Units per second
  float m_rotateSpeed{0.1f};     // Mouse sensitivity
  bool m_cursorCaptured{false};  // Mouse capture state
  glm::vec2 m_lastMousePos{0.0f};
  shaderio::CameraUniforms m_cameraUniforms;  // Camera data for rendering
```

- [ ] **Step 3: Initialize camera perspective in constructor**

Add after the existing constructor initialization:

```cpp
    // Initialize camera
    m_camera.setPerspective(45.0f, static_cast<float>(m_windowSize.width) / static_cast<float>(m_windowSize.height), 0.1f, 100.0f);
    m_camera.setPosition(glm::vec3(0.0f, 0.0f, 3.0f));
    m_camera.update();
```

- [ ] **Step 4: Add input handling in run() method**

Add this code at the beginning of the main loop (after `glfwPollEvents()`):

```cpp
      // Camera input handling
      {
          // Keyboard movement
          glm::vec3 moveDir{0.0f};
          if(glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) moveDir.z += 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) moveDir.z -= 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) moveDir.x -= 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) moveDir.x += 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) moveDir.y += 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) moveDir.y -= 1.0f;

          if(glm::length(moveDir) > 0.0f)
          {
              moveDir = glm::normalize(moveDir) * m_moveSpeed * ImGui::GetIO().DeltaTime;
              m_camera.move(moveDir);
          }

          // Mouse rotation (right-click to capture)
          if(glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
          {
              if(!m_cursorCaptured)
              {
                  glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                  glfwGetCursorPos(m_window, &m_lastMousePos.x, &m_lastMousePos.y);
                  m_cursorCaptured = true;
              }
              else
              {
                  double xpos, ypos;
                  glfwGetCursorPos(m_window, &xpos, &ypos);
                  float deltaX = static_cast<float>(xpos - m_lastMousePos.x) * m_rotateSpeed;
                  float deltaY = static_cast<float>(ypos - m_lastMousePos.y) * m_rotateSpeed;
                  m_lastMousePos = glm::vec2(xpos, ypos);
                  m_camera.rotate(deltaX, -deltaY);  // Inverted Y for natural feel
              }
          }
          else if(m_cursorCaptured)
          {
              glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
              m_cursorCaptured = false;
          }

          // Update camera matrices
          m_camera.update();

          // Update camera uniforms for rendering
          m_cameraUniforms.view = m_camera.getViewMatrix();
          m_cameraUniforms.projection = m_camera.getProjectionMatrix();
          m_cameraUniforms.viewProjection = m_camera.getViewProjectionMatrix();
          m_cameraUniforms.cameraPosition = m_camera.getPosition();
      }
```

- [ ] **Step 5: Update RenderParams with cameraUniforms**

Modify the RenderParams setup (around line 134) to add:

```cpp
      frameParams.cameraUniforms = &m_cameraUniforms;
```

- [ ] **Step 6: Update camera aspect ratio on resize**

In the viewport resize handling (around line 89), add camera aspect ratio update:

```cpp
      if(requestedViewportSize.width > 0 && requestedViewportSize.height > 0
         && (requestedViewportSize.width != m_viewportSize.width || requestedViewportSize.height != m_viewportSize.height))
      {
        m_viewportSize = requestedViewportSize;
        m_renderer.resize(m_viewportSize);
        m_camera.setPerspective(45.0f, static_cast<float>(m_viewportSize.width) / static_cast<float>(m_viewportSize.height), 0.1f, 100.0f);
      }
```

- [ ] **Step 7: Build to verify changes**

Run: `cmake --build build --config Debug 2>&1 | tail -20`
Expected: Build succeeds

- [ ] **Step 8: Test the camera controls**

Run: `cd build/Debug && timeout 20 ./Demo.exe 2>&1`
Test:
- WASD keys for movement
- Q/E for up/down
- Right-click + drag for rotation

- [ ] **Step 9: Commit**

```bash
git add app/MinimalLatestApp.h
git commit -m "feat(app): integrate Camera with first-person controls

- Add WASD + Q/E movement
- Add right-click mouse rotation
- Pass cameraUniforms to Renderer

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage:**
- [x] Camera class with move/rotate interfaces - Task 1 & 2
- [x] cameraUniforms in RenderParams - Task 3
- [x] GBufferPass uses cameraUniforms - Task 4
- [x] App layer input handling - Task 5
- [x] WASD + Q/E controls - Task 5
- [x] Right-click mouse rotation - Task 5

**2. Placeholder scan:**
- No TBD, TODO, or vague descriptions
- All code blocks are complete
- All commands are explicit

**3. Type consistency:**
- `shaderio::CameraUniforms` used consistently across Renderer.h and MinimalLatestApp.h
- `demo::Camera` used in MinimalLatestApp.h
- Method signatures match between declaration and implementation