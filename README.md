# NUKE-HexTile

Nuke native C++ plugin implementing **stochastic hex-tiling** to reduce visible repetition in tileable textures.

## Overview

Based on "Practical Real-Time Hex-Tiling" by Morten S. Mikkelsen (JCGT 2022). This plugin implements hexagonal grid-based stochastic sampling with random UV offsets and rotations, plus optional height-map blending.

## Features

- **HexTile** (node `HexTile`): Color/texture tiling with luminance-weighted blending + optional height blend
- **HexTile_Normal** (node `HexTile_Normal`, C++ class `HexTileNormal`): Normal map tiling with derivative-based blending (OpenGL/DirectX toggle)
- **Height Blend**: Redshift OSL-style stochastic height blending with weight/delta controls
- **Multi-version**: Nuke 15.1 and 16.0 support (separate builds)

## Installation

### Build from Source

```bash
# Nuke 16.0
cmake -G "Visual Studio 17 2022" -A x64 -B build
cmake --build build --config Release

# Nuke 15.1
cmake -G "Visual Studio 17 2022" -A x64 -B build151 -DNuke_DIR="C:/Program Files/Nuke15.1v5/cmake"
cmake --build build151 --config Release
```

### Deployment

Set the `VMT_NUKE_DEPLOY_DIR` environment variable (or pass `-DDEPLOY_DIR=...`). The
build's POST_BUILD step copies the DLL into a per-version subfolder of that directory:
```
<VMT_NUKE_DEPLOY_DIR>/16.0/HexTile.dll   # built against Nuke 16
<VMT_NUKE_DEPLOY_DIR>/15.1/HexTile.dll   # built against Nuke 15
```
(The version subfolder is chosen automatically from the NDK include path; only `15.1`
and `16.0` are recognized.)

In a typical production tree the plugins live under a `texture/` folder with an `init.py`
beside it that loads the right build per Nuke version:
```python
import os, platform
if platform.system() == 'Windows':
    ver = "{}.{}".format(nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR)
    tex_dir = os.path.join(os.path.dirname(__file__), 'texture', ver)
    if os.path.isdir(tex_dir):              # skip cleanly if this version isn't built
        nuke.pluginAddPath('./texture/' + ver)
        try:
            nuke.load('HexTile')
        except Exception:
            pass
```
(Point `VMT_NUKE_DEPLOY_DIR` at that `texture/` folder so the build lands in
`texture/16.0/` and `texture/15.1/`.)

## Usage

Create node in Node Graph or run:
```python
nuke.createNode("HexTile")         # menu: Texture > HexTile
nuke.createNode("HexTile_Normal")  # menu: Texture > HexTile_Normal
```

### Parameters (HexTile)

| Knob (script name) | Label | Description | Default |
|------|-------|-------------|---------|
| `tile_scale` | tile scale | Hex grid density | 2.0 |
| `rot_strength` | rotation | Rotation randomness (0-1) | 0.5 |
| `tile_blend` | tile blend | Blend contrast (0.01-0.99) | 0.75 |
| `scale_output` | scale output | Scale output format | off |
| `height_weight` | height weight | Height blend influence (0-2) | 1.0 |
| `height_delta` | height delta | Height transition width (0.01-1) | 0.2 |

> `HexTile_Normal` shares `tile_scale`, `rot_strength`, `scale_output`, `height_weight`, `height_delta` (no `tile_blend`; adds a `directx` checkbox — off = OpenGL `+Y up`, on = DirectX `+Y down`). It blends via screen-space derivatives and re-normalizes, with no luminance diffusion.

### Height Input (Optional)

Connect a height map to input 1. The plugin automatically:
- Enables height blend when connected (`HexTile` only)
- Falls back to its default blend when the input is disabled/disconnected
  (luminance diffusion for `HexTile`, derivative blend for `HexTile_Normal`)
- Handles resolution mismatches gracefully

## Algorithm Details

- **TriangleGrid**: Pixel → 3 hex vertices with random offsets/rotations
- **Hash**: `frac(sin(dot)*43758.5453)` per-vertex randomness
- **Sampling**: NDK `Filter::Coefficients` for separable 2D filtering
- **Blending**:
  - No height: barycentric weight raised to a fixed exponent `g_exp = 7.0`, modulated by
    luminance diffusion (Rec.601 weights, `g_falloff = 0.6`); the `tile_blend` knob then
    applies a separate contrast gain to the blend
  - With height: soft threshold transition (Redshift OSL `soft_twin_threshold`)

Key constants: `g_exp = 7.0`, `g_falloff = 0.6`, `Lw = {0.299, 0.587, 0.114}` (Rec.601),
TriangleGrid scale `2√3`.

## Requirements

- Visual Studio 2022 (MSVC v143)
- CMake 3.10+
- Nuke 16.0v6 or 15.1v5
- Windows only

## License

MPL-2.0 (Mozilla Public License 2.0) - See [LICENSE](LICENSE)

## References

This implementation is based directly on Mikkelsen's hex-tiling method, which is itself an
adaptation of the earlier by-example tiling-and-blending work by Heitz, Neyret and Deliot.

1. Morten S. Mikkelsen. "Practical Real-Time Hex-Tiling." *Journal of Computer Graphics
   Techniques (JCGT)*, vol. 11, no. 2, pp. 77–94, 2022.
   https://jcgt.org/published/0011/03/05/ — reference implementation:
   https://github.com/mmikk/hextile-demo (MIT License)

2. Eric Heitz and Fabrice Neyret. "High-Performance By-Example Noise using a
   Histogram-Preserving Blending Operator." *Proceedings of the ACM on Computer Graphics and
   Interactive Techniques (ACM SIGGRAPH / Eurographics High-Performance Graphics)*, vol. 1,
   no. 2, art. 31, pp. 1–25, 2018. doi:10.1145/3233304

3. Thomas Deliot and Eric Heitz. "Procedural Stochastic Textures by Tiling and Blending."
   In *GPU Zen 2*, Wolfgang Engel (ed.), Black Cat Publishing, 2019.

4. Brent Burley. "On Histogram-Preserving Blending for Randomized Texture Tiling."
   *Journal of Computer Graphics Techniques (JCGT)*, vol. 8, no. 4, pp. 31–53, 2019.
   https://jcgt.org/published/0008/04/02/

## Author

Seitanmen - https://github.com/seitanmen
