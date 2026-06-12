# NUKE-HexTile

Nuke native C++ plugin implementing **stochastic hex-tiling** to reduce visible repetition in tileable textures.

## Overview

Based on "Practical Real-Time Hex-Tiling" by Morten S. Mikkelsen (JCGT 2022). This plugin implements hexagonal grid-based stochastic sampling with random UV offsets and rotations, plus optional height-map blending.

## Features

- **HexTile** (class `HexTile`): Color/texture tiling with luminance-weighted blending + optional height blend
- **HexTile_Normal** (class `HexTile_Normal`): Normal map tiling with derivative-based blending (OpenGL/DirectX toggle)
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

Set `VMT_NUKE_DEPLOY_DIR` environment variable. Build auto-deploys to:
```
<DEPLOY_DIR>/texture/16.0/HexTile.dll
<DEPLOY_DIR>/texture/15.1/HexTile.dll
```

Add to `init.py`:
```python
import os
if platform.system() == 'Windows':
    ver = "{}.{}".format(nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR)
    tex_dir = os.path.join(os.path.dirname(__file__), 'texture', ver)
    if os.path.isdir(tex_dir):
        nuke.pluginAddPath('./texture/' + ver)
        try:
            nuke.load('HexTile')
        except Exception:
            pass
```

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

> `HexTile_Normal` shares `tile_scale`, `rot_strength`, `scale_output`, `height_weight`, `height_delta` (no `tile_blend`; adds a `normalConvention` OpenGL/DirectX toggle).

### Height Input (Optional)

Connect height map to input 1. Plugin automatically:
- Enables height blend when connected
- Falls back to luminance blend when disabled/disconnected
- Handles resolution mismatches gracefully

## Algorithm Details

- **TriangleGrid**: Pixel → 3 hex vertices with random offsets/rotations
- **Hash**: `frac(sin(dot*big)*43758)` per-vertex randomness
- **Sampling**: NDK Filter::Coefficients for separable 2D filtering
- **Blending**: 
  - No height: Barycentric weight^γ × luminance diffusion (Rec.601)
  - With height: Soft threshold transition (Redshift OSL `soft_twin_threshold`)

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
