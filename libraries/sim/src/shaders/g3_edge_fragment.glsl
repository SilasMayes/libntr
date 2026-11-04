R""(
#version 420 core

// The DS's edge marking (DISP3DCNT bit 5), after the frame's polygons are drawn: a pixel of an
// opaque polygon whose left, right, upper or lower neighbour, one DS pixel away, has another
// polygon ID and is farther away takes the edge colour of its own polygon ID (table entry
// ID / 8). Outside the 3D viewport and where no opaque polygon was drawn, the neighbour is the
// clear plane: CLEAR_COLOR's polygon ID, behind everything.
//
// With anti-aliasing (DISP3DCNT bit 4) the DS gives an edge pixel half coverage: half the edge
// colour over what is behind the polygon there. Here that is the farther neighbour (across the
// polygon's border), read from a copy of the frame made before this pass; over the clear plane
// the pixel is half transparent.
//
// attrTexture is what the 3D shader's second output left (g3_fragment.glsl): polygon ID times 4
// in r, the depth's high and low bytes in g and b, a set where an opaque polygon was drawn.
uniform sampler2D attrTexture;
uniform sampler2D colorTexture; // the 3D viewport's copy (anti-aliasing only), from its corner
uniform ivec4 viewportRect;     // x0, y0, x1, y1 (exclusive), in framebuffer pixels
uniform int pixelStep;          // framebuffer pixels per DS pixel
uniform int clearPolygonId;
uniform vec3 edgeColors[8];
uniform int antiAlias;
out vec4 color;

bool Inside(ivec2 p)
{
    return all(greaterThanEqual(p, viewportRect.xy)) && all(lessThan(p, viewportRect.zw));
}

ivec4 Attributes(ivec2 p)
{
    return ivec4(texelFetch(attrTexture, p, 0) * 255.0 + 0.5);
}

bool EdgeAgainst(ivec2 p, int polygonId, int depth)
{
    int otherId = clearPolygonId;
    int otherDepth = 65536;
    if (Inside(p)) {
        ivec4 other = Attributes(p);
        if (other.a != 0) {
            otherId = other.r >> 2;
            otherDepth = other.g * 256 + other.b;
        }
    }
    return otherId != polygonId && depth < otherDepth;
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec4 here = Attributes(p);
    if (here.a == 0) {
        discard;
    }
    int polygonId = here.r >> 2;
    int depth = here.g * 256 + here.b;
    ivec2 behind;
    if (EdgeAgainst(p + ivec2(pixelStep, 0), polygonId, depth)) {
        behind = p + ivec2(pixelStep, 0);
    } else if (EdgeAgainst(p - ivec2(pixelStep, 0), polygonId, depth)) {
        behind = p - ivec2(pixelStep, 0);
    } else if (EdgeAgainst(p + ivec2(0, pixelStep), polygonId, depth)) {
        behind = p + ivec2(0, pixelStep);
    } else if (EdgeAgainst(p - ivec2(0, pixelStep), polygonId, depth)) {
        behind = p - ivec2(0, pixelStep);
    } else {
        discard;
    }

    vec3 edge = edgeColors[polygonId >> 3];
    if (antiAlias == 0) {
        color = vec4(edge, 1.0); // the colour only (the blend keeps the pixel's alpha)
        return;
    }
    vec4 below = Inside(behind) ? texelFetch(colorTexture, behind - viewportRect.xy, 0) : vec4(0.0);
    float alpha = 0.5 + below.a * 0.5;
    color = vec4((edge * 0.5 + below.rgb * below.a * 0.5) / alpha, alpha);
}
)""
