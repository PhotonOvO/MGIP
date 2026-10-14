# Investigation: textured-quad-black-center

## Symptom

The textured quad should be red at the center of the frame, but captures/bug.rdc shows black.

## Capture

captures/bug.rdc

## Responsible Draw

E5: EID 11 is the TexturedQuad/Main draw on a 1280x720 R8G8B8A8_UNORM target.

```json
{
  "draw": {
    "event": 11,
    "type": "ID3D11DeviceContext::Draw()",
    "marker": "TexturedQuad/Main",
    "triangles": 2,
    "instances": 1
  },
  "pass": {
    "name": "TexturedQuad/Main",
    "beginEid": 1,
    "endEid": 11,
    "colorTargets": [
      {
        "id": 35,
        "name": "TexturedQuad/BackBuffer",
        "format": "R8G8B8A8_UNORM",
        "width": 1280,
        "height": 720
      }
    ],
    "depthTarget": null
  },
  "topology": "TriangleList",
  "viewport": {
    "eid": 11,
    "x": 0,
    "y": 0,
    "width": 1280,
    "height": 720,
    "minDepth": 0,
    "maxDepth": 1
  },
  "blend": {
    "eid": 11,
    "blends": [
      {
        "rt": 0,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 1,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 2,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 3,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 4,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 5,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 6,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 7,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      }
    ]
  }
}
```

## Evidence

### E1 — Pixel History

Pixel history identifies EID 11 (TexturedQuad/Main) as the draw that writes black at (640, 360).

Source: renderdoc

```json
{
  "eid": 11,
  "marker": "TexturedQuad/Main",
  "shaderOutput": [
    0,
    0,
    0,
    1
  ],
  "postModification": [
    0,
    0,
    0,
    1
  ],
  "passed": true,
  "flags": []
}
```

### E2 — Binding

The responsible pixel shader has the deterministic red 1x1 RGBA8 texture bound as BaseColorTexture.

Source: renderdoc

```json
{
  "stage": "Pixel",
  "type": "Image",
  "index": 0,
  "array_element": 0,
  "resource_id": 48,
  "format": "R8G8B8A8_UNORM",
  "byte_size": 0,
  "binding": "BaseColorTexture",
  "set": 0,
  "resource_name": "BaseColorTexture.Red1x1",
  "width": 1,
  "height": 1
}
```

### E3 — Constant

MaterialConstants.BaseColorFactor is [0, 0, 0, 1] at the responsible draw.

Source: renderdoc

```json
{
  "buffer": "MaterialConstants",
  "bindPoint": 0,
  "name": "BaseColorFactor",
  "type": "0",
  "rows": 1,
  "columns": 4,
  "value": [
    0,
    0,
    0,
    1
  ]
}
```

### E4 — Shader

Captured pixel-shader source multiplies the sampled texture by BaseColorFactor.

Source: renderdoc

```json
{
  "shader": 47,
  "sourceHash": "b5af67fb9ccbc5faaa2a844af6a34550ec99e51c5e328e4928a318a5adeb3c3d",
  "relevantLines": [
    "cbuffer MaterialConstants : register(b0)",
    "    VSOutput output;",
    "    output.position = float4(positions[vertexId], 0.0, 1.0);",
    "    output.uv = uvs[vertexId];",
    "    return output;",
    "    const float4 texColor = BaseColorTexture.Sample(LinearSampler, input.uv);",
    "    return texColor * BaseColorFactor;"
  ]
}
```

### E5 — Draw

EID 11 is the TexturedQuad/Main draw on a 1280x720 R8G8B8A8_UNORM target.

Source: renderdoc

```json
{
  "draw": {
    "event": 11,
    "type": "ID3D11DeviceContext::Draw()",
    "marker": "TexturedQuad/Main",
    "triangles": 2,
    "instances": 1
  },
  "pass": {
    "name": "TexturedQuad/Main",
    "beginEid": 1,
    "endEid": 11,
    "colorTargets": [
      {
        "id": 35,
        "name": "TexturedQuad/BackBuffer",
        "format": "R8G8B8A8_UNORM",
        "width": 1280,
        "height": 720
      }
    ],
    "depthTarget": null
  },
  "topology": "TriangleList",
  "viewport": {
    "eid": 11,
    "x": 0,
    "y": 0,
    "width": 1280,
    "height": 720,
    "minDepth": 0,
    "maxDepth": 1
  },
  "blend": {
    "eid": 11,
    "blends": [
      {
        "rt": 0,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 1,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 2,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 3,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 4,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 5,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 6,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      },
      {
        "rt": 7,
        "enabled": false,
        "srcColor": "One",
        "dstColor": "Zero",
        "colorOp": "Add",
        "srcAlpha": "One",
        "dstAlpha": "Zero",
        "alphaOp": "Add",
        "writeMask": 15
      }
    ]
  }
}
```

### E6 — Source Code

GPU symbol BaseColorFactor maps to the MaterialConstants initializer in the isolated demo source.

Source: source

```json
{
  "file": "examples/textured-quad/main.cpp",
  "line": 71,
  "symbol": "MaterialConstants::BaseColorFactor",
  "before": "{0.0F, 0.0F, 0.0F, 1.0F}",
  "after": "{1.0F, 1.0F, 1.0F, 1.0F}"
}
```

### E7 — Build Result

The isolated Release target rebuilt successfully after the one-line constant fix.

Source: build

```json
{
  "command": "cmake --build out/pi-renderdoc-demo --config Release",
  "exitCode": 0,
  "executable": "out/pi-renderdoc-demo/bin/Release/pi_renderdoc_textured_quad.exe",
  "executableBytes": 25088
}
```

### E8 — Verification

The post-patch capture writes red, uses an identity factor, and passes the automated center-pixel assertion.

Source: renderdoc

```json
{
  "capture": "captures/fixed.rdc",
  "eid": 11,
  "marker": "TexturedQuad/Main",
  "postModification": [
    1,
    0,
    0,
    1
  ],
  "BaseColorFactor": [
    1,
    1,
    1,
    1
  ],
  "assertion": {
    "passed": true,
    "exitCode": 0,
    "details": {
      "pass": true,
      "expected": [
        1,
        0,
        0,
        1
      ],
      "actual": [
        1,
        0,
        0,
        1
      ],
      "tolerance": 0.05,
      "eid": 11,
      "x": 640,
      "y": 360
    },
    "stderr": ""
  },
  "diff": {
    "before": "G:\\MGIF\\captures\\bug.rdc",
    "after": "G:\\MGIF\\captures\\fixed.rdc",
    "modes": {
      "draws": [
        {
          "status": "=",
          "eid_a": 11,
          "eid_b": 11,
          "marker": "TexturedQuad/Main",
          "draw_type": "Draw",
          "triangles_a": 2,
          "triangles_b": 2,
          "instances_a": 1,
          "instances_b": 1,
          "confidence": "high"
        }
      ],
      "passes": [],
      "framebuffer": {
        "identical": false,
        "diff_pixels": 518400,
        "total_pixels": 921600,
        "diff_ratio": 56.25,
        "diff_image": null,
        "eid": 11,
        "target": 0,
        "threshold": 0.01
      }
    }
  },
  "artifacts": {
    "bugBytes": 62646,
    "fixedBytes": 66372
  }
}
```

## Hypothesis

### H1

Claim: The black output is caused by BaseColorFactor being uploaded as zero RGB, which multiplies the correctly bound red texture to black.

Evidence:
- E1
- E2
- E3
- E4

Expected behavior: Changing only BaseColorFactor to [1, 1, 1, 1] preserves draw structure and changes the center output from black to red.

Verification method: Rebuild, recapture as captures/fixed.rdc, inspect EID 11, diff captures, and assert pixel (640, 360) equals [1, 0, 0, 1] within 0.05.

Confidence: High

Status: confirmed

## Source Mapping

E6: GPU symbol BaseColorFactor maps to the MaterialConstants initializer in the isolated demo source.

```json
{
  "file": "examples/textured-quad/main.cpp",
  "line": 71,
  "symbol": "MaterialConstants::BaseColorFactor",
  "before": "{0.0F, 0.0F, 0.0F, 1.0F}",
  "after": "{1.0F, 1.0F, 1.0F, 1.0F}"
}
```

## Patch

Changed only MaterialConstants.BaseColorFactor from [0, 0, 0, 1] to [1, 1, 1, 1].

Files:
- examples/textured-quad/main.cpp

## Verification

Fixed capture: captures/fixed.rdc

Result: PASS

```json
{
  "evidenceId": "E8",
  "assertion": {
    "passed": true,
    "exitCode": 0,
    "details": {
      "pass": true,
      "expected": [
        1,
        0,
        0,
        1
      ],
      "actual": [
        1,
        0,
        0,
        1
      ],
      "tolerance": 0.05,
      "eid": 11,
      "x": 640,
      "y": 360
    },
    "stderr": ""
  },
  "centerPixel": [
    1,
    0,
    0,
    1
  ],
  "BaseColorFactor": [
    1,
    1,
    1,
    1
  ],
  "diff": {
    "before": "G:\\MGIF\\captures\\bug.rdc",
    "after": "G:\\MGIF\\captures\\fixed.rdc",
    "modes": {
      "draws": [
        {
          "status": "=",
          "eid_a": 11,
          "eid_b": 11,
          "marker": "TexturedQuad/Main",
          "draw_type": "Draw",
          "triangles_a": 2,
          "triangles_b": 2,
          "instances_a": 1,
          "instances_b": 1,
          "confidence": "high"
        }
      ],
      "passes": [],
      "framebuffer": {
        "identical": false,
        "diff_pixels": 518400,
        "total_pixels": 921600,
        "diff_ratio": 56.25,
        "diff_image": null,
        "eid": 11,
        "target": 0,
        "threshold": 0.01
      }
    }
  }
}
```

## Final Result

H1: The black output is caused by BaseColorFactor being uploaded as zero RGB, which multiplies the correctly bound red texture to black. (Evidence: E1, E2, E3, E4)
