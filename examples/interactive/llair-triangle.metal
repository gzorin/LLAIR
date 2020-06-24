// GPU functions for the `llair-draw` example

#include <metal_stdlib>

struct Vertex {
    metal::float2 position;
    metal::float4 color;
};

struct RasterizerData {
    metal::float4 position [[ position ]];
    metal::float4 color;
};

vertex RasterizerData
VertexMain(constant Vertex       *vertices [[ buffer(0) ]],
           constant metal::uint2 *viewport [[ buffer(1) ]],
           uint vertex_id                  [[ vertex_id ]]) {
    return {
        metal::float4(vertices[vertex_id].position.xy / float2(*viewport) / 2.0, 0.0, 1.0),
        vertices[vertex_id].color
    };
}

fragment float4
FragmentMain(RasterizerData rasterizer_data [[stage_in]]) {
    return rasterizer_data.color;
}