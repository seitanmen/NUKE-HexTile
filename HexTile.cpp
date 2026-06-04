/*
 * HexTile — Nuke plugin for stochastic hex-tiling
 *
 * Copyright (c) 2026 kawata. All rights reserved.
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
#include "DDImage/Filter.h"

#include <cmath>
#include <algorithm>

using namespace DD::Image;

static const char* const CLASS = "HexTile";
static const char* const VERSION = "v1.0.0";
static const char* const HELP =
    "HexTile v1.0.0 — Stochastic hex-tiling based on Mikkelsen (JCGT 2022).\n"
    "Copyright (c) 2026 kawata. All rights reserved.\n\n"
    "Reduces visible tiling patterns by randomizing UV offsets and rotations "
    "per hex tile, then blending three samples with luminance-driven diffusion weights.\n\n"
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

static void hexHash(float px, float py, float& hx, float& hy)
{
    float rx = px * 127.1f + py * 311.7f;
    float ry = px * 269.5f + py * 183.3f;
    hx = fracf(sinf(rx) * 43758.5453f);
    hy = fracf(sinf(ry) * 43758.5453f);
}

static void TriangleGrid(float stx, float sty,
                         float& w1, float& w2, float& w3,
                         float& v1x, float& v1y,
                         float& v2x, float& v2y,
                         float& v3x, float& v3y)
{
    const float scale = 2.0f * sqrtf(3.0f);
    float sx = stx * scale;
    float sy = sty * scale;

    const float invSqrt3 = 0.5773502691896258f;
    const float twoInvSqrt3 = 1.1547005383792516f;
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

static void MakeCenST(float vx, float vy, float& cx, float& cy)
{
    const float invDenom = 1.0f / (2.0f * sqrtf(3.0f));
    const float sqrt3Half = 0.8660254037844386f;
    cx = (vx + 0.5f * vy) * invDenom;
    cy = (sqrt3Half * vy) * invDenom;
}

static void LoadRot2x2(float idx_x, float idx_y, float rotStrength,
                       float& cosa, float& sina)
{
    const float PI  = 3.1415926535897932f;
    const float TAU = 6.2831853071795865f;

    float angle = fabsf(idx_x * idx_y) + fabsf(idx_x + idx_y) + PI;

    angle = fmodf(angle, TAU);
    if (angle < 0.0f) angle += TAU;
    if (angle > PI)   angle -= TAU;

    angle *= rotStrength;

    cosa = cosf(angle);
    sina = sinf(angle);
}

static float gainScalar(float x, float k)
{
    if (x < 0.5f) {
        return 0.5f * powf(2.0f * x, k);
    } else {
        return 1.0f - 0.5f * powf(2.0f * (1.0f - x), k);
    }
}


// ---------------------------------------------------------------------------
// Filtered sampling from a Nuke Tile with UV wrapping
// ---------------------------------------------------------------------------

static float sampleFiltered(const Tile& tile, Channel z,
                            float u, float v,
                            int w, int h,
                            int offX, int offY,
                            const Filter& filter)
{
    u = fracf(u);
    v = fracf(v);

    float px = u * (float)w;
    float py = v * (float)h;

    // Nearest neighbor
    if (filter.impulse()) {
        int ix = (int)floorf(px + 0.5f) % w; if (ix < 0) ix += w;
        int iy = (int)floorf(py + 0.5f) % h; if (iy < 0) iy += h;
        const float* row = tile[z][iy + offY];
        return row ? row[ix + offX] : 0.0f;
    }

    int baseX = (int)floorf(px);
    int baseY = (int)floorf(py);
    float fracX = px - (float)baseX;
    float fracY = py - (float)baseY;

    Filter::Coefficients cx, cy;
    filter.get(fracX, 1.0f, cx);
    filter.get(fracY, 1.0f, cy);

    // Separable 2D: filter Y for each X column, then filter X
    const int MAX_TAPS = 32;
    float xFiltered[MAX_TAPS];

    for (int ix = 0; ix < cx.count && ix < MAX_TAPS; ix++) {
        int srcX = ((baseX + cx.first + ix) % w + w) % w;
        float ySum = 0.0f;
        for (int iy = 0; iy < cy.count && iy < MAX_TAPS; iy++) {
            int srcY = ((baseY + cy.first + iy) % h + h) % h;
            const float* row = tile[z][srcY + offY];
            float val = row ? row[srcX + offX] : 0.0f;
            ySum += val * cy.array[iy * cy.delta];
        }
        xFiltered[ix] = ySum * cy.normalize;
    }

    float xSum = 0.0f;
    for (int ix = 0; ix < cx.count && ix < MAX_TAPS; ix++) {
        xSum += xFiltered[ix] * cx.array[ix * cx.delta];
    }
    return xSum * cx.normalize;
}


// ---------------------------------------------------------------------------
// HexTile Iop
// ---------------------------------------------------------------------------

class HexTile : public Iop
{
    float _tileScale;
    float _rotStrength;
    float _contrast;
    bool  _scaleOutput;
    Filter _filter;

    int _inputWidth;
    int _inputHeight;
    int _inputOffsetX;
    int _inputOffsetY;
    Format _scaledFormat;

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
    , _scaleOutput(false)
    , _filter(Filter::Cubic)
    , _inputWidth(0)
    , _inputHeight(0)
    , _inputOffsetX(0)
    , _inputOffsetY(0)
{}


// ---------------------------------------------------------------------------

void HexTile::_validate(bool for_real)
{
    copy_info();

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

    if (_scaleOutput) {
        const Format& fmt = info_.format();
        int outW = (int)((float)(fmt.r() - fmt.x()) * _tileScale + 0.5f);
        int outH = (int)((float)(fmt.t() - fmt.y()) * _tileScale + 0.5f);
        _scaledFormat = Format(outW, outH);
        info_.format(_scaledFormat);
        info_.full_size_format(_scaledFormat);
        info_.set(0, 0, outW, outH);
    } else {
        const Format& fmt = info_.format();
        info_.set(fmt.x(), fmt.y(), fmt.r(), fmt.t());
    }

    set_out_channels(Mask_All);
}


// ---------------------------------------------------------------------------

void HexTile::_request(int x, int y, int r, int t,
                       ChannelMask channels, int count)
{
    if (_inputWidth <= 0 || _inputHeight <= 0) return;

    input0().request(_inputOffsetX, _inputOffsetY,
                     _inputOffsetX + _inputWidth,
                     _inputOffsetY + _inputHeight,
                     channels, count);
}


// ---------------------------------------------------------------------------

void HexTile::engine(int y, int x, int r,
                     ChannelMask channels, Row& row)
{
    if (_inputWidth <= 0 || _inputHeight <= 0) {
        row.erase(channels);
        return;
    }

    Tile tile(input0(),
              _inputOffsetX, _inputOffsetY,
              _inputOffsetX + _inputWidth,
              _inputOffsetY + _inputHeight,
              channels);
    if (aborted()) return;

    _filter.initialize();

    const int MAX_CHANS = 64;
    Channel chanList[MAX_CHANS];
    int     numChans = 0;
    for (Channel z = channels.first(); z != Chan_Black; z = channels.next(z)) {
        if (numChans < MAX_CHANS) chanList[numChans++] = z;
    }

    int idxR = -1, idxG = -1, idxB = -1;
    for (int i = 0; i < numChans; i++) {
        if (chanList[i] == Chan_Red)   idxR = i;
        if (chanList[i] == Chan_Green) idxG = i;
        if (chanList[i] == Chan_Blue)  idxB = i;
    }
    const bool hasRGB = (idxR >= 0 && idxG >= 0 && idxB >= 0);

    const float g_exp      = 7.0f;
    const float g_falloff  = 0.6f;
    const float Lw[3]      = {0.299f, 0.587f, 0.114f};

    float kGain = 0.0f;
    if (_contrast != 0.5f) {
        kGain = logf(1.0f - _contrast) / logf(0.5f);
    }

    const float invIW = 1.0f / (float)_inputWidth;
    const float invIH = 1.0f / (float)_inputHeight;

    // When scale_output is ON, output is tile_scale×larger so UV scale is 1.0.
    // When OFF, UV scale is tile_scale (maps output pixels to tile units).
    const float uvScale = _scaleOutput ? 1.0f : _tileScale;

    for (int px = x; px < r; px++) {

        float stx = (float)px * uvScale * invIW;
        float sty = (float)y   * uvScale * invIH;

        float w1, w2, w3;
        float v1x, v1y, v2x, v2y, v3x, v3y;
        TriangleGrid(stx, sty, w1, w2, w3,
                     v1x, v1y, v2x, v2y, v3x, v3y);

        float c1a, c1b, c2a, c2b, c3a, c3b;
        LoadRot2x2(v1x, v1y, _rotStrength, c1a, c1b);
        LoadRot2x2(v2x, v2y, _rotStrength, c2a, c2b);
        LoadRot2x2(v3x, v3y, _rotStrength, c3a, c3b);

        float cen1x, cen1y, cen2x, cen2y, cen3x, cen3y;
        MakeCenST(v1x, v1y, cen1x, cen1y);
        MakeCenST(v2x, v2y, cen2x, cen2y);
        MakeCenST(v3x, v3y, cen3x, cen3y);

        float h1x, h1y, h2x, h2y, h3x, h3y;
        hexHash(v1x, v1y, h1x, h1y);
        hexHash(v2x, v2y, h2x, h2y);
        hexHash(v3x, v3y, h3x, h3y);

        float dx1 = stx - cen1x, dy1 = sty - cen1y;
        float st1x = c1a * dx1 - c1b * dy1 + cen1x + h1x;
        float st1y = c1b * dx1 + c1a * dy1 + cen1y + h1y;

        float dx2 = stx - cen2x, dy2 = sty - cen2y;
        float st2x = c2a * dx2 - c2b * dy2 + cen2x + h2x;
        float st2y = c2b * dx2 + c2a * dy2 + cen2y + h2y;

        float dx3 = stx - cen3x, dy3 = sty - cen3y;
        float st3x = c3a * dx3 - c3b * dy3 + cen3x + h3x;
        float st3y = c3b * dx3 + c3a * dy3 + cen3y + h3y;

        float s[3][MAX_CHANS];
        for (int i = 0; i < numChans; i++) {
            s[0][i] = sampleFiltered(tile, chanList[i], st1x, st1y,
                                     _inputWidth, _inputHeight,
                                     _inputOffsetX, _inputOffsetY, _filter);
            s[1][i] = sampleFiltered(tile, chanList[i], st2x, st2y,
                                     _inputWidth, _inputHeight,
                                     _inputOffsetX, _inputOffsetY, _filter);
            s[2][i] = sampleFiltered(tile, chanList[i], st3x, st3y,
                                     _inputWidth, _inputHeight,
                                     _inputOffsetX, _inputOffsetY, _filter);
        }

        float W[3];
        if (hasRGB) {
            float D[3];
            D[0] = s[0][idxR] * Lw[0] + s[0][idxG] * Lw[1] + s[0][idxB] * Lw[2];
            D[1] = s[1][idxR] * Lw[0] + s[1][idxG] * Lw[1] + s[1][idxB] * Lw[2];
            D[2] = s[2][idxR] * Lw[0] + s[2][idxG] * Lw[1] + s[2][idxB] * Lw[2];

            float Dw[3];
            Dw[0] = 1.0f + (D[0] - 1.0f) * g_falloff;
            Dw[1] = 1.0f + (D[1] - 1.0f) * g_falloff;
            Dw[2] = 1.0f + (D[2] - 1.0f) * g_falloff;

            W[0] = Dw[0] * powf(w1, g_exp);
            W[1] = Dw[1] * powf(w2, g_exp);
            W[2] = Dw[2] * powf(w3, g_exp);
        } else {
            W[0] = powf(w1, g_exp);
            W[1] = powf(w2, g_exp);
            W[2] = powf(w3, g_exp);
        }

        float wSum = W[0] + W[1] + W[2];
        if (wSum > 0.0f) {
            W[0] /= wSum;
            W[1] /= wSum;
            W[2] /= wSum;
        }

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

    Bool_knob(f, &_scaleOutput, "scale_output", "scale output");
    Tooltip(f,
        "Multiply the output resolution by tile_scale so each hex tile "
        "retains the full input texture resolution. "
        "OFF: output matches the project format (tiles shrink as tile_scale grows). "
        "ON: output = format × tile_scale (each tile stays sharp).");

    Float_knob(f, &_rotStrength, IRange(0.0, 1.0), "rot_strength", "rotation");
    Tooltip(f,
        "Random rotation applied to each hex tile. "
        "0.0 = no rotation (good for directional patterns like bricks). "
        "1.0 = full random rotation (good for organic textures).");

    Float_knob(f, &_contrast, IRange(0.01, 0.99), "tile_blend", "tile blend");
    Tooltip(f,
        "Controls how the three hex tile samples are blended together. "
        "0.50 = no adjustment (blending may look blurry). "
        "0.75 = recommended (restores crispness). "
        "0.99 = maximum contrast (may reveal the hex grid).");

    Divider(f, "");

    _filter.knobs(f);

    Divider(f, "");

    Tab_knob(f, "About");
    Text_knob(f, "version", VERSION);
    Text_knob(f, "Copyright (c) 2026 kawata. All rights reserved.");
}


// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static Iop* build(Node* node) { return new HexTile(node); }

const Iop::Description HexTile::d(CLASS, "Texture/HexTile", build);
