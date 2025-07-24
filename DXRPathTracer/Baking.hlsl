//=================================================================================================
//
//  Baking.hlsl - DXR Baking Shaders
//  Based on RayTrace.hlsl
//
//=================================================================================================

#include <DescriptorTables.hlsl>
#include <Constants.hlsl>
#include <Quaternion.hlsl>
#include <BRDF.hlsl>
#include <RayTracing.hlsl>
#include <Sampling.hlsl>

#include "SharedTypes.h"
#include "AppSettings.hlsl"

// C++端绑定的常量缓冲结构体 (与RayTrace.hlsl一致)
struct RayTraceConstants
{
    row_major float4x4 InvViewProjection;
    float3 SunDirectionWS;
    float CosSunAngularRadius;
    float3 SunIrradiance;
    float SinSunAngularRadius;
    float3 SunRenderColor;
    uint Padding;
    float3 CameraPosWS;
    uint CurrSampleIdx;
    uint TotalNumPixels;
    uint VtxBufferIdx;
    uint IdxBufferIdx;
    uint GeometryInfoBufferIdx;
    uint MaterialBufferIdx;
    uint SkyTextureIdx;
    uint NumLights;
};

// C++端绑定的烘焙专用常量缓冲结构体
// 在 Baking.hlsl 顶部
struct BakingConstants
{
    uint SampleIndex; 
    uint SurfaceMapPositionIdx;
    uint SurfaceMapNormalIdx;
    uint Padding1;
};

struct LightConstants
{
    SpotLight Lights[MaxSpotLights];
    float4x4 ShadowMatrices[MaxSpotLights];
};

// 资源绑定
RaytracingAccelerationStructure Scene : register(t0, space200);

// UAVs
RWTexture2D<float4> g_AccumulationBuffer : register(u0);
RWTexture2D<float4> g_BakedLightMap      : register(u1);

// 常量缓冲
ConstantBuffer<RayTraceConstants> RayTraceCB : register(b0);
ConstantBuffer<LightConstants> LightCBuffer : register(b1);
ConstantBuffer<BakingConstants> BakingCB : register(b2); // 对应C++端的 RTParams_BakingCBuffer

SamplerState MeshSampler : register(s0);
SamplerState LinearSampler : register(s1);


// Payload 结构体 (与RayTrace.hlsl完全一致)
typedef BuiltInTriangleIntersectionAttributes HitAttributes;
struct PrimaryPayload
{
    float3 Radiance;
    float Roughness;
    uint PathLength;
    uint PixelIdx;
    uint SampleSetIdx;
    bool IsDiffuse;
};

struct ShadowPayload
{
    float Visibility;
};

// 光线类型枚举 (与RayTrace.hlsl完全一致)
enum RayTypes {
    RayTypeRadiance = 0,
    RayTypeShadow = 1,
    NumRayTypes = 2
};

// ------------------------------------------------------------------------------------------------
//  以下所有辅助函数、命中/未命中着色器都直接从 RayTrace.hlsl 复制而来，无需修改
// ------------------------------------------------------------------------------------------------
// 在 Baking.hlsl 中
static float2 SamplePoint(in uint pixelIdx, inout uint setIdx)
{
    const uint permutation = setIdx * RayTraceCB.TotalNumPixels + pixelIdx;
    setIdx += 1;
    return SampleCMJ2D(BakingCB.SampleIndex, AppSettings.SqrtNumSamples, AppSettings.SqrtNumSamples, permutation);
}

float3 PathTrace(in MeshVertex hitSurface, in Material material, in PrimaryPayload inPayload);
MeshVertex GetHitSurface(in HitAttributes attr, in uint geometryIdx);
Material GetGeometryMaterial(in uint geometryIdx);

[shader("closesthit")]
void ClosestHitShader(inout PrimaryPayload payload, in HitAttributes attr)
{
    const MeshVertex hitSurface = GetHitSurface(attr, GeometryIndex());
    const Material material = GetGeometryMaterial(GeometryIndex()); 
    payload.Radiance = PathTrace(hitSurface, material, payload);
}

[shader("anyhit")]
void AnyHitShader(inout PrimaryPayload payload, in HitAttributes attr)
{
    const MeshVertex hitSurface = GetHitSurface(attr, GeometryIndex());
    const Material material = GetGeometryMaterial(GeometryIndex());
    Texture2D opacityMap = ResourceDescriptorHeap[NonUniformResourceIndex(material.Opacity)];
    if(opacityMap.SampleLevel(MeshSampler, hitSurface.UV, 0.0f).x < 0.35f)
        IgnoreHit();
}

[shader("anyhit")]
void ShadowAnyHitShader(inout ShadowPayload payload, in HitAttributes attr)
{
    const MeshVertex hitSurface = GetHitSurface(attr, GeometryIndex());
    const Material material = GetGeometryMaterial(GeometryIndex());
    Texture2D opacityMap = ResourceDescriptorHeap[NonUniformResourceIndex(material.Opacity)];
    if(opacityMap.SampleLevel(MeshSampler, hitSurface.UV, 0.0f).x < 0.35f)
        IgnoreHit();
}

[shader("miss")]
void MissShader(inout PrimaryPayload payload)
{
    payload.Radiance = 1.0;
    if(AppSettings.EnableWhiteFurnaceMode)
    {
        payload.Radiance = 1.0.xxx;
    }
    else
    {
        const float3 rayDir = WorldRayDirection();
        TextureCube skyTexture = TexCubeTable[RayTraceCB.SkyTextureIdx];
        payload.Radiance = AppSettings.EnableSky ? skyTexture.SampleLevel(LinearSampler, rayDir, 0.0f).xyz : float3(0.0f, 0.0f, 0.0f);

        if(payload.PathLength == 1)
        {
            float cosSunAngle = dot(rayDir, RayTraceCB.SunDirectionWS);
            if(cosSunAngle >= RayTraceCB.CosSunAngularRadius)
                payload.Radiance = RayTraceCB.SunRenderColor;
        }
    }
}

[shader("closesthit")]
void ShadowHitShader(inout ShadowPayload payload, in HitAttributes attr)
{
    payload.Visibility = 0.0f;
}

[shader("miss")]
void ShadowMissShader(inout ShadowPayload payload)
{
    payload.Visibility = 1.0f;
}

MeshVertex GetHitSurface(in HitAttributes attr, in uint geometryIdx)
{
    float3 barycentrics = float3(1 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    StructuredBuffer<GeometryInfo> geoInfoBuffer = ResourceDescriptorHeap[RayTraceCB.GeometryInfoBufferIdx];
    const GeometryInfo geoInfo = geoInfoBuffer[geometryIdx];
    StructuredBuffer<MeshVertex> vtxBuffer = ResourceDescriptorHeap[RayTraceCB.VtxBufferIdx];
    Buffer<uint> idxBuffer = ResourceDescriptorHeap[RayTraceCB.IdxBufferIdx];
    const uint primIdx = PrimitiveIndex();
    const uint idx0 = idxBuffer[primIdx * 3 + geoInfo.IdxOffset + 0];
    const uint idx1 = idxBuffer[primIdx * 3 + geoInfo.IdxOffset + 1];
    const uint idx2 = idxBuffer[primIdx * 3 + geoInfo.IdxOffset + 2];
    const MeshVertex vtx0 = vtxBuffer[idx0 + geoInfo.VtxOffset];
    const MeshVertex vtx1 = vtxBuffer[idx1 + geoInfo.VtxOffset];
    const MeshVertex vtx2 = vtxBuffer[idx2 + geoInfo.VtxOffset];
    return BarycentricLerp(vtx0, vtx1, vtx2, barycentrics);
}
Material GetGeometryMaterial(in uint geometryIdx)
{
    StructuredBuffer<GeometryInfo> geoInfoBuffer = ResourceDescriptorHeap[RayTraceCB.GeometryInfoBufferIdx];
    const GeometryInfo geoInfo = geoInfoBuffer[geometryIdx];
    StructuredBuffer<Material> materialBuffer = ResourceDescriptorHeap[RayTraceCB.MaterialBufferIdx];
    return materialBuffer[geoInfo.MaterialIdx];
}
float3 PathTrace(in MeshVertex hitSurface, in Material material, in PrimaryPayload inPayload)
{
    if((!AppSettings.EnableDiffuse && !AppSettings.EnableSpecular) || (!AppSettings.EnableDirect && !AppSettings.EnableIndirect))
        return 0.0.xxx;
    if(inPayload.PathLength > 1 && !AppSettings.EnableIndirect)
        return 0.0.xxx;
    float3x3 tangentToWorld = float3x3(hitSurface.Tangent, hitSurface.Bitangent, hitSurface.Normal);
    const float3 positionWS = hitSurface.Position;
    const float3 incomingRayOriginWS = WorldRayOrigin();
    const float3 incomingRayDirWS = WorldRayDirection();
    float3 normalWS = hitSurface.Normal;
    if(AppSettings.EnableNormalMaps){
        Texture2D normalMap = ResourceDescriptorHeap[NonUniformResourceIndex(material.Normal)];
        float3 normalTS;
        normalTS.xy = normalMap.SampleLevel(MeshSampler, hitSurface.UV, 0.0f).xy * 2.0f - 1.0f;
        normalTS.z = sqrt(1.0f - saturate(normalTS.x * normalTS.x + normalTS.y * normalTS.y));
        normalWS = normalize(mul(normalTS, tangentToWorld));
        tangentToWorld._31_32_33 = normalWS;
    }
    float3 baseColor = 1.0f;
    if(AppSettings.EnableAlbedoMaps && !AppSettings.EnableWhiteFurnaceMode){
        Texture2D albedoMap = ResourceDescriptorHeap[NonUniformResourceIndex(material.Albedo)];
        baseColor = albedoMap.SampleLevel(MeshSampler, hitSurface.UV, 0.0f).xyz;
    }
    Texture2D metallicMap = ResourceDescriptorHeap[NonUniformResourceIndex(material.Metallic)];
    const float metallic = saturate((AppSettings.EnableWhiteFurnaceMode ? 1.0f : metallicMap.SampleLevel(MeshSampler, hitSurface.UV, 0.0f).x) * AppSettings.MetallicScale);
    const bool enableDiffuse = (AppSettings.EnableDiffuse && metallic < 1.0f) || AppSettings.EnableWhiteFurnaceMode;
    const bool enableSpecular = (AppSettings.EnableSpecular && (AppSettings.EnableIndirectSpecular ? !(AppSettings.AvoidCausticPaths && inPayload.IsDiffuse) : (inPayload.PathLength == 1)));
    if(enableDiffuse == false && enableSpecular == false)
        return 0.0f;
    Texture2D roughnessMap = ResourceDescriptorHeap[NonUniformResourceIndex(material.Roughness)];
    const float sqrtRoughness = saturate((AppSettings.EnableWhiteFurnaceMode ? 1.0f : roughnessMap.SampleLevel(MeshSampler, hitSurface.UV, 0.0f).x) * AppSettings.RoughnessScale);
    const float3 diffuseAlbedo = lerp(baseColor, 0.0f, metallic) * (enableDiffuse ? 1.0f : 0.0f);
    const float3 specularAlbedo = lerp(0.03f, baseColor, metallic) * (enableSpecular ? 1.0f : 0.0f);
    float roughness = sqrtRoughness * sqrtRoughness;
    if(AppSettings.ClampRoughness)
        roughness = max(roughness, inPayload.Roughness);
    float3 msEnergyCompensation = 1.0.xxx;
    if(AppSettings.ApplyMultiscatteringEnergyCompensation){
        float2 DFG = GGXEnvironmentBRDFScaleBias(saturate(dot(normalWS, -incomingRayDirWS)), sqrtRoughness);
        float Ess = DFG.x;
        msEnergyCompensation = 1.0.xxx + specularAlbedo * (1.0f / Ess - 1.0f);
    }
    Texture2D emissiveMap = ResourceDescriptorHeap[NonUniformResourceIndex(material.Emissive)];
    float3 radiance = AppSettings.EnableWhiteFurnaceMode ? 0.0.xxx : emissiveMap.SampleLevel(MeshSampler, hitSurface.UV, 0.0f).xyz;
    if(AppSettings.EnableSun && !AppSettings.EnableWhiteFurnaceMode){
        float3 sunDirection = RayTraceCB.SunDirectionWS;
        if(AppSettings.SunAreaLightApproximation){
            float3 D = RayTraceCB.SunDirectionWS;
            float3 R = reflect(incomingRayDirWS, normalWS);
            float r = RayTraceCB.SinSunAngularRadius;
            float d = RayTraceCB.CosSunAngularRadius;
            float DDotR = dot(D, R);
            float3 S = R - DDotR * D;
            sunDirection = DDotR < d ? normalize(d * D + normalize(S) * r) : R;
        }
        RayDesc ray; ray.Origin = positionWS; ray.Direction = RayTraceCB.SunDirectionWS; ray.TMin = 0.00001f; ray.TMax = FP32Max;
        ShadowPayload payload; payload.Visibility = 1.0f;
        uint traceRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
        if(inPayload.PathLength > AppSettings.MaxAnyHitPathLength) traceRayFlags = RAY_FLAG_FORCE_OPAQUE;
        const uint hitGroupOffset = RayTypeShadow; const uint hitGroupGeoMultiplier = NumRayTypes; const uint missShaderIdx = RayTypeShadow;
        TraceRay(Scene, traceRayFlags, 0xFFFFFFFF, hitGroupOffset, hitGroupGeoMultiplier, missShaderIdx, ray, payload);
        radiance += CalcLighting(normalWS, sunDirection, RayTraceCB.SunIrradiance, diffuseAlbedo, specularAlbedo, roughness, positionWS, incomingRayOriginWS, msEnergyCompensation) * payload.Visibility;
    }
    if (AppSettings.RenderLights){
        for (uint spotLightIdx = 0; spotLightIdx < RayTraceCB.NumLights; spotLightIdx++){
            SpotLight spotLight = LightCBuffer.Lights[spotLightIdx];
            float3 surfaceToLight = spotLight.Position - positionWS; float distanceToLight = length(surfaceToLight); surfaceToLight /= distanceToLight;
            float angleFactor = saturate(dot(surfaceToLight, spotLight.Direction)); float angularAttenuation = smoothstep(spotLight.AngularAttenuationY, spotLight.AngularAttenuationX, angleFactor);
            float d = distanceToLight / spotLight.Range; float falloff = saturate(1.0f - (d * d * d * d)); falloff = (falloff * falloff) / (distanceToLight * distanceToLight + 1.0f);
            angularAttenuation *= falloff;
            if (angularAttenuation > 0.0f){
                RayDesc ray; ray.Origin = positionWS + normalWS * 0.01f; ray.Direction = surfaceToLight; ray.TMin = SpotShadowNearClip; ray.TMax = distanceToLight - SpotShadowNearClip;
                ShadowPayload payload; payload.Visibility = 1.0f;
                uint traceRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
                if (inPayload.PathLength > AppSettings.MaxAnyHitPathLength) traceRayFlags = RAY_FLAG_FORCE_OPAQUE;
                const uint hitGroupOffset = RayTypeShadow; const uint hitGroupGeoMultiplier = NumRayTypes; const uint missShaderIdx = RayTypeShadow;
                TraceRay(Scene, traceRayFlags, 0xFFFFFFFF, hitGroupOffset, hitGroupGeoMultiplier, missShaderIdx, ray, payload);
                float3 intensity = spotLight.Intensity * angularAttenuation;
                radiance += CalcLighting(normalWS, surfaceToLight, intensity, diffuseAlbedo, specularAlbedo, roughness, positionWS, incomingRayOriginWS, msEnergyCompensation) * payload.Visibility;
            }
        }
    }
    float2 brdfSample = SamplePoint(inPayload.PixelIdx, inPayload.SampleSetIdx);
    float3 throughput = 0.0f; float3 rayDirTS = 0.0f;
    float selector = brdfSample.x;
    if(enableSpecular == false) selector = 0.0f; else if(enableDiffuse == false) selector = 1.0f;
    if(selector < 0.5f){
        if(enableSpecular) brdfSample.x *= 2.0f;
        rayDirTS = SampleDirectionCosineHemisphere(brdfSample.x, brdfSample.y);
        throughput = diffuseAlbedo;
    } else {
        if(enableDiffuse) brdfSample.x = (brdfSample.x - 0.5f) * 2.0f;
        float3 incomingRayDirTS = normalize(mul(incomingRayDirWS, transpose(tangentToWorld)));
        float3 microfacetNormalTS = SampleGGXVisibleNormal(-incomingRayDirTS, roughness, roughness, brdfSample.x, brdfSample.y);
        float3 sampleDirTS = reflect(incomingRayDirTS, microfacetNormalTS);
        float3 normalTS = float3(0.0f, 0.0f, 1.0f);
        float3 F = AppSettings.EnableWhiteFurnaceMode ? 1.0.xxx : Fresnel(specularAlbedo, microfacetNormalTS, sampleDirTS);
        float G1 = SmithGGXMasking(normalTS, sampleDirTS, -incomingRayDirTS, roughness * roughness);
        float G2 = SmithGGXMaskingShadowing(normalTS, sampleDirTS, -incomingRayDirTS, roughness * roughness);
        throughput = (F * (G2 / G1)); rayDirTS = sampleDirTS;
        if(AppSettings.ApplyMultiscatteringEnergyCompensation){
            float2 DFG = GGXEnvironmentBRDFScaleBias(saturate(dot(normalTS, -incomingRayDirWS)), sqrtRoughness);
            float Ess = DFG.x; throughput *= 1.0.xxx + specularAlbedo * (1.0f / Ess - 1.0f);
        }
    }
    const float3 rayDirWS = normalize(mul(rayDirTS, tangentToWorld));
    if(enableDiffuse && enableSpecular) throughput *= 2.0f;
    RayDesc ray; ray.Origin = positionWS; ray.Direction = rayDirWS; ray.TMin = 0.00001f; ray.TMax = FP32Max;
    if(inPayload.PathLength == 1 && !AppSettings.EnableDirect) radiance = 0.0.xxx;
    if(AppSettings.EnableIndirect && (inPayload.PathLength + 1 < AppSettings.MaxPathLength) && !AppSettings.EnableWhiteFurnaceMode){
        PrimaryPayload payload; payload.Radiance = 0.0f; payload.PathLength = inPayload.PathLength + 1;
        payload.PixelIdx = inPayload.PixelIdx; payload.SampleSetIdx = inPayload.SampleSetIdx;
        payload.IsDiffuse = (selector < 0.5f); payload.Roughness = roughness;
        uint traceRayFlags = 0;
        if(payload.PathLength > AppSettings.MaxAnyHitPathLength) traceRayFlags = RAY_FLAG_FORCE_OPAQUE;
        const uint hitGroupOffset = RayTypeRadiance; const uint hitGroupGeoMultiplier = NumRayTypes; const uint missShaderIdx = RayTypeRadiance;
        TraceRay(Scene, traceRayFlags, 0xFFFFFFFF, hitGroupOffset, hitGroupGeoMultiplier, missShaderIdx, ray, payload);
        radiance += payload.Radiance * throughput;
    } else {
        ShadowPayload payload; payload.Visibility = 1.0f;
        uint traceRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
        if(inPayload.PathLength + 1 > AppSettings.MaxAnyHitPathLength) traceRayFlags = RAY_FLAG_FORCE_OPAQUE;
        const uint hitGroupOffset = RayTypeShadow; const uint hitGroupGeoMultiplier = NumRayTypes; const uint missShaderIdx = RayTypeShadow;
        TraceRay(Scene, traceRayFlags, 0xFFFFFFFF, hitGroupOffset, hitGroupGeoMultiplier, missShaderIdx, ray, payload);
        if(AppSettings.EnableWhiteFurnaceMode){
            radiance = throughput;
        } else {
            TextureCube skyTexture = TexCubeTable[RayTraceCB.SkyTextureIdx];
            float3 skyRadiance = AppSettings.EnableSky ? skyTexture.SampleLevel(LinearSampler, rayDirWS, 0.0f).xyz : 0.0.xxx;
            radiance += payload.Visibility * skyRadiance * throughput;
        }
    }
    return radiance;
}


// ------------------------------------------------------------------------------------------------
//  BakeRayGen: 核心的光线生成着色器
// ------------------------------------------------------------------------------------------------
[shader("raygeneration")]
void BakeRayGen()
{
    // 获取当前线程处理的像素坐标，这对应光照贴图中的一个texel
    const uint2 pixelCoord = DispatchRaysIndex().xy;
    /*
    g_BakedLightMap[pixelCoord] = float4(1.0, 0.0, 0.0, 1.0);
    g_AccumulationBuffer[pixelCoord] = float4(0, 0, 0, 0);
    return;*/

    const uint pixelIdx = pixelCoord.y * DispatchRaysDimensions().x + pixelCoord.x;
    
    // 从G-Buffer中采样表面数据
    Texture2D<float4> surfacePosMap = ResourceDescriptorHeap[NonUniformResourceIndex(BakingCB.SurfaceMapPositionIdx)];
    Texture2D<float4> surfaceNormalMap = ResourceDescriptorHeap[NonUniformResourceIndex(BakingCB.SurfaceMapNormalIdx)];
    
    float4 surfacePosData = surfacePosMap.Load(int3(pixelCoord, 0));
    
    // 如果w分量为0，说明该像素不在任何UV岛内，直接跳过
    if (surfacePosData.w == 0.0f)
    {
        return;
    }
    
    float3 worldPos = surfacePosData.xyz;
    if (any(isinf(worldPos)))
    {
        g_BakedLightMap[pixelCoord] = float4(0.0, 0.0, 1.0, 1.0); // 蓝色代表无穷大
        return;
    }
    float3 worldNormalVec = surfaceNormalMap.Load(int3(pixelCoord, 0)).xyz;
    if (dot(worldNormalVec, worldNormalVec) < 0.0001f)
    {
        // 如果是，说明这个 texel 来源于 UV 外部或数据无效。
        // 我们就不为它产生光线，直接将它标记为黑色并跳过。
        g_BakedLightMap[pixelCoord] = float4(0, 0, 0, 1);
        return;
    }
    float3 worldNormal = normalize(worldNormalVec);
    
    // 初始化随机数种子
    uint sampleSetIdx = 0;
    
    // --- 修改开始 ---
    // 1. 根据世界法线构建一个TBN变换矩阵 (Tangent-to-World)
    float3 up = abs(worldNormal.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, worldNormal));
    float3 bitangent = cross(worldNormal, tangent);
    float3x3 tangentToWorld = float3x3(tangent, bitangent, worldNormal);

    // 2. 在切线空间生成随机方向
    float2 hemisphereSample = SamplePoint(pixelIdx, sampleSetIdx);
    float3 rayDirTS = SampleDirectionCosineHemisphere(hemisphereSample.x, hemisphereSample.y);

    // 3. 将方向变换到世界空间
    float3 rayDir = mul(rayDirTS, tangentToWorld);
    // --- 修改结束 ---

    // 准备光线和Payload
    RayDesc ray;
    ray.Origin = worldPos + worldNormal * 0.001f; // 将起点沿法线方向稍微偏移，避免自相交
    ray.Direction = rayDir;
    ray.TMin = 0.0001f;
    ray.TMax = FP32Max;

    bool bIsOriginBad = any(isinf(ray.Origin)) || any(isnan(ray.Origin));
    bool bIsDirectionBad = any(isinf(ray.Direction)) || any(isnan(ray.Direction)) || length(ray.Direction) < 0.001f;

    PrimaryPayload payload;
    payload.Radiance = 0.0f;
    payload.Roughness = 0.0f;
    payload.PathLength = 1; // 这是路径的第一条光线
    payload.PixelIdx = pixelIdx;
    payload.SampleSetIdx = sampleSetIdx;
    payload.IsDiffuse = true; // 从漫反射表面发出，影响后续弹射

    // 发射光线
    uint traceRayFlags = 0;
    if(payload.PathLength > AppSettings.MaxAnyHitPathLength)
        traceRayFlags = RAY_FLAG_FORCE_OPAQUE;

    const uint hitGroupOffset = RayTypeRadiance;
    const uint hitGroupGeoMultiplier = NumRayTypes;
    const uint missShaderIdx = RayTypeRadiance;

    if (bIsOriginBad|| bIsDirectionBad)
    {
        g_BakedLightMap[pixelCoord] = float4(1.0, 0.0, 1.0, 1.0); // 蓝色代表无穷大
        return;
    }

    TraceRay(Scene, traceRayFlags, 0xFFFFFFFF, hitGroupOffset, hitGroupGeoMultiplier, missShaderIdx, ray, payload);
        
    // 累加结果
    float3 newSampleColor = payload.Radiance;
    
    // 读取上一帧的累加值
    float3 oldSum = g_AccumulationBuffer[pixelCoord].xyz;
    
    // 计算新的累加值并写回
    float3 newSum = oldSum + newSampleColor;
    g_AccumulationBuffer[pixelCoord] = float4(newSum, 1.0f);

    // 计算平均值并写入最终的光照贴图，供UI预览
    float3 averageColor = newSum / (BakingCB.SampleIndex + 1.0f);
    g_BakedLightMap[pixelCoord] = float4(averageColor, 1.0f);

}