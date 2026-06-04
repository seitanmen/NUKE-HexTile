/*
 * HexTile — Nuke plugin for stochastic hex-tiling
 *
 * Based on "Practical Real-Time Hex-Tiling" by Morten S. Mikkelsen
 * Journal of Computer Graphics Techniques (JCGT), vol. 11, no. 2, 77-94, 2022
 * http://jcgt.org/published/0011/03/05/
 *
 * Reduces visible repetition in tileable textures by using a stochastic
 * hexagonal tiling with random offsets, rotations, and luminance-based
 * weight diffusion with contrast restoration.
 *
 * Build: compile as shared library (.dll/.so/.dylib), place in Nuke plugin path.
 */

#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Tile.h"

#include <cmath>
#include <algorithm>

using namespace DD::Image;

static const char* const CLASS = "HexTile";
static const char* const HELP =
    "Stochastic hex-tiling based on Mikkelsen (JCGT 2022). Reduces visible tiling "
    "patterns by randomizing UV offsets and rotations per hex tile, then blending "
    "three samples with luminance-driven diffusion weights.\n\n"
    "Feed a seamlessly tileable texture into the input. Adjust tile_scale to control "
    "repetition density, rot_strength for random rotation per tile, and contrast to "
    "restore lost contrast in the blending.\n\n"
    "Reference: http://jcgt.org/published/0011/03/05/";


// ---------------------------------------------------------------------------
// Hex-tiling math helpers (ported from HLSL reference implementation)
// ---------------------------------------------------------------------------

static inline float fracf(float x)
{
    return x - floorf(x);
}

/** Deterministic 2D hash in [0,1)^2 for a grid vertex. */
static void hexHash(float px, float py, float& hx, float& hy)
{
    // Matrix from the reference: [[127.1, 311.7], [269.5, 183.3]]
    float rx = px * 127.1f + py * 311.7f;
    float ry = px * 269.5f + py * 183.3f;
    hx = fracf(sinf(rx) * 43758.5453f);
    hy = fracf(sinf(ry) * 43758.5453f);
}

/**
 * Partition UV space into a simplex (triangle) grid.
 * Returns barycentric weights w1,w2,w3 and grid vertex positions.
 *
 * Ported from Listing 1 of the paper.
 */
static void TriangleGrid(float stx, float sty,
                         float& w1, float& w2, float& w3,
                         float& v1x, float& v1y,
                         float& v2x, float& v2y,
                         float& v3x, float& v3y)
{
    // Scaling of the input
    const float scale = 2.0f * sqrtf(3.0f);
    float sx = stx * scale;
    float sy = sty * scale;

    // Skew input space into simplex triangle grid
    // gridToSkewedGrid = [[1.0, -1/sqrt(3)], [0.0, 2/sqrt(3)]]
    const float invSqrt3 = 0.5773502691896258f;  // 1/sqrt(3)
    const float twoInvSqrt3 = 1.1547005383792516f; // 2/sqrt(3)
    float skx = sx - invSqrt3 * sy;
    float sky = twoInvSqrt3 * sy;

    float bx = floorf(skx);
    float by = floorf(sky);

    float fx = skx - bx;
    float fy = sky - by;
    float fz = 1.0f - fx - fy;

    int s  = (fz < 0.0f) ? 1 : 0;
    float s2 = 2.0f * (float)s - 1.0f;

    w1 = -fz * s2;
    w2 = (float)s - fy * s2;
    w3 = (float)s - fx * s2;

    float sv = (float)s;
    v1x = bx + sv;         v1y = by + sv;
    v2x = bx + sv;         v2y = by + 1.0f - sv;
    v3x = bx + 1.0f - sv;  v3y = by + sv;
}

/**
 * Convert grid vertex integer coordinates back to tile-UV center position.
 * Inverse of the skew transform, divided by the grid scale.
 *
 * Ported from Listing 5 of the paper.
 */
static void MakeCenST(float vx, float vy, float& cx, float& cy)
{
    // invSkewMat = [[1.0, 0.5], [0.0, 1/(2/sqrt(3))]]
    //            = [[1.0, 0.5], [0.0, sqrt(3)/2]]
    const float invDenom = 1.0f / (2.0f * sqrtf(3.0f));
    const float sqrt3Half = 0.8660254037844386f; // sqrt(3)/2
    cx = (vx + 0.5f * vy) * invDenom;
    cy = (sqrt3Half * vy) * invDenom;
}

/**
 * Deterministic rotation angle for a grid vertex.
 * Returns cosine and sine of the rotation angle scaled by rotStrength.
 *
 * Ported from Listing 6 of the paper.
 */
static void LoadRot2x2(float idx_x, float idx_y, float rotStrength,
                       float& cosa, float& sina)
{
    const float PI  = 3.1415926535897932f;
    const float TAU = 6.2831853071795865f;

    float angle = fabsf(idx_x * idx_y) + fabsf(idx_x + idx_y) + PI;

    // remap to +/-pi
    angle = fmodf(angle, TAU);
    if (angle < 0.0f) angle += TAU;
    if (angle > PI)   angle -= TAU;

    angle *= rotStrength;

    cosa = cosf(angle);
    sina = sinf(angle);
}

/**
 * S-curve gain function (applied per component, then normalized).
 * Increases contrast when r > 0.5, reduces when r < 0.5.
 *
 * Ported from Listing 8 of the paper.
 */
static float gainScalar(float x, float k)
{
    if (x < 0.5f) {
        return 0.5f * powf(2.0f * x, k);
    } else {
        return 1.0f - 0.5f * powf(2.0f * (1.0f - x), k);
    }
}

/**
 * Bilinear sample from a Nuke Tile with UV wrapping.
 * u, v are in "tile units" — frac(u) maps to [0,1) within one texture repeat.
 */
static float sampleBilinear(const Tile& tile, Channel z,
                             float u, float v,
                             int w, int h,
                             int offX, int offY)
{
    u = fracf(u);
    v = fracf(v);

    float px = u * (float)w;
    float py = v * (float)h;

    int x0 = (int)floorf(px);
    int y0 = (int)floorf(py);
    float fx = px - (float)x0;
    float fy = py - (float)y0;

    // Wrap to [0, w) and [0, h)
    x0 = x0 % w;  if (x0 < 0) x0 += w;
    y0 = y0 % h;  if (y0 < 0) y0 += h;
    int x1 = (x0 + 1) % w;
    int y1 = (y0 + 1) % h;

    // Absolute coordinates for Tile access
    int ax0 = x0 + offX, ax1 = x1 + offX;
    int ay0 = y0 + offY, ay1 = y1 + offY;

    // tile[z] returns LinePointers object (not null-checkable).
    // tile[z][y] returns const float* (RowPtr) which is null if channel missing.
    const float* row_lo = tile[z][ay0];
    if (!row_lo) return 0.0f;
    const float* row_hi = tile[z][ay1];

    float c00 = row_lo[ax0];
    float c10 = row_lo[ax1];
    float c01 = row_hi ? row_hi[ax0] : 0.0f;
    float c11 = row_hi ? row_hi[ax1] : 0.0f;

    return (1.0f - fx) * (1.0f - fy) * c00
         + fx         * (1.0f - fy) * c10
         + (1.0f - fx) * fy         * c01
         + fx         * fy         * c11;
}


// ---------------------------------------------------------------------------
// HexTile Iop
// ---------------------------------------------------------------------------

class HexTile : public Iop
{
    // Knob storage
    float _tileScale;
    float _rotStrength;
    float _contrast;

    // Cached in _validate from input info
    int _inputWidth;
    int _inputHeight;
    int _inputOffsetX;
    int _inputOffsetY;

public:
    HexTile(Node* node);

    void        knobs(Knob_Callback f) override;
    void        _validate(bool for_real) override;
    void        _request(int x, int y, int r, int t,
                         ChannelMask channels, int count) override;
    void        engine(int y, int x, int r,
                       ChannelMask channels, Row& row) override;

    const char* Class()      const override { return CLASS; }
    const char* node_help()  const override { return HELP; }

    static const Iop::Description d;
};


HexTile::HexTile(Node* node)
    : Iop(node)
    , _tileScale(2.0f)
    , _rotStrength(0.5f)
    , _contrast(0.75f)
    , _inputWidth(0)
    , _inputHeight(0)
    , _inputOffsetX(0)
    , _inputOffsetY(0)
{}


// ---------------------------------------------------------------------------

void HexTile::_validate(bool for_real)
{
    copy_info();

    // No input connected → pass through empty
    if (input0().node() == nullptr) {
        _inputWidth = 0;
        _inputHeight = 0;
        return;
    }

    const Box& inputBox = input0().info();
    _inputOffsetX = inputBox.x();
    _inputOffsetY = inputBox.y();
    _inputWidth   = inputBox.r() - inputBox.x();
    _inputHeight  = inputBox.t() - inputBox.y();

    if (_inputWidth <= 0 || _inputHeight <= 0) {
        _inputWidth = 0;
        _inputHeight = 0;
        return;
    }

    // Expand output bbox to full format so the tiling covers the entire frame
    const Format& fmt = info_.format();
    info_.set(fmt.x(), fmt.y(), fmt.r(), fmt.t());

    set_out_channels(Mask_All);
}


// ---------------------------------------------------------------------------

void HexTile::_request(int x, int y, int r, int t,
                       ChannelMask channels, int count)
{
    if (_inputWidth <= 0 || _inputHeight <= 0) return;

    // We need the entire input texture for random-access sampling
    input0().request(_inputOffsetX, _inputOffsetY,
                     _inputOffsetX + _inputWidth,
                     _inputOffsetY + _inputHeight,
                     channels, count);
}


// ---------------------------------------------------------------------------

void HexTile::engine(int y, int x, int r,
                     ChannelMask channels, Row& row)
{
    // Guard: no valid input
    if (_inputWidth <= 0 || _inputHeight <= 0) {
        row.erase(channels);
        return;
    }

    // Create a Tile covering the full input region
    Tile tile(input0(),
              _inputOffsetX, _inputOffsetY,
              _inputOffsetX + _inputWidth,
              _inputOffsetY + _inputHeight,
              channels);
    if (aborted()) return;

    // Collect the requested channels into a flat list
    const int MAX_CHANS = 64;
    Channel chanList[MAX_CHANS];
    int     numChans = 0;
    for (Channel z = channels.first(); z != Chan_Black; z = channels.next(z)) {
        if (numChans < MAX_CHANS) chanList[numChans++] = z;
    }

    // Locate RGB channels for luminance computation
    int idxR = -1, idxG = -1, idxB = -1;
    for (int i = 0; i < numChans; i++) {
        if (chanList[i] == Chan_Red)   idxR = i;
        if (chanList[i] == Chan_Green) idxG = i;
        if (chanList[i] == Chan_Blue)  idxB = i;
    }
    const bool hasRGB = (idxR >= 0 && idxG >= 0 && idxB >= 0);

    // Precompute constants
    const float g_exp      = 7.0f;   // weight exponent (γ in paper)
    const float g_falloff  = 0.6f;   // falloff contrast (β in paper)
    const float Lw[3]      = {0.299f, 0.587f, 0.114f}; // Rec.601 luminance

    float kGain = 0.0f;
    if (_contrast != 0.5f) {
        kGain = logf(1.0f - _contrast) / logf(0.5f);
    }

    const float invIW = 1.0f / (float)_inputWidth;
    const float invIH = 1.0f / (float)_inputHeight;

    // Process each output pixel
    for (int px = x; px < r; px++) {

        // ---- 1. Compute UV in tile units ----
        float stx = (float)px * _tileScale * invIW;
        float sty = (float)y   * _tileScale * invIH;

        // ---- 2. Triangle grid ----
        float w1, w2, w3;
        float v1x, v1y, v2x, v2y, v3x, v3y;
        TriangleGrid(stx, sty, w1, w2, w3,
                     v1x, v1y, v2x, v2y, v3x, v3y);

        // ---- 3. Rotation matrices ----
        float c1a, c1b, c2a, c2b, c3a, c3b;
        LoadRot2x2(v1x, v1y, _rotStrength, c1a, c1b);
        LoadRot2x2(v2x, v2y, _rotStrength, c2a, c2b);
        LoadRot2x2(v3x, v3y, _rotStrength, c3a, c3b);

        // ---- 4. Hex tile centers ----
        float cen1x, cen1y, cen2x, cen2y, cen3x, cen3y;
        MakeCenST(v1x, v1y, cen1x, cen1y);
        MakeCenST(v2x, v2y, cen2x, cen2y);
        MakeCenST(v3x, v3y, cen3x, cen3y);

        // ---- 5. Random offsets ----
        float h1x, h1y, h2x, h2y, h3x, h3y;
        hexHash(v1x, v1y, h1x, h1y);
        hexHash(v2x, v2y, h2x, h2y);
        hexHash(v3x, v3y, h3x, h3y);

        // ---- 6. Compute sample UVs ----
        // st_i = rot_i * (st - cen_i) + cen_i + hash_i
        float dx1 = stx - cen1x, dy1 = sty - cen1y;
        float st1x = c1a * dx1 - c1b * dy1 + cen1x + h1x;
        float st1y = c1b * dx1 + c1a * dy1 + cen1y + h1y;

        float dx2 = stx - cen2x, dy2 = sty - cen2y;
        float st2x = c2a * dx2 - c2b * dy2 + cen2x + h2x;
        float st2y = c2b * dx2 + c2a * dy2 + cen2y + h2y;

        float dx3 = stx - cen3x, dy3 = sty - cen3y;
        float st3x = c3a * dx3 - c3b * dy3 + cen3x + h3x;
        float st3y = c3b * dx3 + c3a * dy3 + cen3y + h3y;

        // ---- 7. Sample all channels at the 3 positions ----
        float s[3][MAX_CHANS]; // s[vertex][channel]
        for (int i = 0; i < numChans; i++) {
            s[0][i] = sampleBilinear(tile, chanList[i], st1x, st1y,
                                     _inputWidth, _inputHeight,
                                     _inputOffsetX, _inputOffsetY);
            s[1][i] = sampleBilinear(tile, chanList[i], st2x, st2y,
                                     _inputWidth, _inputHeight,
                                     _inputOffsetX, _inputOffsetY);
            s[2][i] = sampleBilinear(tile, chanList[i], st3x, st3y,
                                     _inputWidth, _inputHeight,
                                     _inputOffsetX, _inputOffsetY);
        }

        // ---- 8. Compute blending weights ----
        // (luminance-based diffusion for color, exponentiated barycentric)
        float W[3];
        if (hasRGB) {
            // Luminance of each sample
            float D[3];
            D[0] = s[0][idxR] * Lw[0] + s[0][idxG] * Lw[1] + s[0][idxB] * Lw[2];
            D[1] = s[1][idxR] * Lw[0] + s[1][idxG] * Lw[1] + s[1][idxB] * Lw[2];
            D[2] = s[2][idxR] * Lw[0] + s[2][idxG] * Lw[1] + s[2][idxB] * Lw[2];

            // Diffusion: Dw = lerp(1.0, D, g_falloff)
            float Dw[3];
            Dw[0] = 1.0f + (D[0] - 1.0f) * g_falloff;
            Dw[1] = 1.0f + (D[1] - 1.0f) * g_falloff;
            Dw[2] = 1.0f + (D[2] - 1.0f) * g_falloff;

            // W = Dw * pow(barycentric, g_exp)
            W[0] = Dw[0] * powf(w1, g_exp);
            W[1] = Dw[1] * powf(w2, g_exp);
            W[2] = Dw[2] * powf(w3, g_exp);
        } else {
            // No RGB — fall back to plain exponentiated barycentric weights
            W[0] = powf(w1, g_exp);
            W[1] = powf(w2, g_exp);
            W[2] = powf(w3, g_exp);
        }

        // Normalize
        float wSum = W[0] + W[1] + W[2];
        if (wSum > 0.0f) {
            W[0] /= wSum;
            W[1] /= wSum;
            W[2] /= wSum;
        }

        // Contrast restoration (Gain3 S-curve)
        if (_contrast != 0.5f) {
            float gW[3];
            gW[0] = gainScalar(W[0], kGain);
            gW[1] = gainScalar(W[1], kGain);
            gW[2] = gainScalar(W[2], kGain);
            float gSum = gW[0] + gW[1] + gW[2];
            if (gSum > 0.0f) {
                W[0] = gW[0] / gSum;
                W[1] = gW[1] / gSum;
                W[2] = gW[2] / gSum;
            }
        }

        // ---- 9. Write blended output ----
        for (int i = 0; i < numChans; i++) {
            row.writable(chanList[i])[px] =
                W[0] * s[0][i] + W[1] * s[1][i] + W[2] * s[2][i];
        }
    }
}


// ---------------------------------------------------------------------------

void HexTile::knobs(Knob_Callback f)
{
    Float_knob(f, &_tileScale, "tile_scale", "tile scale");
    SetRange(f, 0.1, 100);
    Tooltip(f,
        "How many times the input texture repeats across the output. "
        "Higher values produce smaller hex tiles with more randomisation.");

    Float_knob(f, &_rotStrength, IRange(0.0, 1.0), "rot_strength", "rotation");
    Tooltip(f,
        "Random rotation applied to each hex tile. "
        "0.0 = no rotation (good for directional patterns like bricks). "
        "1.0 = full random rotation (good for organic textures).");

    Float_knob(f, &_contrast, IRange(0.01, 0.99), "contrast", "contrast");
    Tooltip(f,
        "Contrast restoration via an S-curve on the blending weights. "
        "0.50 = no adjustment (blending may look blurry). "
        "0.75 = recommended (restores crispness). "
        "0.99 = maximum contrast (may reveal the hex grid).");

    Divider(f, "");
}


// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static Iop* build(Node* node) { return new HexTile(node); }

const Iop::Description HexTile::d(CLASS, "Texture/HexTile", build);
