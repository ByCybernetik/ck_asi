#version 450
/* Land: Hermite weights OR retail TRANSITIONS atlas masks.
 * Atlas: 14×4 cells of 64×46 R8 (ProcessTransBmp: 0..31, 0x80 skip, 0xC0 opaque).
 *
 * Retail DrawSingleTile (0x477c20):
 *  - base = first present type in z-order (lowest z) — underpaint full cell
 *  - for each other type t: mask index (corner_bits(t) ^ 15), set = layer[t].trans + cy_parity
 * BMP {A–D}{NW}{NE}{SE}{SW} is DARK on named corners → xor so weight is high where t present.
 * types[] from CPU must be sorted ascending z (types[0] = base). */
layout(set = 0, binding = 0) uniform sampler2D uTex0;
layout(set = 0, binding = 1) uniform sampler2D uTex1;
layout(set = 0, binding = 2) uniform sampler2D uTex2;
layout(set = 0, binding = 3) uniform sampler2D uTex3;
layout(set = 0, binding = 4) uniform sampler2D uMaskAtlas;

layout(location = 0) in vec2 vUV;
layout(location = 1) in float vShade;
layout(location = 2) in vec4 vW;
layout(location = 3) in vec2 vMaskUV;
layout(location = 4) flat in uint vCz;
layout(location = 5) flat in uint vTypes;
layout(location = 6) flat in uint vMeta;
layout(location = 0) out vec4 outColor;

vec4 sample_wrap(sampler2D tex, vec2 world_xy) {
    /* DrawSingleTile paints VQ pixels in flat projected space:
     *   source_x = world_x
     *   source_y = world_y * 181/256
     * RenderTerrain applies height afterwards.  Using world_y directly applies
     * the 181/256 projection to texture features a second time.
     *
     * BC1 cache pads the two non-aligned retail heights:
     * ground1024 350→352 and grass1024 682→684.  Wrap at the logical VQ height
     * and normalize against the stored KTX height so padded rows are skipped. */
    vec2 ts = vec2(textureSize(tex, 0));
    float w = max(ts.x, 1.0);
    float stored_h = max(ts.y, 1.0);
    float logical_h = stored_h;
    /* Anim strips (swater/dwater 512×5120): wrap inside frame 0, not the stack. */
    if (stored_h >= w * 2.0) {
        stored_h = w;
        logical_h = w;
    } else if (abs(stored_h - 352.0) < 0.5 || abs(stored_h - 684.0) < 0.5) {
        logical_h = stored_h - 2.0;
    }
    const float kY = 181.0 / 256.0;
    float local_y = fract((world_xy.y * kY) / logical_h) * logical_h;
    vec2 uv = vec2(fract(world_xy.x / w), local_y / stored_h);
    return texture(tex, uv);
}

vec4 sample_bind(uint b, vec2 world_xy) {
    if (b == 0u) return sample_wrap(uTex0, world_xy);
    if (b == 1u) return sample_wrap(uTex1, world_xy);
    if (b == 2u) return sample_wrap(uTex2, world_xy);
    return sample_wrap(uTex3, world_xy);
}

void main() {
    float s = clamp(vShade, 0.0, 2.0);
    bool use_mask = (vMeta & 1u) != 0u;

    if (!use_mask) {
        vec4 w = max(vW, vec4(0.0));
        float ssum = w.x + w.y + w.z + w.w;
        if (ssum > 1e-5)
            w /= ssum;
        else
            w = vec4(1.0, 0.0, 0.0, 0.0);
        vec4 c = sample_wrap(uTex0, vUV) * w.x
               + sample_wrap(uTex1, vUV) * w.y
               + sample_wrap(uTex2, vUV) * w.z
               + sample_wrap(uTex3, vUV) * w.w;
        outColor = vec4(c.rgb * s, c.a);
        return;
    }

    uint c0 = vCz & 255u;
    uint c1 = (vCz >> 8u) & 255u;
    uint c2 = (vCz >> 16u) & 255u;
    uint c3 = (vCz >> 24u) & 255u;
    uint set_pack = (vMeta >> 16u) & 255u;
    uint nTypes = (vMeta >> 8u) & 7u;
    if (nTypes == 0u)
        nTypes = 1u;
    if (nTypes > 4u)
        nTypes = 4u;

    /* Inset half-texel inside each 64×46 atlas cell (NEAREST atlas). */
    vec2 mu = clamp(vMaskUV, vec2(0.0), vec2(1.0));
    float ux = (mu.x * 62.0 + 1.0) / 64.0;
    float uy = (mu.y * 44.0 + 1.0) / 46.0;

    /* types[0] = retail base underpaint; skip mask for base (local_c != local_4c). */
    vec3 acc = sample_bind(0u, vUV).rgb;

    for (uint i = 1u; i < nTypes; i++) {
        uint t = (vTypes >> (i * 8u)) & 255u;
        uint bits = 0u;
        if (c0 == t) bits |= 8u;
        if (c1 == t) bits |= 4u;
        if (c2 == t) bits |= 2u;
        if (c3 == t) bits |= 1u;
        if (bits == 0u)
            continue;
        vec4 texc = sample_bind(i, vUV);
        if (bits == 15u) {
            acc = texc.rgb;
            continue;
        }
        uint idx = bits ^ 15u; /* retail: (bits ^ 0xf) */
        if (idx < 1u || idx > 14u)
            continue;
        uint set = (set_pack >> (i * 2u)) & 3u;
        float col = float(idx - 1u);
        float row = float(set);
        vec2 auv = vec2((col + ux) / 14.0, (row + uy) / 4.0);
        float m255 = texture(uMaskAtlas, auv).r * 255.0;
        int mi = int(m255 + 0.5);
        if (mi == 0x80)
            continue;
        if (mi == 0xC0) {
            acc = texc.rgb;
            continue;
        }
        float w = clamp(float(mi) / 31.0, 0.0, 1.0);
        acc = mix(acc, texc.rgb, w);
    }
    outColor = vec4(acc * s, 1.0);
}
