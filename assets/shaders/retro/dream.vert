#version 410 core
// dream.vert — the PS1 half of the look lives here.
//
// Two period-correct artifacts are produced in this stage:
//   1. Vertex snapping: clip-space positions are quantized to the virtual
//      framebuffer's pixel grid, so vertices "pop" between pixels as the
//      camera moves instead of sliding smoothly.
//   2. Affine UVs: a second, noperspective-interpolated copy of the UVs is
//      emitted so the fragment shader can choose the warped PS1 mapping.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform mat3 uNormalMat;    // transpose(inverse(mat3(model))) — survives non-uniform scale
uniform vec2 uSnapRes;      // snap grid size, usually virtual res * snapScale
uniform float uSnapEnabled; // per-draw toggle (the "still" object opts out)
// Vertical gradient: object-local height normalized to 0..1. uGradBase is the
// mesh's local min-y, uGradInv = 1/height (0 for a flat mesh). The fragment
// shader mixes the base color toward the top color by this value.
uniform float uGradBase;
uniform float uGradInv;

out vec3 vNormal;
out float vViewDist;
out float vGradT;

// Same UV, two interpolation modes. GLSL interpolation qualifiers are baked
// at compile time — there is no way to flip noperspective on and off per
// draw — so we ship both varyings and let the fragment shader mix() between
// them with a uniform. The perspective-correct one is the modern default;
// the noperspective one is the PS1's affine texture mapping, which ignored
// depth during interpolation and made textures swim across large polygons.
smooth out vec2 vUVPersp;
noperspective out vec2 vUVAffine;

void main()
{
    vec4 viewPos = uView * uModel * vec4(aPos, 1.0);
    vec4 clip = uProj * viewPos;

    // VERTEX SNAPPING. The PlayStation's GTE produced integer screen
    // coordinates — there was no subpixel precision at all, so every vertex
    // landed exactly on a pixel center and jumped to the next pixel as it
    // moved. We recreate that by rounding NDC to the virtual framebuffer's
    // pixel grid. Done in clip space (then re-multiplied by w) so the
    // hardware's own perspective divide and clipping still work normally.
    // Guarded by clip.w > 0 because dividing by w behind the camera flips
    // and explodes coordinates before the clipper has had a chance to run.
    if (uSnapEnabled > 0.5 && clip.w > 0.0) {
        vec2 ndc = clip.xy / clip.w;
        // NDC spans 2 units across uSnapRes pixels, so one pixel is
        // 2/uSnapRes wide — hence the *0.5 factors around floor().
        ndc = floor(ndc * uSnapRes * 0.5 + 0.5) / (uSnapRes * 0.5);
        clip.xy = ndc * clip.w;
    }

    vNormal = uNormalMat * aNormal;
    vViewDist = length(viewPos.xyz); // radial distance for fog (not just -z)
    vGradT = clamp((aPos.y - uGradBase) * uGradInv, 0.0, 1.0);

    vUVPersp = aUV;
    vUVAffine = aUV;

    gl_Position = clip;
}
