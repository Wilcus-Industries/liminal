#version 410 core
// native/scene.vert — the clean, modern scene pass. No vertex snapping, no
// affine warp; perspective-correct everything. This is the default look.
//
// Shares the vertex attribute layout and the per-draw uniform contract with the
// retro pack so the Renderer can drive either with the same draw() calls — any
// retro-only uniform it still sets (uSnapRes, uAffine, fog) is simply absent
// here and Shader::set() no-ops on the missing location.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform mat3 uNormalMat; // transpose(inverse(mat3(model))) — survives non-uniform scale
// Vertical gradient (shared with retro): object-local height normalized 0..1.
uniform float uGradBase;
uniform float uGradInv;

out vec3 vNormal;
out float vViewDist;
out float vGradT;
// Perspective-correct UVs only — this is the whole point of "native": textures
// stay glued to the surface instead of swimming.
smooth out vec2 vUV;

void main()
{
    vec4 viewPos = uView * uModel * vec4(aPos, 1.0);

    vNormal = uNormalMat * aNormal;
    vViewDist = length(viewPos.xyz);
    vGradT = clamp((aPos.y - uGradBase) * uGradInv, 0.0, 1.0);
    vUV = aUV;

    gl_Position = uProj * viewPos;
}
