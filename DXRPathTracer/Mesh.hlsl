//=================================================================================================
//
//  DXR Path Tracer
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#define UseImplicitShadowDerivatives_ 1

//=================================================================================================
// Includes
//=================================================================================================
#include "Shading.hlsl"
#include "AppSettings.hlsl"

//=================================================================================================
// Constant buffers
//=================================================================================================
struct VSConstants
{
    row_major float4x4 World;
    row_major float4x4 View;
    row_major float4x4 WorldViewProjection;
    float NearClip;
    float FarClip;
};

struct MatIndexConstants
{
    uint MatIndex;
};

struct SRVIndexConstants
{
    uint SunShadowMapIdx;
    uint SpotLightShadowMapIdx;
    uint MaterialTextureIndicesIdx;
    uint SpotLightClusterBufferIdx;
    uint LightMapIdx;
};

ConstantBuffer<VSConstants> VSCBuffer : register(b0);
ConstantBuffer<ShadingConstants> PSCBuffer : register(b0);
ConstantBuffer<SunShadowConstants> ShadowCBuffer : register(b1);
ConstantBuffer<MatIndexConstants> MatIndexCBuffer : register(b2);
ConstantBuffer<LightConstants> LightCBuffer : register(b3);
ConstantBuffer<SRVIndexConstants> SRVIndices : register(b4);

//=================================================================================================
// Resources
//=================================================================================================
SamplerState AnisoSampler : register(s0);
SamplerState LinearSampler : register(s1);
SamplerComparisonState PCFSampler : register(s2);

//=================================================================================================
// Input/Output structs
//=================================================================================================
struct VSInput
{
    float3 PositionOS           : POSITION;
    float3 NormalOS             : NORMAL;
    float2 UV                   : UV;
    float3 TangentOS            : TANGENT;
    float3 BitangentOS          : BITANGENT;
    float2 LightmapUV           : TEXCOORD1;
};

struct VSOutput
{
    float4 PositionCS           : SV_Position;

    float3 NormalWS             : NORMALWS;
    float3 PositionWS           : POSITIONWS;
    float DepthVS               : DEPTHVS;
    float3 TangentWS            : TANGENTWS;
    float3 BitangentWS          : BITANGENTWS;
    float2 UV                   : UV;
    float2 LightmapUV           : TEXCOORD1;
};

struct PSInput
{
    float4 PositionSS           : SV_Position;

    float3 NormalWS             : NORMALWS;
    float3 PositionWS           : POSITIONWS;
    float DepthVS               : DEPTHVS;
    float3 TangentWS            : TANGENTWS;
    float3 BitangentWS          : BITANGENTWS;
    float2 UV                   : UV;
    float2 LightmapUV           : TEXCOORD1;
};

struct PSOutputForward
{
    float4 Color : SV_Target0;
    float4 TangentFrame : SV_Target1;
};

//=================================================================================================
// Vertex Shader
//=================================================================================================
VSOutput VS(in VSInput input, in uint VertexID : SV_VertexID)
{
    VSOutput output;

    float3 positionOS = input.PositionOS;

    // Calc the world-space position
    output.PositionWS = mul(float4(positionOS, 1.0f), VSCBuffer.World).xyz;

    // Calc the clip-space position
    output.PositionCS = mul(float4(positionOS, 1.0f), VSCBuffer.WorldViewProjection);
    output.DepthVS = output.PositionCS.w;

    // Rotate the normal into world space
    output.NormalWS = normalize(mul(float4(input.NormalOS, 0.0f), VSCBuffer.World)).xyz;

    // Rotate the rest of the tangent frame into world space
    output.TangentWS = normalize(mul(float4(input.TangentOS, 0.0f), VSCBuffer.World)).xyz;
    output.BitangentWS = normalize(mul(float4(input.BitangentOS, 0.0f), VSCBuffer.World)).xyz;

    // Pass along the texture coordinates
    output.UV = input.UV;
    output.LightmapUV = input.LightmapUV;

    return output;
}

//=================================================================================================
// Pixel Shader for clustered forward rendering
//=================================================================================================
float4 PSForward(in PSInput input) : SV_Target0
{
    // --- 1. 公共部分：获取材质和执行Alpha测试 ---
    // 这部分代码无论使用哪种渲染路径都是必需的，所以我们把它放在最前面。

    StructuredBuffer<Material> materialBuffer = ResourceDescriptorHeap[SRVIndices.MaterialTextureIndicesIdx];
    Material material = materialBuffer[MatIndexCBuffer.MatIndex];
    Texture2D AlbedoMap = ResourceDescriptorHeap[material.Albedo];

    #if AlphaTest_
        Texture2D OpacityMap = ResourceDescriptorHeap[material.Opacity];
        if(OpacityMap.Sample(AnisoSampler, input.UV).x < 0.35f)
            discard;
    #endif


    // --- 2. 使用 AppSettings 开关选择渲染路径 ---

    if (AppSettings.EnableLightMapRender)
    {
        float4 albedoColor = AlbedoMap.Sample(AnisoSampler, input.UV);
        Texture2D lightMap = ResourceDescriptorHeap[NonUniformResourceIndex(SRVIndices.LightMapIdx)];
        float3 bakedLighting = lightMap.Sample(LinearSampler, input.LightmapUV).rgb;
        float3 finalColor = albedoColor.rgb * bakedLighting;
        return float4(finalColor, albedoColor.a);
    }
    else
    {
        float3 vtxNormalWS = normalize(input.NormalWS);
        float3 positionWS = input.PositionWS;

        float3 tangentWS = normalize(input.TangentWS);
        float3 bitangentWS = normalize(input.BitangentWS);
        float3x3 tangentFrame = float3x3(tangentWS, bitangentWS, vtxNormalWS);

        Texture2D NormalMap = ResourceDescriptorHeap[material.Normal];
        Texture2D RoughnessMap = ResourceDescriptorHeap[material.Roughness];
        Texture2D MetallicMap = ResourceDescriptorHeap[material.Metallic];
        Texture2D EmissiveMap = ResourceDescriptorHeap[material.Emissive];

        ShadingInput shadingInput;
        shadingInput.PositionSS = uint2(input.PositionSS.xy);
        shadingInput.PositionWS = input.PositionWS;
        shadingInput.PositionWS_DX = ddx_fine(input.PositionWS);
        shadingInput.PositionWS_DY = ddy_fine(input.PositionWS);
        shadingInput.DepthVS = input.DepthVS;
        shadingInput.TangentFrame = tangentFrame;
        shadingInput.AlbedoMap = AlbedoMap.Sample(AnisoSampler, input.UV);
        shadingInput.NormalMap = NormalMap.Sample(AnisoSampler, input.UV).xy;
        shadingInput.RoughnessMap = RoughnessMap.Sample(AnisoSampler, input.UV).x;
        shadingInput.MetallicMap = MetallicMap.Sample(AnisoSampler, input.UV).x;
        shadingInput.EmissiveMap = EmissiveMap.Sample(AnisoSampler, input.UV).xyz;
        shadingInput.SpotLightClusterBuffer = RawBufferTable[SRVIndices.SpotLightClusterBufferIdx];
        shadingInput.AnisoSampler = AnisoSampler;
        shadingInput.LinearSampler = LinearSampler;
        shadingInput.ShadingCBuffer = PSCBuffer;
        shadingInput.ShadowCBuffer = ShadowCBuffer;
        shadingInput.LightCBuffer = LightCBuffer;

        Texture2DArray sunShadowMap = Tex2DArrayTable[SRVIndices.SunShadowMapIdx];
        Texture2DArray spotLightShadowMap = Tex2DArrayTable[SRVIndices.SpotLightShadowMapIdx];

        float3 shadingResult = ShadePixel(shadingInput, sunShadowMap, spotLightShadowMap, PCFSampler);

        return float4(shadingResult, 1.0f);
    }
}