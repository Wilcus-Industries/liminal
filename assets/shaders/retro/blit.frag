#version 410 core
// blit.frag — sample the low-res scene texture and put it on screen.
// All the chunkiness comes from the texture's GL_NEAREST filtering; this
// shader just copies. No post effects in Phase 1.

in vec2 vUV;

uniform sampler2D uTex;

out vec4 FragColor;

void main()
{
    FragColor = vec4(texture(uTex, vUV).rgb, 1.0);
}
