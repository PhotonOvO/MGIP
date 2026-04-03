# Current Demo RHI Scope

This refactor exits with a thin RHI whose shipping reference path is Vulkan.
It remains intentionally constrained to what the current `Demo` runtime actually exercises.

## Final Backend Status

- Vulkan is the only implemented and smoke-gated backend.
- Metal 3+: scaffold-only, compile/design-time checked, no runtime implementation claimed.
- D3D12: scaffold-only, compile/design-time checked, no runtime implementation claimed.
- `.sisyphus/scripts/final-verification-matrix.ps1` is the final readiness gate for this scope.

## In Scope

- frame acquire / submit / present for the Vulkan reference backend
- frame timeline / fence style synchronization
- command list recording for current compute + graphics work
- render-pass begin/end for current dynamic-rendering flow
- buffer and texture handles used by the current passes
- pipeline binding for current graphics and compute pipelines
- bind-table / descriptor binding for:
  - material resources
  - draw-dynamic resources
- viewport / scissor / push-constants / vertex-buffer binding
- buffer barriers and texture transitions required by current passes
- swapchain rebuild support for current window-resize flow
- portable capability negotiation for:
  - Core
  - ExtensionAsyncCompute
  - ExtensionMeshShader
  - ExtensionRayTracing
- public-header purity and shader-authority verification as build gates
- Metal and D3D12 interface scaffolds sufficient for compile-time/design-time validation

## Backend Readiness Summary

| Backend | Readiness | Meaning |
|---------|-----------|---------|
| Vulkan | Implemented | Build + smoke gated reference backend for current Demo scope |
| Metal 3+ | Scaffolded | Interface/docs/compile-only proof, runtime bring-up deferred |
| D3D12 | Scaffolded | Interface/docs/compile-only proof, runtime bring-up deferred |

## Out of Scope For This Phase

- Metal full backend implementation
- D3D12 full backend implementation
- runtime parity claims for Metal or D3D12
- generalized future-facing capability matrix beyond the frozen portable tiers
- async compute scheduling in portable v1
- ray tracing portability in portable v1
- mesh shader portability in portable v1
- sparse resources
- render graph abstraction
- broader GPU-driven expansion beyond what current Demo already uses

## Known Limitations

- Smoke coverage proves startup/bring-up/resize only; it does not certify long-running runtime stability.
- Historical runtime stability issues, including access-violation reports outside the smoke horizon, remain explicitly open.
- Metal and D3D12 verification is compile/design-time only and does not imply feature parity or presentability.

## Practical Rule

If the current Demo does not execute it today, this phase does not add it to RHI.

The goal is not a complete future-proof abstraction.
The goal is a thin execution layer that removes native Vulkan hot-path usage from `Renderer` while preserving current behavior.

## Legacy Internal Modules

- `Demo/gfx/**` is now legacy/internal-only carryover code.
- `Demo/render/**` and public `Demo/rhi/**` headers must not include or depend on `Demo/gfx/**`.
- `demo_core` no longer exports Demo source-root include paths that expose `Demo/gfx/**` to renderer/public consumers.
- `.sisyphus/scripts/check-include-law.ps1` enforces this boundary during the Demo build.
- `Demo/gfx/**` remains scheduled for removal in a future refactor once any backend-private residue is fully retired.
