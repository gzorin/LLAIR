// GPU functions for the `llair-draw` example

#include <metal_stdlib>

struct Vertex {
    metal::float4 position [[ attribute(0) ]];
    metal::float4 color    [[ attribute(1) ]];
};

struct Viewport {
    metal::uint2 extent;
};

struct RasterizerData {
    metal::float4 position [[ position ]];
    metal::float4 color;
};

vertex RasterizerData
VertexMain(constant Viewport *viewport [[ buffer(1) ]],
           Vertex            in        [[ stage_in ]]) {
    return {
        metal::float4(in.position.xy / float2(viewport->extent) / 2.0, 0.0, 1.0),
        in.color
    };
}

fragment float4
FragmentMain(RasterizerData rasterizer_data [[stage_in]]) {
    return rasterizer_data.color;
}