#version 410 core
// dream.frag — texture warp selection, deliberately-wrong lighting, and the
// heavy near fog that does most of the mood work.

in vec3 vNormal;
in float vViewDist;
in float vGradT; // 0 at the object's base, 1 at its top

// Same UV interpolated two ways (see dream.vert). uAffine picks which one
// actually gets sampled; declared with matching qualifiers or the link fails.
smooth in vec2 vUVPersp;
noperspective in vec2 vUVAffine;

uniform sampler2D uTex;
uniform vec3 uColor;        // base color (gradient bottom)
uniform vec3 uColor2;       // top color (gradient top)
uniform float uAffine;      // 0 = perspective-correct, 1 = PS1 affine warp
uniform vec3 uFogColor;
uniform float uFogDensity;
// Per-draw multiplier on uFogDensity. 1 for everything; <1 lets one item (the
// figure) read through fog that has already swallowed its surroundings.
uniform float uFogScale;
uniform vec3 uLightDir;
uniform vec3 uShadeDir;
// Dream-decay plumbing. Phase 1 wires the value through but multiplies it by
// zero — no visual effect yet. Later phases will let it eat into fog density
// and color so the world unravels as coherence drops. Declared now so the
// C++ side's uniform set() calls are already exercised.
uniform float uDecayProgress;
// Opt-in cutout. 0 (the default for every opaque draw) leaves this path dead
// and behavior byte-identical: no discard, no branch taken on the texel alpha.
// 1 turns on alpha-test discard for decals/figures that carry transparent
// regions in their texture's alpha channel. Hard cutout only — there is no
// alpha blending anywhere in this engine (the hard texel edge is the look),
// so a texel either survives at full opacity or is discarded outright.
uniform float uAlphaTest;

out vec4 FragColor;

void main()
{
    // Affine warp is faded out near the camera: warp severity scales with the
    // depth range a triangle spans on screen, so ground polys right underfoot
    // (and decals viewed at grazing angles) smear unreadably while the same
    // warp at distance is the charming PS1 swim. Below ~1.5 units the mapping
    // is fully perspective-correct, beyond ~6 it is fully affine.
    float affine = uAffine * smoothstep(1.5, 6.0, vViewDist);
    vec2 uv = mix(vUVPersp, vUVAffine, affine);
    // Single texture fetch: we need both the rgb (albedo) and the alpha (cutout
    // test) from the same texel, so sample once into a vec4 and reuse it.
    vec4 texColor = texture(uTex, uv);
    // Alpha-test cutout, done before any lighting/fog so discarded fragments
    // cost nothing downstream. Gated on uAlphaTest so opaque draws (the vast
    // majority) never branch on alpha at all — when uAlphaTest is 0 the first
    // half short-circuits and this is a no-op.
    if (uAlphaTest > 0.5 && texColor.a < 0.5) discard;
    // Vertical gradient: tint runs base -> top up the object's local height.
    vec3 tint = mix(uColor, uColor2, vGradT);
    vec3 albedo = texColor.rgb * tint;

    vec3 n = normalize(vNormal);

    // DELIBERATELY INCONSISTENT LIGHTING. Two lambert terms from two
    // different directions are multiplied together: uLightDir brightens,
    // uShadeDir darkens, and they do not agree on where the sun is. The
    // light disagrees with its own shadows. Nothing in the scene can be
    // read as "correctly lit", which is exactly the off-ness we want —
    // subtle enough that you feel it before you see it.
    float lambert = max(dot(n, normalize(uLightDir)), 0.0);
    float shade   = max(dot(n, normalize(uShadeDir)), 0.0);
    vec3 lit = albedo * (0.35 + 0.65 * lambert) * (0.55 + 0.45 * shade);

    // exp2-style fog on radial view distance. Squaring the density*distance
    // term keeps a small clear bubble around the camera and then closes in
    // fast — heavy near fog is a core mood parameter, not a draw-distance
    // apology. Fog color is set per-area by the game and usually matches
    // the sky, so geometry dissolves into the air instead of hitting a wall.
    float d = uFogDensity * uFogScale * vViewDist;
    float f = clamp(1.0 - exp(-d * d), 0.0, 1.0);
    vec3 color = mix(lit, uFogColor, f);

    // Trivial use keeps uDecayProgress alive as plumbing; contributes nothing.
    color += vec3(uDecayProgress * 0.0);

    FragColor = vec4(color, 1.0);
}
