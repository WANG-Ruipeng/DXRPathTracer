// SurfaceMap.hlsl

#include <DescriptorTables.hlsl>
#include <Constants.hlsl>
#include <Quaternion.hlsl>
#include <BRDF.hlsl>
#include <RayTracing.hlsl>
#include <Sampling.hlsl>

#include "SharedTypes.h"
#include "AppSettings.hlsl"

struct VSInput
{
    float3 Position     : POSITION;
    float3 Normal       : NORMAL;
    float2 LightmapUV   : TEXCOORD1;
};

struct PSInput
{
    float4 Position     : SV_Position;
    float3 WorldPos     : WORLDPOS;
    float3 WorldNormal  : NORMAL;
};

// 顶点着色器：使用LightmapUV作为输出位置，并传递世界坐标/法线
PSInput VSMain(VSInput input)
{
    PSInput output;
    // 将 LightmapUV [0,1] 映射到裁剪空间 [-1, 1]
    output.Position.x = input.LightmapUV.x * 2.0f - 1.0f;
    output.Position.y = (1.0f - input.LightmapUV.y) * 2.0f - 1.0f; // Y轴翻转
    output.Position.z = 0.5f;
    output.Position.w = 1.0f;

    // 直接传递世界坐标和法线
    output.WorldPos = input.Position;
    output.WorldNormal = input.Normal;
    
    return output;
}

// 像素着色器：将世界坐标和法线输出到两个渲染目标
struct PSOutput
{
    float4 WorldPos     : SV_Target0;
    float4 WorldNormal  : SV_Target1;
};

PSOutput PSMain(PSInput input)
{
    PSOutput output;
    // SV_Target0: 存储世界坐标
    output.WorldPos = float4(input.WorldPos, 1.0f); 
    // SV_Target1: 存储世界法线 (XYZ) 和一个标记位 (W)
    output.WorldNormal = float4(normalize(input.WorldNormal), 1.0f);

    return output;
}