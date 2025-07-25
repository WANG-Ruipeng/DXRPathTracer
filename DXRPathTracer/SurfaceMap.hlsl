// SurfaceMap.hlsl

#include <DescriptorTables.hlsl>
#include <Constants.hlsl>
#include <Quaternion.hlsl>
#include <BRDF.hlsl>
#include <RayTracing.hlsl>
#include <Sampling.hlsl>

#include "SharedTypes.h"
#include "AppSettings.hlsl"

StructuredBuffer<GeometryInfo> GeoInfoBuffer    : register(t0, space1);
StructuredBuffer<Material>     MaterialBuffer   : register(t1, space1);
Texture2D                      AlbedoTextures[] : register(t0, space0);
SamplerState                   LinearSampler    : register(s1);

struct VSInput
{
    float3 Position     : POSITION;
    float3 Normal       : NORMAL;
    float2 TexCoord     : TEXCOORD0;
    float2 LightmapUV   : TEXCOORD1;
};

struct PSInput
{
    float4 Position     : SV_Position;
    float3 WorldPos     : WORLDPOS;
    float3 WorldNormal  : NORMAL;
    float2 TexCoord     : TEXCOORD0;
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

    output.TexCoord = input.TexCoord;
    
    return output;
}

// 像素着色器：将世界坐标和法线输出到两个渲染目标
struct PSOutput
{
    float4 WorldPos     : SV_Target0;
    float4 WorldNormal  : SV_Target1;
    float4 Albedo      : SV_Target2;
};

// 像素着色器
PSOutput PSMain(PSInput input, uint primitiveID : SV_PrimitiveID)
{
    PSOutput output;

    // SV_Target0: 存储世界坐标
    output.WorldPos = float4(input.WorldPos, 1.0f);

    // SV_Target1: 存储世界法线
    output.WorldNormal = float4(normalize(input.WorldNormal), 1.0f);

    // --- SV_Target2: 计算并存储Albedo ---
    
    // 1. 获取材质索引
    uint materialIdx = GeoInfoBuffer[primitiveID].MaterialIdx;

    // 2. 获取材质数据
    Material mat = MaterialBuffer[materialIdx];

    // 3. 计算最终的Albedo颜色
    float4 albedo = float4(1.0f, 1.0f, 1.0f, 1.0f); // 默认白色

    // 检查是否存在Albedo纹理 (索引不为0)
    if (mat.Albedo != 0xFFFFFFFF && mat.Albedo != 0) // 同时检查0和无效索引
    {
        uint texIdx = mat.Albedo;
        albedo = AlbedoTextures[texIdx].Sample(LinearSampler, input.TexCoord);
    }

    // 4. 将计算出的Albedo写入输出
    output.Albedo = albedo;

    return output;
}