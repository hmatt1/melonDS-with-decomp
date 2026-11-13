/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef MAIN_SHADERS_H
#define MAIN_SHADERS_H

const char* kScreenVS = R"(#version 140

uniform vec2 uScreenSize;
uniform mat2x3 uTransform;

in vec2 vPosition;
in vec3 vTexcoord;

smooth out vec3 fTexcoord;

void main()
{
    vec4 fpos;

    fpos.xy = vec3(vPosition, 1.0) * uTransform;

    fpos.xy = ((fpos.xy * 2.0) / uScreenSize) - 1.0;
    fpos.y *= -1;
    fpos.z = 0.0;
    fpos.w = 1.0;

    gl_Position = fpos;
    fTexcoord = vTexcoord;
}
)";

const char* kScreenFS = R"(#version 140

uniform sampler2DArray ScreenTex;
uniform bool uGrid;
uniform float uGridDark;   // vertical lines: 0 = none, 1 = black
uniform float uGridWidth;  // line width, whole output pixels
uniform float uGridSoft;   // edge softness, output pixels
uniform bool  uSubSample;  // each stripe takes its channel from the upscaled picture where it sits
uniform ivec2 uBlurReach;  // how many DS pixels the blur reaches each way (0 to 3), x and y
uniform float uBlurX;      // blur strength: weight of the neighbours within reach (0 to 1)
uniform float uBlurY;
uniform float uHalo;
uniform float uSubKeep;    // share of the light the stripes take that is given back (0 to 1)       // 0 = plain blur, 1 = blur as light, where bright pixels spread into a halo
uniform float uGridDarkH;  // horizontal lines: 0 = none, 1 = black
uniform int   uSubpixel;   // LED stripe layout: 0 none, 1 DS Lite, 2 DSi, 3 DSi XL
uniform float uSubStrength; // 0 = no tint, 1 = each stripe shows only its colour

smooth in vec3 fTexcoord;

out vec4 oColor;

// how much of each LED stripe (left, middle, right) output pixel column i of
// n lights, in row j. At 4x the rows are staggered:
//   even rows:  s0  s0+s1  s1     s2
//   odd rows:   s0  s1     s1+s2  s2
// and the middle stripe is scaled so that over the 4x2 tile all three come out
// equal. Any other zoom weights each stripe by how much of it the column covers.
vec3 stripeWeights(float i, float j, float n)
{
    if (abs(n - 4.0) < 0.01)
    {
        bool odd = mod(j, 2.0) >= 1.0;
        vec3 w;
        if      (i < 0.5) w = vec3(1, 0, 0);
        else if (i > 2.5) w = vec3(0, 0, 1);
        else if (i < 1.5) w = odd ? vec3(0, 1, 0) : vec3(0.5, 0.5, 0);
        else              w = odd ? vec3(0, 0.5, 0.5) : vec3(0, 1, 0);
        return w * vec3(1.0, 2.5 / 3.0, 1.0);
    }
    float a0 = i / n, a1 = (i + 1.0) / n;
    return clamp(min(vec3(a1), vec3(1.0, 2.0, 3.0) / 3.0)
               - max(vec3(a0), vec3(0.0, 1.0, 2.0) / 3.0), 0.0, 1.0) * n;
}

void main()
{
    const vec2  kDSRes     = vec2(256.0, 192.0); // base pixels
    vec2 p    = fTexcoord.xy * kDSRes;           // position in base pixels
    vec2 cell = (floor(p) + 0.5) / kDSRes;       // centre of this base pixel
    // at 1x take the DS pixel's centre; above 1x (3D upscaling) keep the finer
    // picture's own detail
    bool hires = textureSize(ScreenTex, 0).x > int(kDSRes.x);
    vec2 at = hires ? p : floor(p) + 0.5;        // where to read, in DS pixels
    if (!uGrid)
    {
        // screen filter Off: the plain picture, as the software display shows it
        oColor = vec4(texture(ScreenTex, vec3(at / kDSRes, fTexcoord.z)).rgb, 1.0);
        return;
    }
    vec3 pixel;
    vec2 bw = vec2(uBlurReach.x > 0 ? uBlurX : 0.0, uBlurReach.y > 0 ? uBlurY : 0.0);
    if (bw.x <= 0.0 && bw.y <= 0.0)
        pixel = texture(ScreenTex, vec3(at / kDSRes, fTexcoord.z)).rgb;
    else
    {
        // neighbours up to uBlurReach DS pixels away count with weight uBlurX/Y
        // (the pixel itself 1), so 1 is a full box blur. Averaged both on the
        // stored (gamma) values, like an ordinary image blur, and as light, where
        // bright pixels outweigh dark ones and swell into a halo; Halo mixes them
        vec3 acc = vec3(0.0), accLight = vec3(0.0);
        float sum = 0.0;
        for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
        {
            if (abs(dx) > uBlurReach.x || abs(dy) > uBlurReach.y) continue;
            float wgt = (dx == 0 ? 1.0 : bw.x) * (dy == 0 ? 1.0 : bw.y);
            if (wgt <= 0.0) continue;
            vec2 q = clamp(at + vec2(dx, dy), vec2(0.5), kDSRes - 0.5);
            vec3 c = texture(ScreenTex, vec3(q / kDSRes, fTexcoord.z)).rgb;
            acc += c * wgt;
            accLight += pow(c, vec3(2.2)) * wgt;
            sum += wgt;
        }
        pixel = mix(acc / sum, pow(accLight / sum, vec3(1.0 / 2.2)), uHalo);
    }

    // line on the right/bottom edge of each cell, inside the cell, so it
    // lands on whole output pixels at integer zoom instead of straddling two
    vec2 aa = fwidth(p);                         // one output pixel, in base pixels
    vec2 w  = aa * uGridWidth;                   // line width, in base pixels
    vec2 d  = 1.0 - fract(p);                    // distance to right/bottom edge
    vec2 s  = max(aa * uGridSoft * 0.5, vec2(1e-4));
    vec2 edge = 1.0 - smoothstep(w - s, w + s, d);
    if (!uGrid) edge = vec2(0.0);

    // subpixel stripes: each base pixel is three columns, B G R left to right
    // (DSi XL bottom screen: R G B); layer 0 is the top screen
    vec3 stripes = vec3(1.0);   // this output pixel's stripe mask, per channel
    float cellMean = 1.0;       // the mask's average over the DS pixel (same for each channel)
    vec3 lineComp = vec3(1.0);  // keeps the grid lines neutral over the stripes
    if (uSubpixel != 0)
    {
        // stripes in screen order, left to right, as R G B weights
        bool rgb = uSubpixel == 3 && fTexcoord.z > 0.5;
        vec3 s0 = rgb ? vec3(1, 0, 0) : vec3(0, 0, 1);
        vec3 s1 = vec3(0, 1, 0);
        vec3 s2 = rgb ? vec3(0, 0, 1) : vec3(1, 0, 0);
        float n = 1.0 / aa.x;                    // output pixels per DS pixel
        vec2 o = floor(fract(p) * n);             // this output pixel's column and row in the cell
        vec3 sw = stripeWeights(o.x, o.y, n);
        stripes = mix(vec3(1.0), sw.x * s0 + sw.y * s1 + sw.z * s2, uSubStrength);
        // each stripe covers a third of the cell, except at 4x where the
        // staggered rows give each 2.5 of 8 parts
        float stripeMean = abs(n - 4.0) < 0.01 ? 2.5 / 8.0 : 1.0 / 3.0;
        cellMean = mix(1.0, stripeMean, uSubStrength);

        // the vertical line covers the right stripe(s) more than the others, which
        // takes that colour out of the cell. Measure, over the cell's own output
        // pixels, how much light of each stripe the lines keep, and dim the other
        // stripes to match, so the lines stay neutral
        if (uGrid && (uGridDark > 0.0 || uGridDarkH > 0.0))
        {
            float m = clamp(floor(n + 0.5), 1.0, 8.0);
            vec3 keep = vec3(0.0), total = vec3(0.0);
            for (int jj = 0; jj < 8; jj++)
            for (int ii = 0; ii < 8; ii++)
            {
                if (float(ii) >= m || float(jj) >= m) continue;
                vec2 c = (vec2(ii, jj) + 0.5) / m;                  // pixel centre in the cell
                vec2 e = 1.0 - smoothstep(w - s, w + s, 1.0 - c);
                float dk = max(uGridDark * e.x, uGridDarkH * e.y);
                vec3 t = mix(vec3(1.0), stripeWeights(float(ii), float(jj), m), uSubStrength);
                keep  += t * pow(1.0 - dk, 2.2);
                total += t;
            }
            keep /= total;
            float k = min(keep.x, min(keep.y, keep.z));
            vec3 comp = k / keep;
            lineComp = comp.x * s0 + comp.y * s1 + comp.z * s2;
        }

        // subpixel sampling (needs 3x or more internal resolution): each stripe
        // keeps only its own channel, read at the stripe's centre
        if (uSubSample && textureSize(ScreenTex, 0).x >= 3 * int(kDSRes.x))
        {
            float x = floor(p.x);
            float y = fTexcoord.y;
            vec3 c0 = texture(ScreenTex, vec3((x + 1.0 / 6.0) / kDSRes.x, y, fTexcoord.z)).rgb;
            vec3 c1 = texture(ScreenTex, vec3((x + 3.0 / 6.0) / kDSRes.x, y, fTexcoord.z)).rgb;
            vec3 c2 = texture(ScreenTex, vec3((x + 5.0 / 6.0) / kDSRes.x, y, fTexcoord.z)).rgb;
            pixel = s0 * c0 + s1 * c1 + s2 * c2;
        }
    }

    float dark = max(uGridDark * edge.x, uGridDarkH * edge.y);

    // stripes are weights on light, and neighbouring screen pixels mix as light:
    // apply them in linear light, or split stripes (2x, 4x) come out too dark
    // and tint the picture
    vec3 light = pow(pixel, vec3(2.2));
    if (cellMean < 0.999)
    {
        // luminance preservation with overflow (after Harrison Allen's CRT shader):
        // to keep the DS pixel's average, the stripes need light / cellMean. The
        // lit stripe takes what fits; the rest spills evenly onto the other
        // stripes, so the average is kept exactly and nothing clips.
        // Keep brightness = 0 keeps only what the stripes let through
        vec3 target = light * mix(cellMean, 1.0, uSubKeep) / cellMean;
        vec3 primary = min(target, vec3(1.0));
        vec3 spill = (target - primary) * cellMean / (1.0 - cellMean);
        light = primary * stripes + spill * (1.0 - stripes);
    }
    vec3 shown = pow(light * lineComp, vec3(1.0 / 2.2));

    oColor = vec4(shown * (1.0 - dark), 1.0);
}
)";

// --- CRT filter ---------------------------------------------------------------------------------
// After the CRT shader in "CRT shader for pixel art" (spritecook.ai). SoftCrtFilter.cpp is the
// software version; a change to one should go to the other.
//
// The article's shader does everything per screen pixel (43 texture reads). Most of that depends
// only on DS pixels, so a prepass at 256x192 per screen does it once per frame: the YIQ signal
// with the luma prefilter, and the glow. The main pass then reads 18 texels and one bilinear glow.

// one triangle covering the 256x192 target; no vertex data
const char* kCrtPrepassVS = R"(#version 140

void main()
{
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kCrtPrepassFS = R"(#version 140

uniform sampler2DArray ScreenTex;
uniform float uLayer;          // 0 top screen, 1 bottom

out vec4 oYiq;                 // luma with the [1 2 1] prefilter, I, Q
out vec4 oGlow;                // 5x5 Gaussian of linear light (sigma 1.6 DS px)

const vec2 kDSRes = vec2(256.0, 192.0);
const vec3 kLuma = vec3(0.299, 0.587, 0.114);

// a DS pixel: the texel at its centre (with 3D upscaling, one sample per DS pixel), edges clamped.
// texelFetch on level 0: this pass draws at 256x192, so texture() would pick a smaller mip level
// of an upscaled picture, which can hold an old image
vec3 fetch(ivec2 t)
{
    t = clamp(t, ivec2(0), ivec2(kDSRes) - 1);
    ivec2 size = textureSize(ScreenTex, 0).xy;
    ivec2 at = ivec2((vec2(t) + 0.5) * vec2(size) / kDSRes);
    return texelFetch(ScreenTex, ivec3(at, int(uLayer)), 0).rgb;
}

void main()
{
    ivec2 t = ivec2(gl_FragCoord.xy);
    vec3 c = fetch(t);
    float y = 0.25 * (dot(fetch(t - ivec2(1, 0)), kLuma) + 2.0 * dot(c, kLuma) + dot(fetch(t + ivec2(1, 0)), kLuma));
    oYiq = vec4(y,
                dot(c, vec3(0.596, -0.274, -0.322)),
                dot(c, vec3(0.211, -0.523, 0.312)), 1.0);

    vec3 acc = vec3(0.0);
    float sum = 0.0;
    for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++)
    {
        float w = exp(-0.5 * float(dx * dx + dy * dy) / 2.56);
        acc += pow(fetch(t + ivec2(dx, dy)), vec3(2.4)) * w;
        sum += w;
    }
    oGlow = vec4(acc / sum, 1.0);
}
)";

const char* kCrtFS = R"(#version 140

uniform sampler2DArray YiqTex;     // prepass: prefiltered luma, I, Q per DS pixel (nearest)
uniform sampler2DArray GlowTex;    // prepass: glow per DS pixel, linear light (bilinear)
uniform float uCrtWarp;            // curvature 0..1
uniform float uCrtLumaBlur;        // luma Gaussian sigma, DS px
uniform float uCrtChromaBlur;      // chroma Gaussian sigma, DS px
uniform float uCrtBeamMin;         // beam sigma on black, DS rows
uniform float uCrtBeamMax;         // beam sigma on white
uniform float uCrtMask;            // aperture grille strength 0..1
uniform float uCrtMaskPx;          // screen pixels per stripe
uniform float uCrtGlow;
uniform float uCrtVignette;
uniform float uCrtBoost;

smooth in vec3 fTexcoord;

out vec4 oColor;

const vec2 kDSRes = vec2(256.0, 192.0);

// a scanline beam: thin on dark pixels, wider on bright ones, d DS rows from its centre
vec3 beam(vec3 lin, float d)
{
    vec3 s = max(mix(vec3(uCrtBeamMin), vec3(uCrtBeamMax), pow(lin, vec3(0.6))), vec3(1e-3));
    return lin * exp(-0.5 * d * d / (s * s));
}

void main()
{
    // the mask's stripes follow the screen's own x axis, so they turn with a rotated screen (as
    // in the software filter); here, before any early return, while derivatives are defined
    float maskX = abs(dFdx(fTexcoord.x)) >= abs(dFdy(fTexcoord.x)) ? gl_FragCoord.x : gl_FragCoord.y;

    // curvature
    vec2 c = fTexcoord.xy * 2.0 - 1.0;
    vec2 warp = uCrtWarp * vec2(0.124, 0.164);
    c *= vec2(1.0 + c.y * c.y * warp.x, 1.0 + c.x * c.x * warp.y);
    vec2 uv = c * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        oColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 p = uv * kDSRes;
    int layer = int(fTexcoord.z + 0.5);

    // horizontal signal blur: the 9 tap weights depend only on x, so both rows share them
    int x0 = int(floor(p.x)) - 4;
    float il = -0.5 / max(uCrtLumaBlur * uCrtLumaBlur, 1e-4);
    float ic = -0.5 / max(uCrtChromaBlur * uCrtChromaBlur, 1e-4);
    float wl[9], wc[9];
    float sl = 0.0, sc = 0.0;
    for (int k = 0; k < 9; k++)
    {
        float d = float(x0 + k) + 0.5 - p.x;
        wl[k] = exp(d * d * il);
        wc[k] = exp(d * d * ic);
        sl += wl[k];
        sc += wc[k];
    }

    // the two nearest DS rows, each a beam
    float y = p.y - 0.5;
    float r0 = floor(y);
    float f = y - r0;
    vec3 col = vec3(0.0);
    for (int r = 0; r < 2; r++)
    {
        int row = clamp(int(r0) + r, 0, 191);
        float Y = 0.0;
        vec2 IQ = vec2(0.0);
        for (int k = 0; k < 9; k++)
        {
            vec3 t = texelFetch(YiqTex, ivec3(clamp(x0 + k, 0, 255), row, layer), 0).rgb;
            Y += t.x * wl[k];
            IQ += t.yz * wc[k];
        }
        Y /= sl;
        IQ /= sc;
        vec3 rgb = clamp(vec3(Y + 0.956 * IQ.x + 0.621 * IQ.y,
                              Y - 0.272 * IQ.x - 0.647 * IQ.y,
                              Y - 1.106 * IQ.x + 1.703 * IQ.y), 0.0, 1.0);
        col += beam(pow(rgb, vec3(2.4)), r == 0 ? f : 1.0 - f);
    }
    col *= 1.32;

    col += texture(GlowTex, vec3(p / kDSRes, fTexcoord.z)).rgb * uCrtGlow;

    // aperture grille: R G B stripes uCrtMaskPx screen pixels wide
    float m = mod(floor(maskX / max(uCrtMaskPx, 1.0)), 3.0);
    vec3 on = vec3(equal(vec3(m), vec3(0.0, 1.0, 2.0)));
    col *= mix(vec3(mix(1.0, 0.62, uCrtMask)), vec3(mix(1.0, 1.22, uCrtMask)), on);

    // edge darkening, on the curved screen
    col *= 1.0 - uCrtVignette * pow(clamp(dot(c, c) * 0.5, 0.0, 1.0), 1.5);

    oColor = vec4(pow(clamp(col * uCrtBoost, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
)";

#endif // MAIN_SHADERS_H
