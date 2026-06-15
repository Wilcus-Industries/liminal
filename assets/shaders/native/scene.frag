#version 410 core
// native/scene.frag — clean lighting, perspective-correct textures, no fog.
//
// One consistent directional light plus ambient. The deliberately-wrong
// double-light, the affine warp and the heavy near fog of the retro pack are
// all gone: this is an honest, readable render.

in vec3 vNormal;
in float vViewDist;
in float vGradT;     // 0 at the object's base, 1 at its top
smooth in vec2 vUV;  // perspective-correct

uniform sampler2D uTex;
uniform vec3 uColor;     // base color (gradient bottom)
uniform vec3 uColor2;    // top color (gradient top)
uniform vec3 uLightDir;  // single key light direction
// Opt-in hard cutout, shared semantics with the retro pack: 0 = opaque.
uniform float uAlphaTest;

out vec4 FragColor;

void main()
{
    vec4 texColor = texture(uTex, vUV);
    if (uAlphaTest > 0.5 && texColor.a < 0.5) discard;

    vec3 tint = mix(uColor, uColor2, vGradT);
    vec3 albedo = texColor.rgb * tint;

    vec3 n = normalize(vNormal);
    float lambert = max(dot(n, normalize(uLightDir)), 0.0);
    // Ambient floor keeps unlit faces readable instead of pure black.
    vec3 lit = albedo * (0.35 + 0.65 * lambert);

    FragColor = vec4(lit, 1.0);
}
