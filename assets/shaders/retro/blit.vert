#version 410 core
// blit.vert — fullscreen triangle for the low-res -> window upscale pass.
// A single oversized triangle (verts at -1..3) covers the screen with no
// diagonal seam and no vertex processing worth mentioning; the parts hanging
// off-screen are clipped for free.

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

out vec2 vUV;

void main()
{
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
