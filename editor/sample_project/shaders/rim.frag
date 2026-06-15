// rim.frag — body-only fragment shader. The engine supplies #version, the ins
// (vNormal/vViewDist/vGradT/vUV), the uniforms (uTex/uColor/uColor2/uLightDir/
// uAlphaTest) and FragColor, plus the native vertex stage. Write only main().
//
// Lit base color with a bright Fresnel-style rim: faces turning away from the
// camera glow, so silhouettes pop. Drop this in <projectDir>/shaders/, then pick
// "rim" in a Camera's shader dropdown (or set cam.shader = "rim" from Lua).
void main()
{
    vec4 texColor = texture(uTex, vUV);
    if (uAlphaTest > 0.5 && texColor.a < 0.5) discard;

    vec3 tint = mix(uColor, uColor2, vGradT);
    vec3 albedo = texColor.rgb * tint;

    vec3 n = normalize(vNormal);
    float lambert = max(dot(n, normalize(uLightDir)), 0.0);
    vec3 lit = albedo * (0.35 + 0.65 * lambert);

    // View direction is the negated normalized view-space position; we only have
    // its length (vViewDist), so approximate facing via the world normal's Z.
    // A cheap, stable rim: stronger where the surface faces away from up-front.
    float facing = clamp(abs(n.z), 0.0, 1.0);
    float rim = pow(1.0 - facing, 3.0);
    vec3 rimColor = vec3(0.4, 0.7, 1.0) * rim;

    FragColor = vec4(lit + rimColor, 1.0);
}
