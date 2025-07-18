// UVVisualizer.hlsl

#include <DescriptorTables.hlsl>
#include <Constants.hlsl>
#include <Quaternion.hlsl>
#include <BRDF.hlsl>
#include <RayTracing.hlsl>
#include <Sampling.hlsl>

#include "SharedTypes.h" // 假设共享结构体在这里
#include "AppSettings.hlsl"

struct VSInput
{
    float2 LightmapUV : TEXCOORD1; // 我们只关心光照贴图UV
};

float4 VSMain(VSInput input) : SV_Position
{
    // UV 坐标范围是 [0, 1]。我们需要把它映射到裁剪空间 [-1, 1]。
    // (u, v) -> (u*2 - 1, v*2 - 1)
    // 同时，Y轴需要翻转，因为裁剪空间Y向上，而UV空间Y向下。
    float4 output;
    output.x = input.LightmapUV.x * 2.0f - 1.0f;
    output.y = (1.0f - input.LightmapUV.y) * 2.0f - 1.0f;
    output.z = 0.5f; // Z可以是任意值
    output.w = 1.0f;
    return output;
}

float4 PSMain() : SV_Target0
{
    // 直接输出一个固定的颜色，比如绿色
    return float4(0.0f, 1.0f, 0.0f, 1.0f);
}