R""(
#version 420 core

// One triangle over the whole viewport, from gl_VertexID alone (no vertex buffer)
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)""
