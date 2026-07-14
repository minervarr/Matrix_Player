---
name: msdf-integration
description: Pattern for enabling MSDF font rendering in new windows/platforms of the Vulkan matrix player, replacing per-frame Bezier curve text rendering
source: auto-skill
extracted_at: '2026-07-08T03:57:09.623Z'
---

# MSDF Font Integration Pattern

## Context

This project has a dual font rendering architecture:
- **Bezier curve path** (default): FreeType decomposes glyph outlines → CPU emits cubic/quadratic curve records → GPU analytically rasterizes via winding-fill compute shaders every frame. Expensive for text-heavy UIs.
- **MSDF path** (opt-in): Pre-baked multi-channel signed distance field atlas → text emitted as textured quads → single graphics pipeline draw. Crisp at any size, minimal per-frame cost.

The MSDF engine (`MsdfFont` class, shaders, pipeline code) lives in `libs/firstparty/vk_canvas/first_party/vulkan_font_engine/` but is **not automatically wired** into new platform renderers. Each window/platform must integrate it explicitly.

## When to Use

When adding a new window, platform port, or when text rendering performance is a bottleneck (scrolling, animations, high-frequency UI updates).

## Integration Steps

### 1. Shader Availability

MSDF shaders (`msdf_vert.slang`, `msdf_frag.slang`) must exist in `libs/firstparty/vk_canvas/shaders_src/` and be listed in the platform's `CMakeLists.txt` `vce_compile_slang()` call:

```cmake
vce_compile_slang(compile_vk_canvas_shaders
    ${CMAKE_BINARY_DIR}/assets/shaders
    ${CMAKE_SOURCE_DIR}/libs/firstparty/vk_canvas/shaders_src
    # ... other shaders ...
    msdf_vert
    msdf_frag
)
```

If shaders don't exist in `shaders_src/`, copy them from the font engine's Android app: `vulkan_font_engine/app/src/main/shaders_src/msdf_{vert,frag}.slang`.

### 2. Renderer MSDF Pipeline

The platform's `Renderer` class needs MSDF Vulkan resources. Add these members:

```cpp
// Atlas texture
VkImage msdfAtlasImage_; VkDeviceMemory msdfAtlasMemory_;
VkImageView msdfAtlasView_; VkSampler msdfAtlasSampler_;
uint32_t msdfAtlasW_, msdfAtlasH_; float msdfPxRange_;

// Vertex buffer (host-visible, persistently mapped)
VkBuffer msdfVbo_; VkDeviceMemory msdfVboMemory_; void* msdfVboMapped_;
uint32_t msdfVertCount_;

// Pipeline
VkDescriptorSetLayout msdfSetLayout_; VkDescriptorPool msdfDescPool_;
VkDescriptorSet msdfDescSet_; VkPipelineLayout msdfPipelineLayout_;
VkPipeline msdfPipeline_;
```

Methods to implement:
- `initMsdf(const MsdfFont& font)` — create all resources, upload atlas, create pipeline
- `uploadMsdfAtlas(rgba, w, h, pxRange)` — staging buffer → image → transition barriers → view + sampler + descriptor
- `createMsdfPipeline()` — load `shaders/msdf_vert.spv` + `msdf_frag.spv`, create graphics pipeline with alpha blend, push constants for `screen.xy, pxRange, pad, atlas.xy, scroll.xy`
- `recordMsdfDraw(cmd)` — bind pipeline, push constants, bind descriptor + VBO, `vkCmdDraw`
- `cleanupMsdf()` — destroy all resources

Modify `draw()` to accept `const std::vector<float>& msdfQuads = {}`, memcpy quads to VBO each frame, call `recordMsdfDraw` inside the render pass after overlay composite.

### 3. Application Window Wiring

In the window class (header + cpp):

```cpp
// Header: add members
MsdfFont msdfFont_;
std::vector<float> msdfQuads_;
```

At init time, generate the MSDF atlas from the OTF font with disk caching.

**CRITICAL: The `AssetLoader` lambda must read files by absolute path directly (via `fopen`), NOT through `FileAssetReader`**, because `FileAssetReader` prepends `<exe_dir>/assets/` to all paths, corrupting absolute font paths:

```cpp
AssetLoader loader = [](const char* path) -> std::vector<uint8_t> {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return buf;
};
if (msdfFont_.generate(loader, fontPath.c_str(), cachePath.c_str())) {
    renderer_->initMsdf(msdfFont_);
}
```

Cache path should be next to the font file (e.g. `fonts/lmsans10-regular.msdf.cache`). First run generates the atlas via msdfgen (~1-2 seconds), subsequent runs load from cache instantly.

## MSDF Quality Parameters

The runtime `MsdfFont::generate()` in `msdf.cc` has hardcoded quality parameters that must match or approach the offline baker's quality:

| Parameter | Low quality (old default) | Production quality | Effect |
|---|---|---|---|
| `sizePxEm_` | 64 | **100** | Texels per EM in atlas — higher = crisper magnified text |
| `distanceRange_` | 4 | **8** | SDF margin pixels — higher = better corner resolution |
| Atlas width (`AW`) | 1024 | **2048** | Wider sheet fits larger cells without wrapping |

The offline baker (`atlas_gen`) uses EM=100, RANGE=10, AW=3072. For runtime generation, EM=100/RANGE=8/AW=2048 is a good balance of quality vs atlas size.

**Cache invalidation:** The cache format has a version number (in `loadCache`/`saveCache`). Bump it when changing generation parameters so stale low-quality caches are automatically regenerated.

In `drawFrame()`:

```cpp
msdfQuads_.clear();
Canvas canvas(frameCurves_, renderer_->width(), renderer_->height(), &uiFont_, ...);
if (msdfFont_.valid())
    canvas.useMsdf(&msdfFont_, &msdfQuads_);
// ... draw UI ...
renderer_->draw(frameCurves_, 0, frameImages_, frameImagesFg_, msdfQuads_);
```

### 4. Include Paths

`msdf.hh` is PUBLIC-exposed by `vk_font_core` (linked via `vk_canvas_core`). Add `#include "msdf.hh"` to any header that needs `MsdfFont`. The `AssetLoader` type is `std::function<std::vector<uint8_t>(const char*)>` defined in `vulkan_font_engine/core/asset_loader.hh` (included transitively by `msdf.hh`).

## Key Gotchas

- **`AssetLoader` must NOT wrap `FileAssetReader::read()`** for absolute paths. `FileAssetReader` prepends `<exe_dir>/assets/` to every path — passing an absolute font path like `C:\...\fonts\foo.otf` produces an invalid combined path. Use a direct `fopen`-based lambda instead. This is the #1 reason MSDF silently fails to generate (returns false, falls back to Bezier curves with no visible error).
- **`MsdfFont::generate()` uses a different `AssetLoader` type** than the platform's `AssetReader`. `AssetLoader` is `std::function<std::vector<uint8_t>(const char*)>` defined in `asset_loader.hh`.
- **Atlas upload uses a one-shot command buffer** submitted to the graphics queue with `vkQueueWaitIdle`. Must happen after the Renderer's device/queue/swapchain are created.
- **Pipeline front face is `COUNTER_CLOCKWISE`** (not clockwise like the overlay) — the MSDF glyph quads use CCW winding. `cullMode = VK_CULL_MODE_NONE` so it doesn't matter, but the convention differs.
- **The FreeType `Font` is still needed** as a fallback and for `Font::load()` — keep both `Font uiFont_` and `MsdfFont msdfFont_`.
- **Build requires VS Developer environment** — use `build.bat` which calls `vcvars64.bat`, not raw `cmake --build`.
- **Cache version must be bumped** when changing generation parameters (`sizePxEm_`, `distanceRange_`, `AW`), otherwise stale low-quality caches load indefinitely.
- **Default runtime parameters (EM=64, RANGE=4) produce noticeably low-quality text** at display sizes. Match the offline baker's EM=100 for production quality.
