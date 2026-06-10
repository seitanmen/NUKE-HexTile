# NUKE-HexTile

Nuke native C++ plugin implementing **stochastic hex-tiling** to reduce visible repetition in tileable textures.

## Overview

Based on "Practical Real-Time Hex-Tiling" by Morten S. Mikkelsen (JCGT 2022). This plugin implements hexagonal grid-based stochastic sampling with random UV offsets and rotations, plus optional height-map blending.

## Features

- **HexTile** (`vmt_HexTile`): Color/texture tiling with luminance-weighted blending + optional height blend
- **HexTile_Normal** (`vmt_HexTile_Normal`): Normal map tiling with derivative-based blending (OpenGL/DirectX toggle)
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
nuke.createNode("HexTile")  # vmt_HexTile
nuke.createNode("HexTile_Normal")  # vmt_HexTile_Normal
```

### Parameters (HexTile)

| Knob | Description | Default |
|------|-------------|---------|
| `tileScale` | Hex grid density | 1.0 |
| `rotStrength` | Rotation randomness | 0.5 |
| `tile_blend` | Barycentric weight exponent | 0.5 |
| `scale_output` | Scale output format | disabled |
| `height_weight` | Height blend influence | 1.0 |
| `height_delta` | Height transition width | 0.2 |

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

## Citation

This implementation is based on:

```
Morten S. Mikkelsen, "Practical Real-Time Hex-Tiling",
Journal of Computer Graphics Techniques, vol. 11, no. 2, pp. 77-94, 2022.
```

Paper: https://jcgt.org/published/0011/03/05/

Reference implementation: https://github.com/mmikk/hextile-demo (MIT License)

## Author

Seitanmen - https://github.com/seitanmen
