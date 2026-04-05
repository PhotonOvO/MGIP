# Camera Module Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a first-person camera for scene preview with WASD+mouse controls.

**Architecture:** Camera class provides abstract move/rotate interfaces, App layer handles GLFW input and calls Camera methods. Camera data flows to Renderer as CameraUniforms struct (no Camera type dependency in Renderer).

**Tech Stack:** C++17, GLM for math, GLFW for input

---

## 1. Overview

```
┌─────────────────────────────────────────────────────────┐
│                    MinimalLatestApp                      │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐  │
│  │ GLFW Input  │───▶│   Camera    │───▶│CameraUniforms│ │
│  │ (WASD+Mouse)│    │             │    │             │  │
│  └─────────────┘    └─────────────┘    └──────┬──────┘  │
└──────────────────────────────────────────────┼──────────┘
                                               │
                                               ▼
┌─────────────────────────────────────────────────────────┐
│                      Renderer                            │
│  RenderParams.cameraUniforms ──▶ PassContext            │
│                                        │                 │
│                                        ▼                 │
│                                   GBufferPass            │
│                                  (writes to UBO)         │
└─────────────────────────────────────────────────────────┘
```

**Key Principle:** Renderer knows `CameraUniforms` struct only, not `Camera` class. This keeps rendering layer decoupled from camera implementation.

---

## 2. Camera Class

### File: `render/Camera.h`

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
    void move(const glm::vec3& delta);              // Move in world space
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

### File: `render/Camera.cpp`

```cpp
#include "Camera.h"
#include <glm/gtc/quaternion.hpp>
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

---

## 3. App Layer Integration

### Changes to `app/MinimalLatestApp.h`

**New members:**

```cpp
// Camera
demo::Camera m_camera;
float m_moveSpeed{5.0f};       // Units per second
float m_rotateSpeed{0.1f};     // Mouse sensitivity
bool m_cursorCaptured{false};  // Mouse capture state
glm::vec2 m_lastMousePos{0.0f};
```

**Input handling in `run()` loop:**

```cpp
// Handle keyboard movement
glm::vec3 moveDir{0.0f};
if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) moveDir.z += 1.0f;
if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) moveDir.z -= 1.0f;
if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) moveDir.x -= 1.0f;
if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) moveDir.x += 1.0f;
if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) moveDir.y += 1.0f;
if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) moveDir.y -= 1.0f;

if (glm::length(moveDir) > 0.0f)
{
    moveDir = glm::normalize(moveDir) * m_moveSpeed * deltaTime;
    m_camera.move(moveDir);
}

// Handle mouse rotation (right-click to capture)
if (glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
{
    if (!m_cursorCaptured)
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
else if (m_cursorCaptured)
{
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    m_cursorCaptured = false;
}

// Update camera matrices
m_camera.update();
```

---

## 4. Renderer Integration

### Changes to `render/Renderer.h`

**RenderParams struct update:**

```cpp
struct RenderParams
{
    rhi::Extent2D viewportSize{};
    float deltaTime{0.0f};
    float timeSeconds{0.0f};
    MaterialHandle materialHandle{};
    rhi::ClearColorValue clearColor{0.2f, 0.2f, 0.3f, 1.0f};
    std::function<void(rhi::CommandList&)> recordUi;
    const GltfUploadResult* gltfModel{nullptr};

    // Camera data (pointer to App-owned CameraUniforms)
    const shaderio::CameraUniforms* cameraUniforms{nullptr};
};
```

### Changes to `render/passes/GBufferPass.cpp`

Replace hardcoded camera matrices:

```cpp
// In GBufferPass::execute()
// OLD: Hardcoded camera
// cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), ...);

// NEW: Use camera from params
shaderio::CameraUniforms cameraData{};
if (context.params->cameraUniforms != nullptr)
{
    cameraData = *context.params->cameraUniforms;
}
else
{
    // Fallback: default camera
    cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f));
    cameraData.projection = glm::perspective(glm::radians(45.0f),
        static_cast<float>(extent.width) / static_cast<float>(extent.height), 0.1f, 100.0f);
    cameraData.viewProjection = cameraData.projection * cameraData.view;
    cameraData.cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
}
```

---

## 5. Controls Summary

| Key | Action |
|-----|--------|
| **W** | Move forward |
| **S** | Move backward |
| **A** | Move left (strafe) |
| **D** | Move right (strafe) |
| **Q** | Move up |
| **E** | Move down |
| **Right-click + drag** | Rotate camera view |

---

## 6. File Changes Summary

| File | Action | Description |
|------|--------|-------------|
| `render/Camera.h` | Create | Camera class declaration |
| `render/Camera.cpp` | Create | Camera class implementation |
| `render/Renderer.h` | Modify | Add `cameraUniforms` to RenderParams |
| `render/passes/GBufferPass.cpp` | Modify | Use cameraUniforms instead of hardcoded matrices |
| `app/MinimalLatestApp.h` | Modify | Add Camera member and input handling |

---

## 7. Future Extensions (Out of Scope)

- **Mobile platform**: Implement input adapter calling `move()` / `rotate()`
- **Reflection cameras**: Camera subclass with different update logic
- **Camera manager**: Multiple cameras with transitions
- **Frustum culling**: Derive frustum planes from viewProjection matrix