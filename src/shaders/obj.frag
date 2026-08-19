#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vLayer;
layout(location = 2) in float vShadow;
layout(location = 3) in float vPc;
layout(location = 4) in vec3 vTeam;
layout(location = 0) out vec4 outColor;
/* Binding 0: BC3 albedo; binding 1: RGBA8 (soft-alpha legacy);
 * binding 2: R8 material (BuildRamp index+1). Layer >=1000 → RGBA path. */
layout(set = 0, binding = 0) uniform sampler2DArray atlasBc3;
layout(set = 0, binding = 1) uniform sampler2DArray atlasRgba;
layout(set = 0, binding = 2) uniform sampler2DArray atlasMat;

/*
 * KTX ver2 player_color — I2 native / retail BuildRamp (FUN_0046c110):
 *   material R = (i+1)/255  OR legacy RGBA A = (i+1)/255
 * Frag: team slot → retail 8×8 HSV shade from vTeam (not flat team×lum).
 */

int mul_sar_magic(int v, int magic, int sar)
{
    int msb, lsb;
    imulExtended(v, magic, msb, lsb);
    if (sar > 0)
        msb >>= sar;
    return msb;
}

void retail_rgb_to_hsv(int b, int g, int r, out int H, out int S, out int V)
{
    int mx = max(b, max(g, r));
    int mn = min(b, min(g, r));
    int chroma = mx - mn;
    V = mx << 4;
    if (mx == 0) {
        H = 0xffff;
        S = 0;
        return;
    }
    S = (chroma << 12) / mx;
    if (S == 0) {
        H = 0xffff;
        return;
    }
    int diff;
    int Hn;
    if (r == mx) {
        diff = g - b;
        Hn = (diff * 682) / chroma;
        if (g < b)
            Hn += 0x1000;
        H = Hn & 0xffff;
    } else if (g == mx) {
        diff = b - r;
        H = ((diff * 682) / chroma + 0x554) & 0xffff;
    } else {
        diff = r - g;
        H = ((diff * 682) / chroma + 0xaa8) & 0xffff;
    }
}

void retail_hsv_to_rgb(int H, int S, int V, out int b, out int g, out int r)
{
    if (S == 0) {
        int gray = (V >> 4) & 255;
        b = g = r = gray;
        return;
    }
    int edx = mul_sar_magic(H, int(0x300c0301u), 7);
    int sector = edx + (edx < 0 ? 1 : 0);
    int eax = (sector << 4) + sector;
    edx = eax + eax * 4;
    eax = sector + edx * 4;
    eax <<= 1;
    int rem = H - eax;
    uint f = uint(rem * S);
    /* uint64 f * 0x80601807 → high 32 (umulExtended). */
    uint hi, lo;
    umulExtended(f, 0x80601807u, hi, lo);
    uint ecx = f - hi;
    ecx >>= 1u;
    ecx += hi;
    ecx >>= 9u;
    uint p = ecx;
    uint Vx = uint(V);
    uint c1 = ((0x1000u - uint(S)) * Vx) >> 16u;
    uint c2 = ((0x1000u - p) * Vx) >> 16u;
    uint c3 = ((p - uint(S) + 0x1000u) * Vx) >> 16u;
    int vmax = int(Vx >> 4u) & 255;
    int dl = int(c1) & 255;
    int bl = int(c2) & 255;
    int al = int(c3) & 255;
    int cl = vmax;
    if (sector == 0) { b = dl; g = al; r = cl; }
    else if (sector == 1) { b = dl; g = cl; r = bl; }
    else if (sector == 2) { b = al; g = cl; r = dl; }
    else if (sector == 3) { b = cl; g = bl; r = dl; }
    else if (sector == 4) { b = cl; g = dl; r = al; }
    else if (sector == 5) { b = bl; g = dl; r = cl; }
    else { b = g = r = 0; }
}

uint retail_div7_scale(int v)
{
    int msb, lsb;
    imulExtended(v, int(0x92492493u), msb, lsb);
    int edx = msb + v;
    edx >>= 2;
    return uint(edx + (edx < 0 ? 1 : 0) + 0x100);
}

vec3 retail_ramp_shade(vec3 team, int index)
{
    int R = int(clamp(team.r, 0.0, 1.0) * 255.0 + 0.5);
    int G = int(clamp(team.g, 0.0, 1.0) * 255.0 + 0.5);
    int B = int(clamp(team.b, 0.0, 1.0) * 255.0 + 0.5);
    int r5 = R >> 3, g5 = G >> 3, b5 = B >> 3;
    R = r5 << 3;
    G = g5 << 3;
    B = b5 << 3;
    int H, S, V;
    retail_rgb_to_hsv(B, G, R, H, S, V);
    int row = index / 8;
    int col = index - row * 8;
    int ebp = row * 0xea0;
    int esi = col * 0xea0;
    uint scale_v = retail_div7_scale(ebp);
    uint newV = (uint(V) * scale_v) >> 12u;
    uint newS = (retail_div7_scale(esi) * uint(S)) >> 12u;
    int bb, gg, rr;
    retail_hsv_to_rgb(H, int(newS & 0xffffu), int(newV & 0xffffu), bb, gg, rr);
    int c = ((rr & 0xf8) << 7) | ((gg & 0xf8) << 2) | (bb >> 3);
    r5 = (c >> 10) & 31;
    g5 = (c >> 5) & 31;
    b5 = c & 31;
    return vec3(float((r5 << 3) | (r5 >> 2)),
                float((g5 << 3) | (g5 >> 2)),
                float((b5 << 3) | (b5 >> 2))) / 255.0;
}

void main() {
    vec4 c;
    float layer = vLayer;
    int use_rgba = layer >= 999.5 ? 1 : 0;
    if (use_rgba != 0)
        c = texture(atlasRgba, vec3(vUV, layer - 1000.0));
    else
        c = texture(atlasBc3, vec3(vUV, layer));
    if (c.a < 0.002)
        discard;
    if (vShadow > 0.5) {
        float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
        float a = c.a * mix(0.55, 0.28, clamp(lum, 0.0, 1.0));
        outColor = vec4(0.0, 0.0, 0.0, a);
        return;
    }
    if (use_rgba != 0) {
        /* Legacy: index-in-alpha on RGBA8 atlases. */
        float a8 = c.a * 255.0;
        if (a8 >= 0.5 && a8 <= 64.5) {
            int idx = int(a8 + 0.5) - 1;
            if (vPc > 0.5)
                c.rgb = retail_ramp_shade(vTeam, clamp(idx, 0, 63));
            c.a = 1.0;
        }
    } else if (vPc > 0.5) {
        /* BC3 albedo + R8 material map (same layer index). */
        float m8 = texture(atlasMat, vec3(vUV, layer)).r * 255.0;
        if (m8 >= 0.5 && m8 <= 64.5)
            c.rgb = retail_ramp_shade(vTeam, clamp(int(m8 + 0.5) - 1, 0, 63));
    }
    outColor = c;
}
