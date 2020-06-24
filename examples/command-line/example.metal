//-*-C++-*-
#include <metal_stdlib>
#include <metal_matrix>

using namespace metal;

struct Vertex {
  float4 position [[ position ]];
};

vertex Vertex
vertex_main()
{
  Vertex result;
  result.position = float4(1.0, 2.0, 3.0, 1.0);
  return result;
}

vertex Vertex
other_vertex_main(device float4 *positions [[ buffer(0) ]])
{
  Vertex result;
  result.position = float4(1.0, 2.0, 3.0, 1.0);
  return result;
}

struct Fragment {
  float4 color [[ color(0) ]];
};

fragment Fragment
fragment_main()
{
  Fragment result;
  result.color = float4(1.0, 0.0, 0.0, 1.0);
  return result;
}
