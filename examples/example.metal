//-*-C++-*-
#include <metal_stdlib>
#include <metal_matrix>

using namespace metal;

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
