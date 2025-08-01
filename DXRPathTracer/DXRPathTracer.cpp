//=================================================================================================
//
//  DXR Path Tracer
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#include <PCH.h>

#include <InterfacePointers.h>
#include <Window.h>
#include <Input.h>
#include <Utility.h>
#include <Graphics/SwapChain.h>
#include <Graphics/ShaderCompilation.h>
#include <Graphics/Profiler.h>
#include <Graphics/Textures.h>
#include <Graphics/Sampling.h>
#include <Graphics/DX12.h>
#include <Graphics/DX12_Helpers.h>
#include <Graphics/DXRHelper.h>
#include <Graphics/BRDF.h>
#include <EnkiTS/TaskScheduler_c.h>
#include <ImGui/ImGui.h>
#include <ImGuiHelper.h>
#include <d3d12sdklayers.h>

#include "DXRPathTracer.h"
#include "SharedTypes.h"
#include <wrl.h>

using Microsoft::WRL::ComPtr;
using namespace SampleFramework12;

#include <fstream>
#include <algorithm>

void SaveFloatVectorAsPPM(const std::string& filename, const std::vector<float>& data, int width, int height) {
    std::ofstream file(filename, std::ios::out | std::ios::binary);
    if (!file) return;

    file << "P6\n" << width << " " << height << "\n255\n"; // PPM 文件头

    for (int i = 0; i < width * height; ++i) {
        // 将浮点数 [0, 1] 范围转换到字节 [0, 255] 范围
        unsigned char r = static_cast<unsigned char>(std::min(1.0f, std::max(0.0f, data[i * 4 + 0])) * 255.0f);
        unsigned char g = static_cast<unsigned char>(std::min(1.0f, std::max(0.0f, data[i * 4 + 1])) * 255.0f);
        unsigned char b = static_cast<unsigned char>(std::min(1.0f, std::max(0.0f, data[i * 4 + 2])) * 255.0f);
        file.write(reinterpret_cast<const char*>(&r), 1);
        file.write(reinterpret_cast<const char*>(&g), 1);
        file.write(reinterpret_cast<const char*>(&b), 1);
    }
    file.close();
    OutputDebugStringA(("Saved image to " + filename + "\n").c_str());
}

void GpuCrashDumpCallback(const void* pGpuCrashDump, const uint32_t gpuCrashDumpSize, void* pUserData)
{
    // 收到崩溃回调，将数据写入文件
    std::ofstream dumpFile("DXRPathTracer_Crash.nv-gpudmp", std::ios::out | std::ios::binary);
    if (dumpFile)
    {
        dumpFile.write(static_cast<const char*>(pGpuCrashDump), gpuCrashDumpSize);
        dumpFile.close();
    }
}

// 新增这个回调函数用于诊断
void ShaderDebugInfoCallback(const void* pShaderDebugInfo, const uint32_t shaderDebugInfoSize, void* pUserData)
{
    // 只要这个函数被调用，就会在你的输出目录生成一个日志文件。
    std::ofstream logFile("shader_debug_info_log.txt", std::ios::app);
    logFile << "ShaderDebugInfoCallback was called! Size: " << shaderDebugInfoSize << std::endl;
    logFile.close();
    
    OutputDebugStringA("ShaderDebugInfoCallback was called!\n");
}

// Model filenames
static const wchar* ScenePaths[] =
{
    //L"..\\Content\\Models\\Sponza\\Sponza.fbx",
    L"..\\Content\\Models\\Sponza\\Sponza_NoSpotLight.fbx",
    L"..\\Content\\Models\\SunTemple\\SunTemple.fbx",
    nullptr,
    L"..\\Content\\Models\\WhiteFurnace\\WhiteFurnace.fbx",
    L"..\\Content\\Models\\theInn\\source\\theInn.fbx",
    //L"..\\Content\\Models\\cathedral\\source\\combined02.obj",
};

static const wchar* SceneTextureDirs[] = { nullptr, L"Textures", nullptr, nullptr, L"..\\textures" };
static const float SceneScales[] = { 0.01f, 0.005f, 1.0f, 1.0f, 0.1f };
static const Float3 SceneCameraPositions[] = { Float3(-11.5f, 1.85f, -0.45f), Float3(-1.0f, 5.5f, 12.0f), Float3(0.0f, 2.5f, -10.0f), Float3(0.0f, 0.0f, -3.0f) , Float3(0.0f, 0.0f, -30.0f) };
static const Float2 SceneCameraRotations[] = { Float2(0.0f, 1.544f), Float2(0.2f, 3.0f), Float2(0.0f, 0.0f), Float2(0.0f, 0.0f) , Float2(0.0f, 0.0f)};
static const Float3 SceneSunDirections[] = { Float3(0.26f, 0.987f, -0.16f), Float3(-0.133022308f, 0.642787635f, 0.75440651f), Float3(0.26f, 0.987f, -0.16f), Float3(0.0f, 1.0f, 0.0f) , Float3(-0.218f, 0.5f, -0.839f) };

StaticAssert_(ArraySize_(ScenePaths) == uint64(Scenes::NumValues));
StaticAssert_(ArraySize_(SceneTextureDirs) == uint64(Scenes::NumValues));
StaticAssert_(ArraySize_(SceneScales) == uint64(Scenes::NumValues));
StaticAssert_(ArraySize_(SceneCameraPositions) == uint64(Scenes::NumValues));
StaticAssert_(ArraySize_(SceneCameraRotations) == uint64(Scenes::NumValues));
StaticAssert_(ArraySize_(SceneSunDirections) == uint64(Scenes::NumValues));

static const uint64 NumConeSides = 16;

static const bool Benchmark = false;

static const uint32 LightMapResolution = 4096;

struct HitGroupRecord
{
    ShaderIdentifier ID;
};

StaticAssert_(sizeof(HitGroupRecord) % D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT == 0);

struct LightConstants
{
    SpotLight Lights[AppSettings::MaxSpotLights];
    Float4x4 ShadowMatrices[AppSettings::MaxSpotLights];
};

struct ClusterConstants
{
    Float4x4 ViewProjection;
    Float4x4 InvProjection;
    float NearClip = 0.0f;
    float FarClip = 0.0f;
    float InvClipRange = 0.0f;
    uint32 NumXTiles = 0;
    uint32 NumYTiles = 0;
    uint32 NumXYTiles = 0;
    uint32 ElementsPerCluster = 0;
    uint32 InstanceOffset = 0;
    uint32 NumLights = 0;

    uint32 BoundsBufferIdx = uint32(-1);
    uint32 VertexBufferIdx = uint32(-1);
    uint32 InstanceBufferIdx = uint32(-1);
};

struct RayTraceConstants
{
    Float4x4 InvViewProjection;

    Float3 SunDirectionWS;
    float CosSunAngularRadius = 0.0f;
    Float3 SunIrradiance;
    float SinSunAngularRadius = 0.0f;
    Float3 SunRenderColor;
    uint32 Padding = 0;
    Float3 CameraPosWS;
    uint32 CurrSampleIdx = 0;
    uint32 TotalNumPixels = 0;

    uint32 VtxBufferIdx = uint32(-1);
    uint32 IdxBufferIdx = uint32(-1);
    uint32 GeometryInfoBufferIdx = uint32(-1);
    uint32 MaterialBufferIdx = uint32(-1);
    uint32 SkyTextureIdx = uint32(-1);
    uint32 NumLights = 0;
};

enum ClusterRootParams : uint32
{
    ClusterParams_StandardDescriptors,
    ClusterParams_UAVDescriptors,
    ClusterParams_CBuffer,
    ClusterParams_AppSettings,

    NumClusterRootParams,
};

enum ResolveRootParams : uint32
{
    ResolveParams_StandardDescriptors,
    ResolveParams_Constants,
    ResolveParams_AppSettings,

    NumResolveRootParams
};

enum RTRootParams : uint32
{
    RTParams_StandardDescriptors,
    RTParams_SceneDescriptor,
    RTParams_UAVDescriptor,
    RTParams_CBuffer,
    RTParams_LightCBuffer,
    RTParams_AppSettings,
    RTParams_BakingCBuffer,

    NumRTRootParams
};

// Returns true if a sphere intersects a capped cone defined by a direction, height, and angle
static bool SphereConeIntersection(const Float3& coneTip, const Float3& coneDir, float coneHeight,
                                   float coneAngle, const Float3& sphereCenter, float sphereRadius)
{
    if(Float3::Dot(sphereCenter - coneTip, coneDir) > coneHeight + sphereRadius)
        return false;

    float cosHalfAngle = std::cos(coneAngle * 0.5f);
    float sinHalfAngle = std::sin(coneAngle * 0.5f);

    Float3 v = sphereCenter - coneTip;
    float a = Float3::Dot(v, coneDir);
    float b = a * sinHalfAngle / cosHalfAngle;
    float c = std::sqrt(Float3::Dot(v, v) - a * a);
    float d = c - b;
    float e = d * cosHalfAngle;

    return e < sphereRadius;
}

float Pow5(const float x)
{
    float xx = x * x;
    return xx * xx * x;
}

DXRPathTracer::DXRPathTracer(const wchar* cmdLine) : App(L"DXR Path Tracer", cmdLine)
{
    minFeatureLevel = D3D_FEATURE_LEVEL_11_1;
    globalHelpText = "DXR Path Tracer\n\n"
                     "Controls:\n\n"
                     "Use W/S/A/D/Q/E to move the camera, and hold right-click while dragging the mouse to rotate.";
}

void DXRPathTracer::BeforeReset()
{
}

void DXRPathTracer::AfterReset()
{
    float aspect = float(swapChain.Width()) / swapChain.Height();
    camera.SetAspectRatio(aspect);

    CreateRenderTargets();
}

void DXRPathTracer::Initialize()
{
    if(Benchmark)
    {
        AppSettings::EnableVSync.SetValue(false);
        AppSettings::StablePowerState.SetValue(true);
        AppSettings::AlwaysResetPathTrace.SetValue(true);
        AppSettings::CurrentScene.SetValue(Scenes::SunTemple);
    }

    // Check if the device supports conservative rasterization
    D3D12_FEATURE_DATA_D3D12_OPTIONS features = { };
    DX12::Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &features, sizeof(features));
    if(features.ConservativeRasterizationTier == D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED)
    {
        AppSettings::ClusterRasterizationMode.SetValue(ClusterRasterizationModes::MSAA8x);
        AppSettings::ClusterRasterizationMode.ClampNumValues(uint32(ClusterRasterizationModes::NumValues) - 1);
    }

    float aspect = float(swapChain.Width()) / swapChain.Height();
    camera.Initialize(aspect, Pi_4, 0.1f, 100.0f);

    ShadowHelper::Initialize(ShadowMapMode::DepthMap, ShadowMSAAMode::MSAA1x);

    InitializeScene();

    skybox.Initialize();

    postProcessor.Initialize();

    medianDenoiseCS = CompileFromFile(L"DenoiseMedian.hlsl", "DenoiseCS", ShaderType::Compute);

    {
        uvVisVS = CompileFromFile(L"UVVisualizer.hlsl", "VSMain", ShaderType::Vertex);
        uvVisPS = CompileFromFile(L"UVVisualizer.hlsl", "PSMain", ShaderType::Pixel);

        // --- 新增代码：为 UV 可视化器创建空白的根签名 ---
        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = 0;
        rootSignatureDesc.pParameters = nullptr;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        DX12::CreateRootSignature(&uvVisRS, rootSignatureDesc);

        // 给它起个名字，方便在调试器（如PIX）中识别
        uvVisRS->SetName(L"UV Visualizer Root Signature");
    }

    {
        // 编译 SurfaceMap 着色器
        surfaceMapVS = CompileFromFile(L"SurfaceMap.hlsl", "VSMain", ShaderType::Vertex);
        surfaceMapPS = CompileFromFile(L"SurfaceMap.hlsl", "PSMain", ShaderType::Pixel);

        // 编译 Baking DXR 库
        bakingLib = CompileFromFile(L"Baking.hlsl", nullptr, ShaderType::Library);

            // 创建 SurfaceMap 的根签名
        D3D12_ROOT_PARAMETER1 rootParameters[1] = {};

        // 参数0: 绑定全局的 SRV 描述符表 (包含所有Buffer和Texture)
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; // 仅像素着色器需要
        rootParameters[0].DescriptorTable.pDescriptorRanges = DX12::GlobalSRVDescriptorRanges();
        rootParameters[0].DescriptorTable.NumDescriptorRanges = DX12::NumGlobalSRVDescriptorRanges;

        // 静态采样器 (Sampler)
        D3D12_STATIC_SAMPLER_DESC staticSamplers[1] = {};
        staticSamplers[0] = DX12::GetStaticSamplerState(SamplerState::LinearClamp, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_ROOT_SIGNATURE_DESC1 surfaceMapRSDesc = {};
        surfaceMapRSDesc.NumParameters = ArraySize_(rootParameters);
        surfaceMapRSDesc.pParameters = rootParameters;
        surfaceMapRSDesc.NumStaticSamplers = ArraySize_(staticSamplers); // 添加采样器
        surfaceMapRSDesc.pStaticSamplers = staticSamplers;
        surfaceMapRSDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED; // 允许直接索引

        DX12::CreateRootSignature(&surfaceMapRS, surfaceMapRSDesc);
        surfaceMapRS->SetName(L"Surface Map Root Signature");
    }

    {
        // Spot light bounds and instance buffers
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(ClusterBounds);
        sbInit.NumElements = AppSettings::MaxSpotLights;
        sbInit.Dynamic = true;
        sbInit.CPUAccessible = true;
        spotLightBoundsBuffer.Initialize(sbInit);

        sbInit.Stride = sizeof(uint32);
        spotLightInstanceBuffer.Initialize(sbInit);
    }

    {
        // Spot light and shadow bounds buffer
        ConstantBufferInit cbInit;
        cbInit.Size = sizeof(LightConstants);
        cbInit.Dynamic = true;
        cbInit.CPUAccessible = false;
        cbInit.InitialState = D3D12_RESOURCE_STATE_COMMON;
        cbInit.Name = L"Spot Light Buffer";

        spotLightBuffer.Initialize(cbInit);
    }

    {
        CompileOptions opts;
        opts.Add("FrontFace_", 1);
        opts.Add("BackFace_", 0);
        opts.Add("Intersecting_", 0);

        // Clustering shaders
        clusterVS = CompileFromFile(L"Clusters.hlsl", "ClusterVS", ShaderType::Vertex, opts);
        clusterFrontFacePS = CompileFromFile(L"Clusters.hlsl", "ClusterPS", ShaderType::Pixel, opts);

        opts.Reset();
        opts.Add("FrontFace_", 0);
        opts.Add("BackFace_", 1);
        opts.Add("Intersecting_", 0);
        clusterBackFacePS = CompileFromFile(L"Clusters.hlsl", "ClusterPS", ShaderType::Pixel, opts);

        opts.Reset();
        opts.Add("FrontFace_", 0);
        opts.Add("BackFace_", 0);
        opts.Add("Intersecting_", 1);
        clusterIntersectingPS = CompileFromFile(L"Clusters.hlsl", "ClusterPS", ShaderType::Pixel, opts);
    }

    MakeConeGeometry(NumConeSides, spotLightClusterVtxBuffer, spotLightClusterIdxBuffer, coneVertices);

    // Compile resolve shaders
    for(uint64 msaaMode = 1; msaaMode < NumMSAAModes; ++msaaMode)
    {
        for(uint64 deferred = 0; deferred < 2; ++deferred)
        {
            CompileOptions opts;
            opts.Add("MSAASamples_", AppSettings::NumMSAASamples(MSAAModes(msaaMode)));
            resolvePS[msaaMode] = CompileFromFile(L"Resolve.hlsl", "ResolvePS", ShaderType::Pixel, opts);
        }
    }

    std::wstring fullScreenTriPath = SampleFrameworkDir() + L"Shaders\\FullScreenTriangle.hlsl";
    fullScreenTriVS = CompileFromFile(fullScreenTriPath.c_str(), "FullScreenTriangleVS", ShaderType::Vertex);

    {
        // Clustering root signature
        D3D12_DESCRIPTOR_RANGE1 uavRanges[1] = {};
        uavRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[0].NumDescriptors = 1;
        uavRanges[0].BaseShaderRegister = 0;
        uavRanges[0].RegisterSpace = 0;
        uavRanges[0].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER1 rootParameters[NumClusterRootParams] = {};

        // Standard SRV descriptors
        rootParameters[ClusterParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[ClusterParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParameters[ClusterParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::GlobalSRVDescriptorRanges();
        rootParameters[ClusterParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumGlobalSRVDescriptorRanges;

        // PS UAV descriptors
        rootParameters[ClusterParams_UAVDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[ClusterParams_UAVDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ClusterParams_UAVDescriptors].DescriptorTable.pDescriptorRanges = uavRanges;
        rootParameters[ClusterParams_UAVDescriptors].DescriptorTable.NumDescriptorRanges = ArraySize_(uavRanges);

        // CBuffer
        rootParameters[ClusterParams_CBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[ClusterParams_CBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[ClusterParams_CBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[ClusterParams_CBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[ClusterParams_CBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // AppSettings
        rootParameters[ClusterParams_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[ClusterParams_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[ClusterParams_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[ClusterParams_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;
        rootParameters[ClusterParams_AppSettings].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        DX12::CreateRootSignature(&clusterRS, rootSignatureDesc);
    }

    {
        // Resolve root signature
        D3D12_ROOT_PARAMETER1 rootParameters[NumResolveRootParams] = {};

        // Standard SRV descriptors
        rootParameters[ResolveParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[ResolveParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ResolveParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::GlobalSRVDescriptorRanges();
        rootParameters[ResolveParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumGlobalSRVDescriptorRanges;

        // CBuffer
        rootParameters[ResolveParams_Constants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[ResolveParams_Constants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ResolveParams_Constants].Constants.Num32BitValues = 3;
        rootParameters[ResolveParams_Constants].Constants.RegisterSpace = 0;
        rootParameters[ResolveParams_Constants].Constants.ShaderRegister = 0;

        // AppSettings
        rootParameters[ResolveParams_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[ResolveParams_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ResolveParams_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[ResolveParams_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;
        rootParameters[ResolveParams_AppSettings].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        DX12::CreateRootSignature(&resolveRootSignature, rootSignatureDesc);
    }

    {

    }

    oidnDenoiser.Initialize();

    InitRayTracing();
    
}

void DXRPathTracer::Shutdown()
{
    ShadowHelper::Shutdown();

    for(uint64 i = 0; i < ArraySize_(sceneModels); ++i)
        sceneModels[i].Shutdown();

    meshRenderer.Shutdown();
    skybox.Shutdown();
    skyCache.Shutdown();
    postProcessor.Shutdown();

    spotLightBuffer.Shutdown();
    spotLightBoundsBuffer.Shutdown();
    spotLightClusterBuffer.Shutdown();
    spotLightInstanceBuffer.Shutdown();

    DX12::Release(clusterRS);
    clusterMSAATarget.Shutdown();

    spotLightClusterVtxBuffer.Shutdown();
    spotLightClusterIdxBuffer.Shutdown();

    mainTarget.Shutdown();
    resolveTarget.Shutdown();
    depthBuffer.Shutdown();

    DX12::Release(resolveRootSignature);

    rtTarget.Shutdown();
    DX12::Release(rtRootSignature);
    rtBottomLevelAccelStructure.Shutdown();
    rtTopLevelAccelStructure.Shutdown();
    rtRayGenTable.Shutdown();
    rtHitTable.Shutdown();
    rtMissTable.Shutdown();
    rtGeoInfoBuffer.Shutdown();

    bakingRayGenTable.Shutdown();
    bakingHitTable.Shutdown();
    bakingMissTable.Shutdown();
    surfaceMap.Shutdown();
    surfaceMapNormal.Shutdown();
    surfaceMapAlbedo.Shutdown();
    accumulationBuffer.Shutdown();
    DX12::Release(surfaceMapRS);  

    oidnDenoiser.Shutdown();
    denoisedLightMap.Shutdown();
    bakedLightMap.Shutdown();
    uvLayoutMap.Shutdown();
    DX12::Release(uvVisRS);
    DX12::Release(medianDenoiseRS);

    m_lightmapReadbackBuffer.Shutdown();
}

void DXRPathTracer::VisualizeUVs(Model* model)
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    // 1. 将 uvLayoutMap 切换为渲染目标状态
    uvLayoutMap.MakeWritable(cmdList);

    // 2. 清除为黑色背景
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTargetView(uvLayoutMap.RTV, clearColor, 0, nullptr);

    // 3. 设置渲染状态
    cmdList->OMSetRenderTargets(1, &uvLayoutMap.RTV, false, nullptr);
    DX12::SetViewport(cmdList, uvLayoutMap.Width(), uvLayoutMap.Height());
    cmdList->SetPipelineState(uvVisPSO);
    cmdList->SetGraphicsRootSignature(uvVisRS);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 4. 绑定光照贴图几何体的缓冲
    D3D12_VERTEX_BUFFER_VIEW vbView = model->GetLightmappedVertexBuffer().VBView();
    D3D12_INDEX_BUFFER_VIEW ibView = model->GetLightmappedIndexBuffer().IBView();
    cmdList->IASetVertexBuffers(0, 1, &vbView);
    cmdList->IASetIndexBuffer(&ibView);

    // 5. 绘制所有网格
    const Array<Mesh>& meshesToDraw = model->GetLightmappedMeshes();
    for (const Mesh& mesh : meshesToDraw)
    {
        cmdList->DrawIndexedInstanced(mesh.NumIndices(), 1, mesh.IndexOffset(), mesh.VertexOffset(), 0);
    }

    // 6. 将 uvLayoutMap 切换回可读状态，以便HUD显示
    uvLayoutMap.MakeReadable(cmdList);
}

void DXRPathTracer::CreatePSOs()
{
    meshRenderer.CreatePSOs(mainTarget.Texture.Format, depthBuffer.DSVFormat, mainTarget.MSAASamples);
    skybox.CreatePSOs(mainTarget.Texture.Format, depthBuffer.DSVFormat, mainTarget.MSAASamples);
    postProcessor.CreatePSOs();

    {
        // Clustering PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = clusterRS;
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 0;
        psoDesc.VS = clusterVS.ByteCode();

        ClusterRasterizationModes rastMode = AppSettings::ClusterRasterizationMode;
        if(rastMode == ClusterRasterizationModes::MSAA4x || rastMode == ClusterRasterizationModes::MSAA8x)
        {
            psoDesc.SampleDesc.Count = clusterMSAATarget.MSAASamples;
            psoDesc.SampleDesc.Quality = DX12::StandardMSAAPattern;
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = clusterMSAATarget.Format();
        }
        else
            psoDesc.SampleDesc.Count = 1;

        D3D12_CONSERVATIVE_RASTERIZATION_MODE crMode = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        if(rastMode == ClusterRasterizationModes::Conservative)
            crMode = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;

        psoDesc.PS = clusterFrontFacePS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::BackFaceCull);
        psoDesc.RasterizerState.ConservativeRaster = crMode;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&clusterFrontFacePSO)));

        psoDesc.PS = clusterBackFacePS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::FrontFaceCull);
        psoDesc.RasterizerState.ConservativeRaster = crMode;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&clusterBackFacePSO)));

        psoDesc.PS = clusterIntersectingPS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::FrontFaceCull);
        psoDesc.RasterizerState.ConservativeRaster = crMode;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&clusterIntersectingPSO)));

        clusterFrontFacePSO->SetName(L"Cluster Front-Face PSO");
        clusterBackFacePSO->SetName(L"Cluster Back-Face PSO");
        clusterIntersectingPSO->SetName(L"Cluster Intersecting PSO");
    }

    const bool msaaEnabled = AppSettings::MSAAMode != MSAAModes::MSAANone;
    const uint64 msaaModeIdx = uint64(AppSettings::MSAAMode);

    if(msaaEnabled)
    {
        // Resolve PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = resolveRootSignature;
        psoDesc.VS = fullScreenTriVS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::NoCull);
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = mainTarget.Format();
        psoDesc.SampleDesc.Count = 1;

        psoDesc.PS = resolvePS[msaaModeIdx].ByteCode();
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resolvePSO)));
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = uvVisRS; // 使用你的简单根签名
    psoDesc.VS = uvVisVS.ByteCode();
    psoDesc.PS = uvVisPS.ByteCode();

    // --- 这是关键 ---
    psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::Wireframe); 

    psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
    psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled); // 我们不关心深度
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = uvLayoutMap.Format(); // 渲染目标格式必须匹配
    psoDesc.SampleDesc.Count = 1;

    // 我们需要一个特殊的 Input Layout，只读取 LightmapUV
    D3D12_INPUT_ELEMENT_DESC inputElements[] =
    {
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(MeshVertex, LightmapUV), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    psoDesc.InputLayout = { inputElements, ArraySize_(inputElements) };

    DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&uvVisPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC smPsoDesc = {};
    smPsoDesc.pRootSignature = surfaceMapRS;
    smPsoDesc.VS = surfaceMapVS.ByteCode();
    smPsoDesc.PS = surfaceMapPS.ByteCode();
    smPsoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::NoCull);
    smPsoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
    smPsoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled);
    smPsoDesc.SampleMask = UINT_MAX;
    smPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    smPsoDesc.NumRenderTargets = 3; // 输出到两个RT
    smPsoDesc.RTVFormats[0] = surfaceMap.Format();      // 世界坐标
    smPsoDesc.RTVFormats[1] = surfaceMapNormal.Format();      // 世界法线
    smPsoDesc.RTVFormats[2] = surfaceMapAlbedo.Format();      // 世界法线
    smPsoDesc.SampleDesc.Count = 1;

    D3D12_INPUT_ELEMENT_DESC smInputElements[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshVertex, Position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshVertex, Normal),   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(MeshVertex, UV),   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }, 
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(MeshVertex, LightmapUV), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    smPsoDesc.InputLayout = { smInputElements, ArraySize_(smInputElements) };
    DXCall(DX12::Device->CreateGraphicsPipelineState(&smPsoDesc, IID_PPV_ARGS(&surfaceMapPSO)));

    CreateRayTracingPSOs();

    {
        // --- 新代码块开始 ---
        // 为中值降噪计算通道创建根签名和PSO
        D3D12_ROOT_PARAMETER1 rootParameters[3] = {};

        // 参数 0: 常量 (FilterRadius)
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].Constants.Num32BitValues = 1;
        rootParameters[0].Constants.ShaderRegister = 0;
        rootParameters[0].Constants.RegisterSpace = 0;

        // 参数 1: 输入纹理 (SRV)
        D3D12_DESCRIPTOR_RANGE1 srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
        
        // 参数 2: 输出纹理 (UAV)
        D3D12_DESCRIPTOR_RANGE1 uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[2].DescriptorTable.pDescriptorRanges = &uavRange;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&medianDenoiseRS, rootSignatureDesc);
        medianDenoiseRS->SetName(L"Median Denoise Root Signature");

        // 创建 PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc1 = {};
        psoDesc1.pRootSignature = medianDenoiseRS;
        psoDesc1.CS = medianDenoiseCS.ByteCode();
        DXCall(DX12::Device->CreateComputePipelineState(&psoDesc1, IID_PPV_ARGS(&medianDenoisePSO)));
        medianDenoisePSO->SetName(L"Median Denoise PSO");
        // --- 新代码块结束 ---
    }

}

void DXRPathTracer::DestroyPSOs()
{
    meshRenderer.DestroyPSOs();
    skybox.DestroyPSOs();
    postProcessor.DestroyPSOs();
    DX12::DeferredRelease(clusterFrontFacePSO);
    DX12::DeferredRelease(clusterBackFacePSO);
    DX12::DeferredRelease(clusterIntersectingPSO);
    DX12::DeferredRelease(resolvePSO);

    DX12::DeferredRelease(rtPSO);
    DX12::DeferredRelease(uvVisPSO);
    DX12::DeferredRelease(surfaceMapPSO);

    DX12::DeferredRelease(bakingPSO);
    DX12::DeferredRelease(medianDenoisePSO);
}

// Creates all required render targets
void DXRPathTracer::CreateRenderTargets()
{
    uint32 width = swapChain.Width();
    uint32 height = swapChain.Height();
    const uint32 NumSamples = AppSettings::NumMSAASamples();

     {
        RenderTextureInit rtInit;
        rtInit.Width = width;
        rtInit.Height = height;
        rtInit.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtInit.MSAASamples = NumSamples;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = NumSamples == 1;
        rtInit.InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        rtInit.Name = L"Main Target";
        mainTarget.Initialize(rtInit);
    }

    if(NumSamples > 1)
    {
        RenderTextureInit rtInit;
        rtInit.Width = width;
        rtInit.Height = height;
        rtInit.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtInit.MSAASamples = 1;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = false;
        rtInit.InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        rtInit.Name = L"Resolve Target";
        resolveTarget.Initialize(rtInit);
    }

    {
       // 新增：创建用于光照贴图的纹理资源
       RenderTextureInit lmInit;
       lmInit.Width = LightMapResolution;
       lmInit.Height = LightMapResolution;
       lmInit.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;  // 使用高精度浮点格式存储HDR颜色
       lmInit.MSAASamples = 1;
       lmInit.ArraySize = 1;
       lmInit.CreateUAV = true;                         // 需要UAV视图以便于写入烘焙结果
       lmInit.InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; // 初始状态为可读，烘焙时再切换
       lmInit.Name = L"Baked Light Map";
       bakedLightMap.Initialize(lmInit);

        lmInit.Name = L"UV Layout Map";
       uvLayoutMap.Initialize(lmInit);

        lmInit.Name = L"Denoised Light Map"; // 降噪后的光照贴图
        //lmInit.CreateUAV = false; // 不需要 UAV 访问
        denoisedLightMap.Initialize(lmInit);
   }

   {
        // 创建 Surface Map (需要两个RT)
        RenderTextureInit smInit;
        smInit.Width = LightMapResolution;
        smInit.Height = LightMapResolution;
        smInit.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; // 高精度存储坐标和法线
        smInit.MSAASamples = 1;
        smInit.ArraySize = 1;
        smInit.CreateUAV = false; 
        smInit.Name = L"Surface Map";
        surfaceMap.Initialize(smInit);
        smInit.Name = L"Surface Map Normal";
        surfaceMapNormal.Initialize(smInit);
        smInit.Name = L"Surface Map Albedo"; // <-- 新增此代码块
        surfaceMapAlbedo.Initialize(smInit); 

        // 创建 Accumulation Buffer
        RenderTextureInit accumInit;
        accumInit.Width = LightMapResolution;
        accumInit.Height = LightMapResolution;
        accumInit.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; // 高精度累加
        accumInit.MSAASamples = 1;
        accumInit.ArraySize = 1;
        accumInit.CreateUAV = true;
        accumInit.Name = L"Accumulation Buffer";
        accumulationBuffer.Initialize(accumInit);
   }

    {
        DepthBufferInit dbInit;
        dbInit.Width = width;
        dbInit.Height = height;
        dbInit.Format = DXGI_FORMAT_D32_FLOAT;
        dbInit.MSAASamples = NumSamples;
        dbInit.Name = L"Main Depth Buffer";
        depthBuffer.Initialize(dbInit);
    }

    AppSettings::NumXTiles = (width + (AppSettings::ClusterTileSize - 1)) / AppSettings::ClusterTileSize;
    AppSettings::NumYTiles = (height + (AppSettings::ClusterTileSize - 1)) / AppSettings::ClusterTileSize;
    const uint64 numXYZTiles = AppSettings::NumXTiles * AppSettings::NumYTiles * AppSettings::NumZTiles;

    {
        // Render target for forcing MSAA during cluster rasterization. Ideally we would use ForcedSampleCount for this,
        // but it's currently causing the Nvidia driver to crash. :(
        RenderTextureInit rtInit;
        rtInit.Width = AppSettings::NumXTiles;
        rtInit.Height = AppSettings::NumYTiles;
        rtInit.Format = DXGI_FORMAT_R8_UNORM;
        rtInit.MSAASamples = 1;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = false;
        rtInit.Name = L"Deferred MSAA Target";

        ClusterRasterizationModes rastMode = AppSettings::ClusterRasterizationMode;
        if(rastMode == ClusterRasterizationModes::MSAA4x)
        {
            rtInit.MSAASamples = 4;
            clusterMSAATarget.Initialize(rtInit);
        }
        else if(rastMode == ClusterRasterizationModes::MSAA8x)
        {
            rtInit.MSAASamples = 8;
            clusterMSAATarget.Initialize(rtInit);
        }
        else
            clusterMSAATarget.Shutdown();
    }

    {
        // Spotlight cluster bitmask buffer
        RawBufferInit rbInit;
        rbInit.NumElements = numXYZTiles * AppSettings::SpotLightElementsPerCluster;
        rbInit.CreateUAV = true;
        rbInit.InitialState = D3D12_RESOURCE_STATE_COMMON;
        rbInit.Name = L"Spot Light Cluster Buffer";
        spotLightClusterBuffer.Initialize(rbInit);
    }

    {
        RenderTextureInit rtInit;
        rtInit.Width = width;
        rtInit.Height = height;
        rtInit.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        rtInit.CreateUAV = true;
        rtInit.Name = L"RT Target";
        rtTarget.Initialize(rtInit);
    }

    rtShouldRestartPathTrace = true;
}

void DXRPathTracer::InitializeScene()
{
    const uint64 currSceneIdx = uint64(AppSettings::CurrentScene);
    AppSettings::EnableWhiteFurnaceMode.SetValue(currSceneIdx == uint64(Scenes::WhiteFurnace));

    // Load the scene (if necessary)
    if(sceneModels[currSceneIdx].NumMeshes() == 0)
    {
        if(currSceneIdx == uint64(Scenes::BoxTest) || ScenePaths[currSceneIdx] == nullptr)
        {
            sceneModels[currSceneIdx].GenerateBoxTestScene();
        }
        else
        {
            ModelLoadSettings settings;
            settings.FilePath = ScenePaths[currSceneIdx];
            settings.TextureDir = SceneTextureDirs[currSceneIdx];
            settings.ForceSRGB = true;
            settings.SceneScale = SceneScales[currSceneIdx];
            settings.MergeMeshes = false;
            sceneModels[currSceneIdx].CreateWithAssimp(settings);
        }
    }

    currentModel = &sceneModels[currSceneIdx];
    meshRenderer.Shutdown();
    DX12::FlushGPU();
    meshRenderer.Initialize(currentModel);
    camera.SetPosition(SceneCameraPositions[currSceneIdx]);
    camera.SetXRotation(SceneCameraRotations[currSceneIdx].x);
    camera.SetYRotation(SceneCameraRotations[currSceneIdx].y);
    AppSettings::SunDirection.SetValue(SceneSunDirections[currSceneIdx]);

    {
        // Initialize the spotlight data used for rendering
        const uint64 numSpotLights = Min(currentModel->SpotLights().Size(), AppSettings::MaxSpotLights);
        spotLights.Init(numSpotLights);

        for(uint64 i = 0; i < numSpotLights; ++i)
        {
            const ModelSpotLight& srcLight = currentModel->SpotLights()[i];

            SpotLight& spotLight = spotLights[i];
            spotLight.Position = srcLight.Position;
            spotLight.Direction = -srcLight.Direction;
            spotLight.Intensity = srcLight.Intensity * 2500.0f;
            spotLight.AngularAttenuationX = std::cos(srcLight.AngularAttenuation.x * 0.5f);
            spotLight.AngularAttenuationY = std::cos(srcLight.AngularAttenuation.y * 0.5f);
            spotLight.Range = AppSettings::SpotLightRange;
        }
    }

    buildAccelStructure = true;
}

void DXRPathTracer::InitRayTracing()
{
    rayTraceLib = CompileFromFile(L"RayTrace.hlsl", nullptr, ShaderType::Library);

    {
        // RayTrace root signature
        D3D12_DESCRIPTOR_RANGE1 uavRanges[1] = {};
        uavRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[0].NumDescriptors = 4;
        uavRanges[0].BaseShaderRegister = 0;
        uavRanges[0].RegisterSpace = 0;
        uavRanges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        uavRanges[0].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER1 rootParameters[NumRTRootParams] = {};

        // Standard SRV descriptors
        rootParameters[RTParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[RTParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[RTParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::GlobalSRVDescriptorRanges();
        rootParameters[RTParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumGlobalSRVDescriptorRanges;

        // Acceleration structure SRV descriptor
        rootParameters[RTParams_SceneDescriptor].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParameters[RTParams_SceneDescriptor].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[RTParams_SceneDescriptor].Descriptor.ShaderRegister = 0;
        rootParameters[RTParams_SceneDescriptor].Descriptor.RegisterSpace = 200;

        // UAV descriptor
        rootParameters[RTParams_UAVDescriptor].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[RTParams_UAVDescriptor].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[RTParams_UAVDescriptor].DescriptorTable.pDescriptorRanges = uavRanges;
        rootParameters[RTParams_UAVDescriptor].DescriptorTable.NumDescriptorRanges = ArraySize_(uavRanges);

        // CBuffer
        rootParameters[RTParams_CBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[RTParams_CBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[RTParams_CBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[RTParams_CBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[RTParams_CBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // LightCBuffer
        rootParameters[RTParams_LightCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[RTParams_LightCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[RTParams_LightCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[RTParams_LightCBuffer].Descriptor.ShaderRegister = 1;
        rootParameters[RTParams_LightCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

        // AppSettings
        rootParameters[RTParams_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[RTParams_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[RTParams_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[RTParams_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;
        rootParameters[RTParams_AppSettings].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
        staticSamplers[0] = DX12::GetStaticSamplerState(SamplerState::Anisotropic, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
        staticSamplers[1] = DX12::GetStaticSamplerState(SamplerState::LinearClamp, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = ArraySize_(staticSamplers);
        rootSignatureDesc.pStaticSamplers = staticSamplers;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        rootParameters[RTParams_BakingCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[RTParams_BakingCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[RTParams_BakingCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[RTParams_BakingCBuffer].Descriptor.ShaderRegister = 2;
        rootParameters[RTParams_BakingCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        DX12::CreateRootSignature(&rtRootSignature, rootSignatureDesc);
    }

    rtCurrCamera = camera;
}

void DXRPathTracer::CreateRayTracingPSOs()
{
    StateObjectBuilder builder;
    builder.Init(12);

    {
        // DXIL library sub-object containing all of our code
        D3D12_DXIL_LIBRARY_DESC dxilDesc = { };
        dxilDesc.DXILLibrary = rayTraceLib.ByteCode();
        builder.AddSubObject(dxilDesc);
    }

    {
        // Primary hit group
        D3D12_HIT_GROUP_DESC hitDesc = { };
        hitDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitDesc.ClosestHitShaderImport = L"ClosestHitShader";
        hitDesc.HitGroupExport = L"HitGroup";
        builder.AddSubObject(hitDesc);
    }

    {
        // Primary alpha-test hit group
        D3D12_HIT_GROUP_DESC hitDesc = { };
        hitDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitDesc.ClosestHitShaderImport = L"ClosestHitShader";
        hitDesc.AnyHitShaderImport = L"AnyHitShader";
        hitDesc.HitGroupExport = L"AlphaTestHitGroup";
        builder.AddSubObject(hitDesc);
    }

    {
        // Shadow hit group
        D3D12_HIT_GROUP_DESC hitDesc = { };
        hitDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitDesc.ClosestHitShaderImport = L"ShadowHitShader";
        hitDesc.HitGroupExport = L"ShadowHitGroup";
        builder.AddSubObject(hitDesc);
    }

    {
        // Shadow alpha-test hit group
        D3D12_HIT_GROUP_DESC hitDesc = { };
        hitDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitDesc.ClosestHitShaderImport = L"ShadowHitShader";
        hitDesc.AnyHitShaderImport = L"ShadowAnyHitShader";
        hitDesc.HitGroupExport = L"ShadowAlphaTestHitGroup";
        builder.AddSubObject(hitDesc);
    }

    {
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = { };
        shaderConfig.MaxAttributeSizeInBytes = 2 * sizeof(float);                      // float2 barycentrics;
        shaderConfig.MaxPayloadSizeInBytes = 4 * sizeof(float) + 4 * sizeof(uint32);   // float3 radiance + float roughness + uint pathLength + uint pixelIdx + uint setIdx + bool IsDiffuse
        builder.AddSubObject(shaderConfig);
    }

    {
        // Global root signature with all of our normal bindings
        D3D12_GLOBAL_ROOT_SIGNATURE globalRSDesc = { };
        globalRSDesc.pGlobalRootSignature = rtRootSignature;
        builder.AddSubObject(globalRSDesc);
    }

    {
        // The path tracer is recursive, so set the max recursion depth to the max path length
        D3D12_RAYTRACING_PIPELINE_CONFIG configDesc = { };
        configDesc.MaxTraceRecursionDepth = AppSettings::MaxPathLengthSetting;
        builder.AddSubObject(configDesc);
    }

    rtPSO = builder.CreateStateObject(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

    // Get shader identifiers (for making shader records)
    ID3D12StateObjectProperties* psoProps = nullptr;
    rtPSO->QueryInterface(IID_PPV_ARGS(&psoProps));

    const void* rayGenID = psoProps->GetShaderIdentifier(L"RaygenShader");
    const void* hitGroupID = psoProps->GetShaderIdentifier(L"HitGroup");
    const void* alphaTestHitGroupID = psoProps->GetShaderIdentifier(L"AlphaTestHitGroup");
    const void* shadowHitGroupID = psoProps->GetShaderIdentifier(L"ShadowHitGroup");
    const void* shadowAlphaTestHitGroupID = psoProps->GetShaderIdentifier(L"ShadowAlphaTestHitGroup");
    const void* missID = psoProps->GetShaderIdentifier(L"MissShader");
    const void* shadowMissID = psoProps->GetShaderIdentifier(L"ShadowMissShader");

    // Make our shader tables
    {
        ShaderIdentifier rayGenRecords[1] = { ShaderIdentifier(rayGenID) };

        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(ShaderIdentifier);
        sbInit.NumElements = ArraySize_(rayGenRecords);
        sbInit.InitData = rayGenRecords;
        sbInit.ShaderTable = true;
        sbInit.Name = L"Ray Gen Shader Table";
        rtRayGenTable.Initialize(sbInit);
    }

    {
        ShaderIdentifier missRecords[2] = { ShaderIdentifier(missID), ShaderIdentifier(shadowMissID) };

        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(ShaderIdentifier);
        sbInit.NumElements = ArraySize_(missRecords);
        sbInit.InitData = missRecords;
        sbInit.ShaderTable = true;
        sbInit.Name = L"Miss Shader Table";
        rtMissTable.Initialize(sbInit);
    }

    {
        const uint32 numMeshes = uint32(currentModel->NumMeshes());

        Array<HitGroupRecord> hitGroupRecords(numMeshes * 2);
        for(uint64 i = 0; i < numMeshes; ++i)
        {
            // Use the alpha test hit group (with an any hit shader) if the material has an opacity map
            const Mesh& mesh = currentModel->Meshes()[i];
            Assert_(mesh.NumMeshParts() == 1);
            const uint32 materialIdx = mesh.MeshParts()[0].MaterialIdx;
            const MeshMaterial& material = currentModel->Materials()[materialIdx];
            const bool alphaTest = material.Textures[uint32(MaterialTextures::Opacity)] != nullptr;

            hitGroupRecords[i * 2 + 0].ID = alphaTest ? ShaderIdentifier(alphaTestHitGroupID) : ShaderIdentifier(hitGroupID);
            hitGroupRecords[i * 2 + 1].ID = alphaTest ? ShaderIdentifier(shadowAlphaTestHitGroupID) : ShaderIdentifier(shadowHitGroupID);
        }

        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(HitGroupRecord);
        sbInit.NumElements = hitGroupRecords.Size();
        sbInit.InitData = hitGroupRecords.Data();
        sbInit.ShaderTable = true;
        sbInit.Name = L"Hit Shader Table";
        rtHitTable.Initialize(sbInit);
    }

    DX12::Release(psoProps);

    // --- 新增代码 开始: 创建 Baking DXR PSO ---

    // 1. 使用 StateObjectBuilder 构建用于烘焙的 PSO
    StateObjectBuilder bakingBuilder;
    bakingBuilder.Init(12); // 子对象数量可以和 rtPSO 保持类似

    {
        // 使用我们新编译的 bakingLib
        D3D12_DXIL_LIBRARY_DESC dxilDesc = { };
        dxilDesc.DXILLibrary = bakingLib.ByteCode();
        bakingBuilder.AddSubObject(dxilDesc);
    }

    // 复用已有的命中组定义，因为命中物体后的物理逻辑是相同的
    {
        D3D12_HIT_GROUP_DESC hitDesc = { };
        hitDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitDesc.ClosestHitShaderImport = L"ClosestHitShader";
        hitDesc.HitGroupExport = L"HitGroup";
        bakingBuilder.AddSubObject(hitDesc);
    }
    {
        D3D12_HIT_GROUP_DESC hitDesc = { };
        hitDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitDesc.ClosestHitShaderImport = L"ClosestHitShader";
        hitDesc.AnyHitShaderImport = L"AnyHitShader";
        hitDesc.HitGroupExport = L"AlphaTestHitGroup";
        bakingBuilder.AddSubObject(hitDesc);
    }
    {
        D3D12_HIT_GROUP_DESC hitDesc = {};
        hitDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitDesc.ClosestHitShaderImport = L"ShadowHitShader";
        hitDesc.HitGroupExport = L"ShadowHitGroup";
        bakingBuilder.AddSubObject(hitDesc);
    }
    {
        D3D12_HIT_GROUP_DESC hitDesc = {};
        hitDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitDesc.ClosestHitShaderImport = L"ShadowHitShader";
        hitDesc.AnyHitShaderImport = L"ShadowAnyHitShader";
        hitDesc.HitGroupExport = L"ShadowAlphaTestHitGroup";
        bakingBuilder.AddSubObject(hitDesc);
    }
    
    // 复用已有的着色器配置 (Payload 大小等)
    {
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = { };
        shaderConfig.MaxAttributeSizeInBytes = 2 * sizeof(float);
        shaderConfig.MaxPayloadSizeInBytes = 4 * sizeof(float) + 4 * sizeof(uint32);
        bakingBuilder.AddSubObject(shaderConfig);
    }
    
    // 复用已有的全局根签名，因为它已经包含了所有我们需要的资源绑定
    {
        D3D12_GLOBAL_ROOT_SIGNATURE globalRSDesc = { };
        globalRSDesc.pGlobalRootSignature = rtRootSignature;
        bakingBuilder.AddSubObject(globalRSDesc);
    }

    // 复用已有的管线配置 (光线递归深度)
    {
        D3D12_RAYTRACING_PIPELINE_CONFIG configDesc = { };
        configDesc.MaxTraceRecursionDepth = AppSettings::MaxPathLengthSetting;
        bakingBuilder.AddSubObject(configDesc);
    }
    
    // 创建烘焙管线状态对象
    bakingPSO = bakingBuilder.CreateStateObject(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
    bakingPSO->SetName(L"Baking PSO");

    ID3D12StateObjectProperties* bakingPsoProps = nullptr;
    bakingPSO->QueryInterface(IID_PPV_ARGS(&bakingPsoProps));

    const void* bakeRayGenID = bakingPsoProps->GetShaderIdentifier(L"BakeRayGen");
    const void* bakingHitGroupID = bakingPsoProps->GetShaderIdentifier(L"HitGroup");
    const void* bakingAlphaTestHitGroupID = bakingPsoProps->GetShaderIdentifier(L"AlphaTestHitGroup");
    const void* bakingShadowHitGroupID = bakingPsoProps->GetShaderIdentifier(L"ShadowHitGroup");
    const void* bakingShadowAlphaTestHitGroupID = bakingPsoProps->GetShaderIdentifier(L"ShadowAlphaTestHitGroup");
    const void* bakingMissID = bakingPsoProps->GetShaderIdentifier(L"MissShader");
    const void* bakingShadowMissID = bakingPsoProps->GetShaderIdentifier(L"ShadowMissShader");

    DX12::Release(bakingPsoProps);

    // 3. 创建烘焙专用的光线生成着色器表 (RayGen Shader Table)
    {
        ShaderIdentifier bakeRayGenRecords[1] = { ShaderIdentifier(bakeRayGenID) };
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(ShaderIdentifier);
        sbInit.NumElements = ArraySize_(bakeRayGenRecords);
        sbInit.InitData = bakeRayGenRecords;
        sbInit.ShaderTable = true;
        sbInit.Name = L"Baking Ray Gen Shader Table";
        bakingRayGenTable.Initialize(sbInit);
    }

    // 4. 创建烘焙专用的未命中着色器表 (Miss Shader Table)
    {
        ShaderIdentifier missRecords[2] = { ShaderIdentifier(bakingMissID), ShaderIdentifier(bakingShadowMissID) };
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(ShaderIdentifier);
        sbInit.NumElements = ArraySize_(missRecords);
        sbInit.InitData = missRecords;
        sbInit.ShaderTable = true;
        sbInit.Name = L"Baking Miss Shader Table";
        bakingMissTable.Initialize(sbInit); // <-- 使用我们新声明的 bakingMissTable
    }

    // 5. 创建烘焙专用的命中组表 (Hit Group Table)
    {
        const uint32 numMeshes = uint32(currentModel->NumMeshes());
        Array<HitGroupRecord> hitGroupRecords(numMeshes * 2);
        for (uint64 i = 0; i < numMeshes; ++i)
        {
            const Mesh& mesh = currentModel->Meshes()[i];
            Assert_(mesh.NumMeshParts() == 1);
            const uint32 materialIdx = mesh.MeshParts()[0].MaterialIdx;
            const MeshMaterial& material = currentModel->Materials()[materialIdx];
            const bool alphaTest = material.Textures[uint32(MaterialTextures::Opacity)] != nullptr;
            
            hitGroupRecords[i * 2 + 0].ID = alphaTest ? ShaderIdentifier(bakingAlphaTestHitGroupID) : ShaderIdentifier(bakingHitGroupID);
            hitGroupRecords[i * 2 + 1].ID = alphaTest ? ShaderIdentifier(bakingShadowAlphaTestHitGroupID) : ShaderIdentifier(bakingShadowHitGroupID);
        }

        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(HitGroupRecord);
        sbInit.NumElements = hitGroupRecords.Size();
        sbInit.InitData = hitGroupRecords.Data();
        sbInit.ShaderTable = true;
        sbInit.Name = L"Baking Hit Shader Table";
        bakingHitTable.Initialize(sbInit); // <-- 使用我们新声明的 bakingHitTable
    }
    
}

void DXRPathTracer::Update(const Timer& timer)
{
    CPUProfileBlock profileBlock("Update");

    AppSettings::UpdateUI();

    MouseState mouseState = MouseState::GetMouseState(window);
    KeyboardState kbState = KeyboardState::GetKeyboardState(window);

    if(kbState.IsKeyDown(KeyboardState::Escape))
        window.Destroy();

    float CamMoveSpeed = 5.0f * timer.DeltaSecondsF();
    const float CamRotSpeed = 0.180f * timer.DeltaSecondsF();

    // Move the camera with keyboard input
    if(kbState.IsKeyDown(KeyboardState::LeftShift))
        CamMoveSpeed *= 0.25f;

    Float3 camPos = camera.Position();
    if(kbState.IsKeyDown(KeyboardState::W))
        camPos += camera.Forward() * CamMoveSpeed;
    else if (kbState.IsKeyDown(KeyboardState::S))
        camPos += camera.Back() * CamMoveSpeed;
    if(kbState.IsKeyDown(KeyboardState::A))
        camPos += camera.Left() * CamMoveSpeed;
    else if (kbState.IsKeyDown(KeyboardState::D))
        camPos += camera.Right() * CamMoveSpeed;
    if(kbState.IsKeyDown(KeyboardState::Q))
        camPos += camera.Up() * CamMoveSpeed;
    else if (kbState.IsKeyDown(KeyboardState::E))
        camPos += camera.Down() * CamMoveSpeed;
    camera.SetPosition(camPos);

    // Rotate the cameraFP with the mouse
    if(mouseState.RButton.Pressed && mouseState.IsOverWindow)
    {
        float xRot = camera.XRotation();
        float yRot = camera.YRotation();
        xRot += mouseState.DY * CamRotSpeed;
        yRot += mouseState.DX * CamRotSpeed;
        camera.SetXRotation(xRot);
        camera.SetYRotation(yRot);
    }

    UpdateLights();

    appViewMatrix = camera.ViewMatrix();

    // Toggle VSYNC
    swapChain.SetVSYNCEnabled(AppSettings::EnableVSync ? true : false);

    // Toggle stable power state
    if(AppSettings::StablePowerState != stablePowerState)
    {
        DX12::Device->SetStablePowerState(AppSettings::StablePowerState);
        stablePowerState = AppSettings::StablePowerState;
    }

    skyCache.Init(AppSettings::SunDirection, AppSettings::SunSize, AppSettings::GroundAlbedo, AppSettings::Turbidity, true);

    if(AppSettings::MSAAMode.Changed() || AppSettings::ClusterRasterizationMode.Changed())
    {
        DestroyPSOs();
        CreateRenderTargets();
        CreatePSOs();
    }

    if(AppSettings::CurrentScene.Changed() && currentModel != &sceneModels[uint64(AppSettings::CurrentScene)])
    {
        currentModel = &sceneModels[uint64(AppSettings::CurrentScene)];
        DestroyPSOs();
        InitializeScene();
        CreatePSOs();

        rtShouldRestartPathTrace = true;
    }

    const Setting* settingsToCheck[] =
    {
        &AppSettings::SqrtNumSamples,
        &AppSettings::MaxPathLength,
        &AppSettings::EnableAlbedoMaps,
        &AppSettings::EnableNormalMaps,
        &AppSettings::EnableDiffuse,
        &AppSettings::EnableSpecular,
        &AppSettings::EnableDirect,
        &AppSettings::EnableIndirect,
        &AppSettings::EnableIndirectSpecular,
        &AppSettings::EnableSky,
        &AppSettings::EnableSun,
        &AppSettings::RenderLights,
        &AppSettings::SunSize,
        &AppSettings::SunDirection,
        &AppSettings::Turbidity,
        &AppSettings::GroundAlbedo,
        &AppSettings::RoughnessScale,
        &AppSettings::MetallicScale,
        &AppSettings::EnableWhiteFurnaceMode,
        &AppSettings::MaxAnyHitPathLength,
        &AppSettings::AvoidCausticPaths,
        &AppSettings::ClampRoughness,
        &AppSettings::ApplyMultiscatteringEnergyCompensation
    };

    for(const Setting* setting : settingsToCheck)
    {
        if(setting->Changed())
            rtShouldRestartPathTrace = true;
    }

    if(AppSettings::AlwaysResetPathTrace)
        rtShouldRestartPathTrace = true;

    if(rtCurrCamera.Position() != camera.Position() || rtCurrCamera.Orientation() != camera.Orientation() || rtCurrCamera.ProjectionMatrix() != camera.ProjectionMatrix())
        rtShouldRestartPathTrace = true;

    rtCurrCamera = camera;

    if(AppSettings::EnableRayTracing && rtShouldRestartPathTrace)
    {
        rtCurrSampleIdx = 0;
        rtShouldRestartPathTrace = false;
    }
}

void DXRPathTracer::Render(const Timer& timer)
{
    if (m_uploadDenoisedDataRequested)
    {
        UploadDenoisedData();
        m_uploadDenoisedDataRequested = false; // 重置标志位
    }
    if (denoisingRequested)
    {
        DenoiseLightmap();
        denoisingRequested = false; // 重置标志位
    }
    if (medianDenoiseRequested)
    {
        RenderLightmapMedianPass();
        medianDenoiseRequested = false; // 重置标志位
    }

    if (isBaking)
    {
        RenderBakingPass();
    }

    if (uvVisualizationRequested)
    {
        if (currentModel && currentModel->GetLightmappedVertexCount() > 0)
        {
            VisualizeUVs(currentModel);
            textureToPreview = TextureToPreview::UVLayout; // 自动切换预览
        }
        uvVisualizationRequested = false;
}

    if(buildAccelStructure)
        BuildRTAccelerationStructure();
    else if(lastBuildAccelStructureFrame + DX12::RenderLatency == DX12::CurrentCPUFrame)
        WriteLog("Acceleration structure build time: %.2f ms", Profiler::GlobalProfiler.GPUProfileTiming("Build Acceleration Structure"));

    ID3D12GraphicsCommandList4* cmdList = DX12::CmdList;

    CPUProfileBlock cpuProfileBlock("Render");
    ProfileBlock gpuProfileBlock(cmdList, "Render Total");

    RenderTexture* finalRT = nullptr;

    if (spotLights.Size() > 0)
    {
        // Update the light constant buffer
        MapResult staging = DX12::AcquireTempBufferMem(spotLightBuffer.InternalBuffer.Size, 0);
        memcpy(staging.CPUAddress, spotLights.Data(), spotLights.MemorySize());
        uint8* matrixData = reinterpret_cast<uint8*>(staging.CPUAddress) + sizeof(SpotLight) * AppSettings::MaxSpotLights;
        memcpy(matrixData, meshRenderer.SpotLightShadowMatrices(), spotLights.Size() * sizeof(Float4x4));
        spotLightBuffer.QueueUpload(staging.Resource, staging.ResourceOffset, spotLightBuffer.InternalBuffer.Size, 0);
    }

    if (isFirstFrame)
    {

        // 2. 将资源状态切换为“可写”
        bakedLightMap.MakeWritable(cmdList);

        // 3. 定义清除颜色为纯黑
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        // 4. 执行清除操作
        cmdList->ClearRenderTargetView(bakedLightMap.RTV, clearColor, 0, nullptr);

        // 5. 将资源状态切换回“可读”，为本帧的spriteRenderer做准备
        bakedLightMap.MakeReadable(cmdList);

        // 6. 将标志设为false，确保此代码块不再执行
        isFirstFrame = false;
    }

    if(AppSettings::EnableRayTracing)
    {
        RenderRayTracing();

        finalRT = &rtTarget;
    }
    else
    {
        RenderClusters();

        if(AppSettings::EnableSun)
            meshRenderer.RenderSunShadowMap(cmdList, camera);

        if(AppSettings::RenderLights)
            meshRenderer.RenderSpotLightShadowMap(cmdList, camera);

        RenderForward();

        RenderResolve();

        finalRT = mainTarget.MSAASamples > 1 ? &resolveTarget : &mainTarget;
    }

    {
        ProfileBlock ppProfileBlock(cmdList, "Post Processing");
        postProcessor.Render(cmdList, *finalRT, swapChain.BackBuffer());
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { swapChain.BackBuffer().RTV };
    cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);

    DX12::SetViewport(cmdList, swapChain.Width(), swapChain.Height());

    RenderHUD(timer);
}

void DXRPathTracer::UpdateLights()
{
    const uint64 numSpotLights = Min<uint64>(spotLights.Size(), AppSettings::MaxLightClamp);

    // This is an additional scale factor that's needed to make sure that our polygonal bounding cone
    // fully encloses the actual cone representing the light's area of influence
    const float inRadius = std::cos(Pi / NumConeSides);
    const float scaleCorrection = 1.0f / inRadius;

    const Float4x4 viewMatrix = camera.ViewMatrix();
    const float nearClip = camera.NearClip();
    const float farClip = camera.FarClip();
    const float zRange = farClip - nearClip;
    const Float3 cameraPos = camera.Position();
    const uint64 numConeVerts = coneVertices.Size();

    // Come up with a bounding sphere that surrounds the near clipping plane. We'll test this sphere
    // for intersection with the spot light's bounding cone, and use that to over-estimate if the bounding
    // geometry will end up getting clipped by the camera's near clipping plane
    Float3 nearClipCenter = cameraPos + nearClip * camera.Forward();
    Float4x4 invViewProjection = Float4x4::Invert(camera.ViewProjectionMatrix());
    Float3 nearTopRight = Float3::Transform(Float3(1.0f, 1.0f, 0.0f), invViewProjection);
    float nearClipRadius = Float3::Length(nearTopRight - nearClipCenter);

    ClusterBounds* boundsData = spotLightBoundsBuffer.Map<ClusterBounds>();
    bool intersectsCamera[AppSettings::MaxSpotLights] = { };

    // Update the light bounds buffer
    for(uint64 spotLightIdx = 0; spotLightIdx < numSpotLights; ++spotLightIdx)
    {
        const SpotLight& spotLight = spotLights[spotLightIdx];
        const ModelSpotLight& srcSpotLight = currentModel->SpotLights()[spotLightIdx];
        ClusterBounds bounds;
        bounds.Position = spotLight.Position;
        bounds.Orientation = srcSpotLight.Orientation;
        bounds.Scale.x = bounds.Scale.y = std::tan(srcSpotLight.AngularAttenuation.y / 2.0f) * spotLight.Range * scaleCorrection;
        bounds.Scale.z = spotLight.Range;

        // Compute conservative Z bounds for the light based on vertices of the bounding geometry
        float minZ = FloatMax;
        float maxZ = -FloatMax;
        for(uint64 i = 0; i < numConeVerts; ++i)
        {
            Float3 coneVert = coneVertices[i] * bounds.Scale;
            coneVert = Float3::Transform(coneVert, bounds.Orientation);
            coneVert += bounds.Position;

            float vertZ = Float3::Transform(coneVert, viewMatrix).z;
            minZ = Min(minZ, vertZ);
            maxZ = Max(maxZ, vertZ);
        }

        minZ = Saturate((minZ - nearClip) / zRange);
        maxZ = Saturate((maxZ - nearClip) / zRange);

        bounds.ZBounds.x = uint32(minZ * AppSettings::NumZTiles);
        bounds.ZBounds.y = Min(uint32(maxZ * AppSettings::NumZTiles), uint32(AppSettings::NumZTiles - 1));

        // Estimate if the light's bounding geometry intersects with the camera's near clip plane
        boundsData[spotLightIdx] = bounds;
        intersectsCamera[spotLightIdx] = SphereConeIntersection(spotLight.Position, srcSpotLight.Direction, spotLight.Range,
                                                                srcSpotLight.AngularAttenuation.y, nearClipCenter, nearClipRadius);
    }

    numIntersectingSpotLights = 0;
    uint32* instanceData = spotLightInstanceBuffer.Map<uint32>();

    for(uint64 spotLightIdx = 0; spotLightIdx < numSpotLights; ++spotLightIdx)
        if(intersectsCamera[spotLightIdx])
            instanceData[numIntersectingSpotLights++] = uint32(spotLightIdx);

    uint64 offset = numIntersectingSpotLights;
    for(uint64 spotLightIdx = 0; spotLightIdx < numSpotLights; ++spotLightIdx)
        if(intersectsCamera[spotLightIdx] == false)
            instanceData[offset++] = uint32(spotLightIdx);
}

void DXRPathTracer::RenderClusters()
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker marker(cmdList, "Cluster Update");
    ProfileBlock profileBlock(cmdList, "Cluster Update");

    spotLightClusterBuffer.MakeWritable(cmdList);

    {
        // Clear spot light clusters
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { spotLightClusterBuffer.UAV };
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = DX12::TempDescriptorTable(cpuDescriptors, ArraySize_(cpuDescriptors));

        uint32 values[4] = { };
        cmdList->ClearUnorderedAccessViewUint(gpuHandle, cpuDescriptors[0], spotLightClusterBuffer.InternalBuffer.Resource, values, 0, nullptr);
    }

    ClusterConstants clusterConstants;
    clusterConstants.ViewProjection = camera.ViewProjectionMatrix();
    clusterConstants.InvProjection = Float4x4::Invert(camera.ProjectionMatrix());
    clusterConstants.NearClip = camera.NearClip();
    clusterConstants.FarClip = camera.FarClip();
    clusterConstants.InvClipRange = 1.0f / (camera.FarClip() - camera.NearClip());
    clusterConstants.NumXTiles = uint32(AppSettings::NumXTiles);
    clusterConstants.NumYTiles = uint32(AppSettings::NumYTiles);
    clusterConstants.NumXYTiles = uint32(AppSettings::NumXTiles * AppSettings::NumYTiles);
    clusterConstants.InstanceOffset = 0;
    clusterConstants.NumLights = Min<uint32>(uint32(spotLights.Size()), AppSettings::MaxLightClamp);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { clusterMSAATarget.RTV };
    ClusterRasterizationModes rastMode = AppSettings::ClusterRasterizationMode;
    if(rastMode == ClusterRasterizationModes::MSAA4x || rastMode == ClusterRasterizationModes::MSAA8x)
        cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);
    else
        cmdList->OMSetRenderTargets(0, nullptr, false, nullptr);

    DX12::SetViewport(cmdList, AppSettings::NumXTiles, AppSettings::NumYTiles);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmdList->SetGraphicsRootSignature(clusterRS);

    DX12::BindGlobalSRVDescriptorTable(cmdList, ClusterParams_StandardDescriptors, CmdListMode::Graphics);

    if(AppSettings::RenderLights)
    {
        // Update light clusters
        spotLightClusterBuffer.UAVBarrier(cmdList);

        D3D12_INDEX_BUFFER_VIEW ibView = spotLightClusterIdxBuffer.IBView();
        cmdList->IASetIndexBuffer(&ibView);

        clusterConstants.ElementsPerCluster = uint32(AppSettings::SpotLightElementsPerCluster);
        clusterConstants.InstanceOffset = 0;
        clusterConstants.BoundsBufferIdx = spotLightBoundsBuffer.SRV;
        clusterConstants.VertexBufferIdx = spotLightClusterVtxBuffer.SRV;
        clusterConstants.InstanceBufferIdx = spotLightInstanceBuffer.SRV;
        DX12::BindTempConstantBuffer(cmdList, clusterConstants, ClusterParams_CBuffer, CmdListMode::Graphics);

        AppSettings::BindCBufferGfx(cmdList, ClusterParams_AppSettings);

        D3D12_CPU_DESCRIPTOR_HANDLE uavs[] = { spotLightClusterBuffer.UAV };
        DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), ClusterParams_UAVDescriptors, CmdListMode::Graphics);

        const uint64 numLightsToRender = Min<uint64>(spotLights.Size(), AppSettings::MaxLightClamp);
        Assert_(numIntersectingSpotLights <= numLightsToRender);
        const uint64 numNonIntersecting = numLightsToRender - numIntersectingSpotLights;

        // 绘制与摄像机相交的灯光 (背面)
        if (numIntersectingSpotLights > 0)
        {
            cmdList->SetPipelineState(clusterIntersectingPSO);
            cmdList->DrawIndexedInstanced(uint32(spotLightClusterIdxBuffer.NumElements), uint32(numIntersectingSpotLights), 0, 0, 0);
        }

        // 绘制所有其他灯光 (先背面，后正面)
        if (numNonIntersecting > 0)
        {
            // 设置这些灯光的偏移量
            clusterConstants.InstanceOffset = uint32(numIntersectingSpotLights);
            DX12::BindTempConstantBuffer(cmdList, clusterConstants, ClusterParams_CBuffer, CmdListMode::Graphics);

            // 绘制背面
            cmdList->SetPipelineState(clusterBackFacePSO);
            cmdList->DrawIndexedInstanced(uint32(spotLightClusterIdxBuffer.NumElements), uint32(numNonIntersecting), 0, 0, 0);

            spotLightClusterBuffer.UAVBarrier(cmdList);

            // 绘制正面
            cmdList->SetPipelineState(clusterFrontFacePSO);
            cmdList->DrawIndexedInstanced(uint32(spotLightClusterIdxBuffer.NumElements), uint32(numNonIntersecting), 0, 0, 0);
        }
    }

    // Sync
    spotLightClusterBuffer.MakeReadable(cmdList);
}

void DXRPathTracer::RenderForward()
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker marker(cmdList, "Forward rendering");

    {
        // Transition render targets back to a writable state
        D3D12_RESOURCE_BARRIER barriers[1] = { };
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[0].Transition.pResource = mainTarget.Resource();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.Subresource = 0;

        cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { mainTarget.RTV };
    cmdList->OMSetRenderTargets(1, rtvHandles, false, &depthBuffer.DSV);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cmdList->ClearRenderTargetView(rtvHandles[0], clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(depthBuffer.DSV, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    DX12::SetViewport(cmdList, mainTarget.Width(), mainTarget.Height());

    {
        ProfileBlock profileBlock(cmdList, "Forward Rendering Pass");

        // Render the main forward pass
        MainPassData mainPassData;
        mainPassData.SkyCache = &skyCache;
        mainPassData.SpotLightBuffer = &spotLightBuffer;
        mainPassData.SpotLightClusterBuffer = &spotLightClusterBuffer;
        mainPassData.BakedLightMap = useDenoisedLightmap ? &denoisedLightMap : &bakedLightMap;
        meshRenderer.RenderMainPass(cmdList, camera, mainPassData);

        cmdList->OMSetRenderTargets(1, rtvHandles, false, &depthBuffer.DSV);

        // Render the sky
        skybox.RenderSky(cmdList, camera.ViewMatrix(), camera.ProjectionMatrix(), skyCache, true);

        {
            // Make our targets readable again, which will force a sync point.
            D3D12_RESOURCE_BARRIER barriers[1] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[0].Transition.pResource = mainTarget.Resource();
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[0].Transition.Subresource = 0;

            cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
        }
    }
}

// Performs MSAA resolve with a full-screen pixel shader
void DXRPathTracer::RenderResolve()
{
    if(AppSettings::MSAAMode == MSAAModes::MSAANone)
        return;

    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker pixMarker(cmdList, "MSAA Resolve");
    ProfileBlock profileBlock(cmdList, "MSAA Resolve");

    resolveTarget.MakeWritable(cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[1] = { resolveTarget.RTV };
    cmdList->OMSetRenderTargets(ArraySize_(rtvs), rtvs, false, nullptr);
    DX12::SetViewport(cmdList, resolveTarget.Width(), resolveTarget.Height());

    cmdList->SetGraphicsRootSignature(resolveRootSignature);
    cmdList->SetPipelineState(resolvePSO);

    DX12::BindGlobalSRVDescriptorTable(cmdList, ResolveParams_StandardDescriptors, CmdListMode::Graphics);

    cmdList->SetGraphicsRoot32BitConstant(ResolveParams_Constants, uint32(mainTarget.Width()), 0);
    cmdList->SetGraphicsRoot32BitConstant(ResolveParams_Constants, uint32(mainTarget.Height()), 1);
    cmdList->SetGraphicsRoot32BitConstant(ResolveParams_Constants, mainTarget.SRV(), 2);

    AppSettings::BindCBufferGfx(cmdList, ResolveParams_AppSettings);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetIndexBuffer(nullptr);
    cmdList->IASetVertexBuffers(0, 0, nullptr);

    cmdList->DrawInstanced(3, 1, 0, 0);

    resolveTarget.MakeReadable(cmdList);
}

void DXRPathTracer::RenderSurfaceMap()
{
    PIXMarker marker(DX12::CmdList, "Render Surface Map");
    ProfileBlock profileBlock(DX12::CmdList, "Render Surface Map");

    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    if (currentModel == nullptr || currentModel->GetLightmappedVertexCount() == 0)
        return;

    // 1. 将两个资源都切换到渲染目标状态
    surfaceMap.MakeWritable(cmdList);
    surfaceMapNormal.MakeWritable(cmdList);
    surfaceMapAlbedo.MakeWritable(cmdList);

    // 2. 设置三个渲染目标
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[3] = { surfaceMap.RTV, surfaceMapNormal.RTV, surfaceMapAlbedo.RTV };
    cmdList->OMSetRenderTargets(3, rtvHandles, false, nullptr);

    // 3. 分别清除三个目标
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cmdList->ClearRenderTargetView(rtvHandles[0], clearColor, 0, nullptr);
    cmdList->ClearRenderTargetView(rtvHandles[1], clearColor, 0, nullptr);
    cmdList->ClearRenderTargetView(rtvHandles[2], clearColor, 0, nullptr);

    // 4. 设置视口和渲染状态 (这部分不变)
    DX12::SetViewport(cmdList, surfaceMap.Width(), surfaceMap.Height());
    cmdList->SetPipelineState(surfaceMapPSO);
    cmdList->SetGraphicsRootSignature(surfaceMapRS);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 5. 绑定几何体数据 (这部分不变)
    D3D12_VERTEX_BUFFER_VIEW vbView = currentModel->GetLightmappedVertexBuffer().VBView();
    D3D12_INDEX_BUFFER_VIEW ibView = currentModel->GetLightmappedIndexBuffer().IBView();
    cmdList->IASetVertexBuffers(0, 1, &vbView);
    cmdList->IASetIndexBuffer(&ibView);

    // 6. 绘制网格 (这部分不变)
    const Array<Mesh>& meshesToDraw = currentModel->GetLightmappedMeshes();
    for (const Mesh& mesh : meshesToDraw)
    {
        cmdList->DrawIndexedInstanced(mesh.NumIndices(), 1, mesh.IndexOffset(), mesh.VertexOffset(), 0);
    }

    // 7. 将三个资源都切换回可读状态
    surfaceMap.MakeReadable(cmdList);
    surfaceMapNormal.MakeReadable(cmdList);
    surfaceMapAlbedo.MakeReadable(cmdList);
}

void DXRPathTracer::RenderBakingPass_Progressive()
{
    PIXMarker marker(DX12::CmdList, "Baking Pass - Progressive DXR");
    ProfileBlock profileBlock(DX12::CmdList, "Baking Pass - Progressive DXR");

    ID3D12GraphicsCommandList4* cmdList = DX12::CmdList;

    if (currentModel == nullptr)
        return;

    // 1. 绑定根签名和全局资源
    cmdList->SetComputeRootSignature(rtRootSignature);
    DX12::BindGlobalSRVDescriptorTable(cmdList, RTParams_StandardDescriptors, CmdListMode::Compute);
    cmdList->SetComputeRootShaderResourceView(RTParams_SceneDescriptor, rtTopLevelAccelStructure.GPUAddress);
    
    // 2. 绑定UAV资源 (累加缓冲和最终光照贴图)
    // 你的根签名定义了UAV表有4个描述符。
    // 我们将 u0 绑定到 accumulationBuffer，u1 绑定到 bakedLightMap。
    D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] = {};
    uavs[0] = accumulationBuffer.UAV;
    uavs[1] = bakedLightMap.UAV;
    // 用最后一个有效的UAV填充剩余的槽位，以满足根签名要求
    uavs[2] = bakedLightMap.UAV;
    uavs[3] = bakedLightMap.UAV;
    DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), RTParams_UAVDescriptor, CmdListMode::Compute);

    // 3. 绑定常量缓冲
    // --- 修改开始: 完整地填充 RayTraceConstants ---
    RayTraceConstants rtConstants = {};
    // a. 填充光追/路径追踪所依赖的通用常量
    rtConstants.InvViewProjection = Float4x4::Invert(camera.ViewProjectionMatrix()); // 虽然烘焙不依赖相机，但给一个有效值更安全
    rtConstants.SunDirectionWS = AppSettings::SunDirection;
    rtConstants.SunIrradiance = skyCache.SunIrradiance;
    rtConstants.CosSunAngularRadius = std::cos(DegToRad(AppSettings::SunSize));
    rtConstants.SinSunAngularRadius = std::sin(DegToRad(AppSettings::SunSize));
    rtConstants.SunRenderColor = skyCache.SunRenderColor;
    rtConstants.CameraPosWS = camera.Position();
    
    // b. 填充对采样函数至关重要的常量
    rtConstants.CurrSampleIdx = bakingSampleIndex; // 使用烘焙的样本索引
    rtConstants.TotalNumPixels = LightMapResolution * LightMapResolution; // 使用光照贴图的总像素数

    // c. 填充几何体、材质等资源索引
    rtConstants.VtxBufferIdx = currentModel->VertexBuffer().SRV;
    rtConstants.IdxBufferIdx = currentModel->IndexBuffer().SRV;
    rtConstants.GeometryInfoBufferIdx = rtGeoInfoBuffer.SRV;
    rtConstants.MaterialBufferIdx = meshRenderer.MaterialBuffer().SRV;
    rtConstants.SkyTextureIdx = skyCache.CubeMap.SRV;
    rtConstants.NumLights = Min<uint32>(uint32(spotLights.Size()), AppSettings::MaxLightClamp);
    
    // 绑定这个完整的常量缓冲到 b0
    DX12::BindTempConstantBuffer(cmdList, rtConstants, RTParams_CBuffer, CmdListMode::Compute);
    // --- 修改结束 ---

    // b. 绑定烘焙专用常量
    struct BakingConstants
    {
        uint32 SampleIndex;
        uint32 SurfaceMapPositionIdx; // <-- 将原来的 SurfaceMapIdx 重命名
        uint32 SurfaceMapNormalIdx;   // <-- 新增这一行
        uint32 Padding1;
    };

    BakingConstants bakingConstants = {};
    bakingConstants.SampleIndex = bakingSampleIndex;
    bakingConstants.SurfaceMapPositionIdx = surfaceMap.SRV(); // <-- 修改点
    bakingConstants.SurfaceMapNormalIdx = surfaceMapNormal.SRV(); // <-- 新增这一行
    DX12::BindTempConstantBuffer(cmdList, bakingConstants, RTParams_BakingCBuffer, CmdListMode::Compute);

    
    // c. 绑定其他常量
    spotLightBuffer.SetAsComputeRootParameter(cmdList, RTParams_LightCBuffer);
    AppSettings::BindCBufferCompute(cmdList, RTParams_AppSettings);

    // 4. 将UAV资源切换到可写状态
    accumulationBuffer.MakeWritableUAV(cmdList);
    bakedLightMap.MakeWritableUAV(cmdList);

    // 5. 设置DXR管线状态
    cmdList->SetPipelineState1(bakingPSO);

    // 6. 配置并分发光线
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord = bakingRayGenTable.ShaderRecord(0);
    dispatchDesc.MissShaderTable = bakingMissTable.ShaderTable();
    dispatchDesc.HitGroupTable = bakingHitTable.ShaderTable();
    dispatchDesc.Width = LightMapResolution;
    dispatchDesc.Height = LightMapResolution;
    dispatchDesc.Depth = 1;

    cmdList->DispatchRays(&dispatchDesc);


    // 7. 将UAV切换回可读状态，以便UI显示
    accumulationBuffer.MakeReadableUAV(cmdList);
    bakedLightMap.MakeReadableUAV(cmdList);
}

void DXRPathTracer::RenderBakingPass()
{
    PIXMarker marker(DX12::CmdList, "Baking Pass");

    // 烘焙流程的总指挥
    if (isBaking)
    {
        // 如果是第一帧，执行一次性的预烘焙
        if (bakingSampleIndex == 0)
        {
            // 1. 清空累加器和输出结果
            accumulationBuffer.MakeWritable(DX12::CmdList);
            bakedLightMap.MakeWritable(DX12::CmdList);
            const float clearColor[] = { 0, 0, 0, 0 };
            DX12::CmdList->ClearRenderTargetView(accumulationBuffer.RTV, clearColor, 0, nullptr);
            DX12::CmdList->ClearRenderTargetView(bakedLightMap.RTV, clearColor, 0, nullptr);
            accumulationBuffer.MakeReadable(DX12::CmdList);
            bakedLightMap.MakeReadable(DX12::CmdList);

            // 2. 执行表面映射图生成
            RenderSurfaceMap();
        }

        // 3. 执行一帧渐进式烘焙
        RenderBakingPass_Progressive();

        // 4. 增加采样计数
        bakingSampleIndex++;
    }
}

void DXRPathTracer::RenderRayTracing()
{
    // Don't keep tracing rays if we've hit our maximum per-pixel sample count
    if(rtCurrSampleIdx >= uint32(AppSettings::SqrtNumSamples * AppSettings::SqrtNumSamples))
        return;

    ID3D12GraphicsCommandList4* cmdList = DX12::CmdList;
    cmdList->SetComputeRootSignature(rtRootSignature);

    DX12::BindGlobalSRVDescriptorTable(cmdList, RTParams_StandardDescriptors, CmdListMode::Compute);

    cmdList->SetComputeRootShaderResourceView(RTParams_SceneDescriptor, rtTopLevelAccelStructure.GPUAddress);

    // 3. 构建完整的描述符数组
    D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] = {};
    uavs[0] = rtTarget.UAV;
    uavs[1] = rtTarget.UAV;
    uavs[2] = rtTarget.UAV;
    uavs[3] = rtTarget.UAV;

    // 4. 绑定这个包含4个描述符的完整表
    DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), RTParams_UAVDescriptor, CmdListMode::Compute);


    RayTraceConstants rtConstants;
    rtConstants.InvViewProjection = Float4x4::Invert(camera.ViewProjectionMatrix());

    rtConstants.SunDirectionWS = AppSettings::SunDirection;
    rtConstants.SunIrradiance = skyCache.SunIrradiance;
    rtConstants.CosSunAngularRadius = std::cos(DegToRad(AppSettings::SunSize));
    rtConstants.SinSunAngularRadius = std::sin(DegToRad(AppSettings::SunSize));
    rtConstants.SunRenderColor = skyCache.SunRenderColor;
    rtConstants.CameraPosWS = camera.Position();
    rtConstants.CurrSampleIdx = rtCurrSampleIdx;
    rtConstants.TotalNumPixels = uint32(rtTarget.Width()) * uint32(rtTarget.Height());

    rtConstants.VtxBufferIdx = currentModel->VertexBuffer().SRV;
    rtConstants.IdxBufferIdx = currentModel->IndexBuffer().SRV;
    rtConstants.GeometryInfoBufferIdx = rtGeoInfoBuffer.SRV;
    rtConstants.MaterialBufferIdx = meshRenderer.MaterialBuffer().SRV;
    rtConstants.SkyTextureIdx = skyCache.CubeMap.SRV;
    rtConstants.NumLights = Min<uint32>(uint32(spotLights.Size()), AppSettings::MaxLightClamp);

    DX12::BindTempConstantBuffer(cmdList, rtConstants, RTParams_CBuffer, CmdListMode::Compute);

    spotLightBuffer.SetAsComputeRootParameter(cmdList, RTParams_LightCBuffer);

    AppSettings::BindCBufferCompute(cmdList, RTParams_AppSettings);

    rtTarget.MakeWritableUAV(cmdList);

    cmdList->SetPipelineState1(rtPSO);

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.HitGroupTable = rtHitTable.ShaderTable();
    dispatchDesc.MissShaderTable = rtMissTable.ShaderTable();
    dispatchDesc.RayGenerationShaderRecord = rtRayGenTable.ShaderRecord(0);
    dispatchDesc.Width = uint32(rtTarget.Width());
    dispatchDesc.Height = uint32(rtTarget.Height());
    dispatchDesc.Depth = 1;

    DX12::CmdList->DispatchRays(&dispatchDesc);

    rtTarget.MakeReadableUAV(cmdList);

    rtCurrSampleIdx += 1;
}

// 在 DXRPathTracer.cpp 中，找到空的 RenderLightmapMedianPass 函数
void DXRPathTracer::RenderLightmapMedianPass()
{
    PIXMarker marker(DX12::CmdList, "GPU Median Filter Pass");
    ProfileBlock profileBlock(DX12::CmdList, "GPU Median Filter");

    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    // 为计算通道转换资源状态
    // 输入的 bakedLightMap 需要能被着色器读取
    // 输出的 denoisedLightMap 需要作为UAV可写
    bakedLightMap.MakeReadable(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    denoisedLightMap.MakeWritableUAV(cmdList);

    // 设置计算管线状态
    cmdList->SetComputeRootSignature(medianDenoiseRS);
    cmdList->SetPipelineState(medianDenoisePSO);

    // 绑定资源
    // 根参数 0: 常量
    cmdList->SetComputeRoot32BitConstant(0, 1, 0); // 滤波半径 = 1，对应3x3核心

    // 根参数 1: 输入SRV
    D3D12_CPU_DESCRIPTOR_HANDLE srvs[] = { bakedLightMap.SRV() };
    DX12::BindTempDescriptorTable(cmdList, srvs, ArraySize_(srvs), 1, CmdListMode::Compute);

    // 根参数 2: 输出UAV
    D3D12_CPU_DESCRIPTOR_HANDLE uavs[] = { denoisedLightMap.UAV };
    DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), 2, CmdListMode::Compute);
    
    // 分发计算着色器
    const uint32 threadGroupSize = 8; // 与HLSL中的 [numthreads(8, 8, 1)] 对应
    const uint32 dispatchX = (LightMapResolution + threadGroupSize - 1) / threadGroupSize;
    const uint32 dispatchY = (LightMapResolution + threadGroupSize - 1) / threadGroupSize;
    cmdList->Dispatch(dispatchX, dispatchY, 1);

    // 将降噪后的贴图转换回可读状态，以便用于渲染和显示
    denoisedLightMap.MakeReadable(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 设置标志位，以使用并预览新降噪的光照贴图
    useDenoisedLightmap = true;
    textureToPreview = TextureToPreview::DenoisedLightmap;
}

void DXRPathTracer::RenderHUD(const Timer& timer)
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;
    PIXMarker pixMarker(cmdList, "HUD Pass");
    
    // Draw the progress bar
    const uint32 totalNumSamples = uint32(AppSettings::SqrtNumSamples * AppSettings::SqrtNumSamples);
    if(rtCurrSampleIdx < totalNumSamples && AppSettings::ShowProgressBar)
    {
        float width = float(swapChain.Width());
        float height = float(swapChain.Height());

        const uint32 barEmptyColor = ImColor(0.0f, 0.0f, 0.0f, 1.0f);
        const uint32 barFilledColor = ImColor(1.0f, 0.0f, 0.0f, 1.0f);
        const uint32 barOutlineColor = ImColor(1.0f, 1.0f, 1.0f, 1.0f);
        const uint32 textColor = ImColor(1.0f, 1.0f, 1.0f, 1.0f);

        const float barPercentage = 0.75f;
        const float barHeight = 75.0f;
        Float2 barStart = Float2(width * (1.0f - barPercentage) * 0.5f, height - 200.0f);
        Float2 barSize = Float2(width * barPercentage, barHeight);
        Float2 barEnd = barStart + barSize;

        Float2 windowStart = barStart - 8.0f;
        Float2 windowSize = barSize + 16.0f;
        Float2 windowEnd = windowStart + windowSize;

        ImGui::SetNextWindowPos(ToImVec2(windowStart), ImGuiSetCond_Always);
        ImGui::SetNextWindowSize(ToImVec2(windowSize), ImGuiSetCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("HUD Window", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        const float progress = float(rtCurrSampleIdx) / totalNumSamples;

        drawList->AddRectFilled(ToImVec2(barStart), ToImVec2(barEnd), barEmptyColor);
        drawList->AddRectFilled(ToImVec2(barStart), ImVec2(barStart.x + barSize.x * progress, barEnd.y), barFilledColor);
        drawList->AddRect(ToImVec2(barStart), ToImVec2(barEnd), barOutlineColor);

        const uint64 raysPerFrame = rtTarget.Width() * rtTarget.Height() * (1 + (AppSettings::MaxPathLength - 1) * 2);
        const double mRaysPerSecond = raysPerFrame * (1.0 / timer.DeltaSecondsF()) / 1000000.0;

        std::string progressText = MakeString("Progress: %.2f%% (%.2f Mrays per second)", progress * 100.0f, mRaysPerSecond);
        Float2 progressTextSize = ToFloat2(ImGui::CalcTextSize(progressText.c_str()));
        Float2 progressTextPos = barStart + (barSize * 0.5f) - (progressTextSize * 0.5f);
        drawList->AddText(ToImVec2(progressTextPos), textColor, progressText.c_str());

        ImGui::PopStyleVar();

        ImGui::End();
    }

    if (showLightmapWindow)
    {
        // 设置窗口的初始大小
        ImGui::SetNextWindowSize(ImVec2(512.0f, 600.0f), ImGuiCond_FirstUseEver);

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

        if (ImGui::Begin("Baked Lightmap", &showLightmapWindow))
        {
            // 1. 获取内容区域的精确位置和大小
            ImVec2 contentPos = ImGui::GetCursorScreenPos();
            ImVec2 contentSize = ImGui::GetContentRegionAvail();

            // 2. 计算保持正方形居中显示的最终尺寸和位置
            float aspect = bakedLightMap.Width() / float(bakedLightMap.Height()); // 我们的纹理是正方形，所以 aspect = 1.0
            float finalWidth, finalHeight, offsetX, offsetY;

            if (contentSize.x / contentSize.y >= aspect)
            {
                // 内容区域比纹理更“宽”，以高为基准
                finalHeight = contentSize.y;
                finalWidth = finalHeight * aspect;
                offsetY = 0;
                offsetX = (contentSize.x - finalWidth) * 0.5f;
            }
            else
            {
                // 内容区域比纹理更“高”，以宽为基准
                finalWidth = contentSize.x;
                finalHeight = finalWidth / aspect;
                offsetX = 0;
                offsetY = (contentSize.y - finalHeight) * 0.5f;
            }

            // 3. 更新下一帧 spriteRenderer 要使用的矩形
            lightmapWindowRect.x = contentPos.x + offsetX;
            lightmapWindowRect.y = contentPos.y + offsetY;
            lightmapWindowRect.z = finalWidth;
            lightmapWindowRect.w = finalHeight;

            // 在窗口里添加按钮
            if (isBaking)
            {
                if (ImGui::Button("Stop Baking"))
                    isBaking = false;
                ImGui::SameLine();

            }
            else
            {
                if (ImGui::Button("Start Baking"))
                {
                    isBaking = true;
                    bakingSampleIndex = 0;
                    useDenoisedLightmap = false;
                }
            }

            ImGui::SameLine(); // 让下一个按钮在同一行
            if (ImGui::Button("Visualize UVs"))
            {
                uvVisualizationRequested = true;
                textureToPreview = TextureToPreview::UVLayout;
            }

            ImGui::SameLine();
            if (ImGui::Button("OIDN Denoise"))
            {
                denoisingRequested = true;
            }

            ImGui::SameLine();
            if (ImGui::Button("GPU Median Denoise"))
            {
                medianDenoiseRequested = true;
            }

            const char* items[] = {
                "UV Layout",
                "Surface Map (World Pos)",
                "Surface Map (Normal)",
                "Surface Map (Albedo)",
                "Accumulation Buffer",
                "Final Lightmap"
            };
            // 将枚举转换为整数，用于 ImGui::Combo
            int currentItem = static_cast<int>(textureToPreview);
            if(ImGui::Combo("Preview", &currentItem, items, IM_ARRAYSIZE(items)))
            {
                textureToPreview = static_cast<TextureToPreview>(currentItem);
            }

            // 2. 使用 switch 语句来选择正确的纹理
            RenderTexture* previewTexture = nullptr;
            switch (textureToPreview)
            {
                case TextureToPreview::UVLayout:
                    previewTexture = &uvLayoutMap;
                    break;
                case TextureToPreview::SurfaceMap:
                    previewTexture = &surfaceMap;
                    break;
                case TextureToPreview::SurfaceMapNormal:
                    previewTexture = &surfaceMapNormal;
                    break;
                case TextureToPreview::SurfaceMapAlbedo:
                    previewTexture = &surfaceMapAlbedo;
                    break;
                case TextureToPreview::Accumulation:
                    previewTexture = &accumulationBuffer;
                    break;
                case TextureToPreview::DenoisedLightmap:
                    previewTexture = &denoisedLightMap;
                    break;
                case TextureToPreview::FinalLightmap:
                default:
                    previewTexture = &bakedLightMap;
                    break;
            }

            Float2 viewportSize;
            viewportSize.x = float(swapChain.Width());
            viewportSize.y = float(swapChain.Height());
            spriteRenderer.Begin(cmdList, viewportSize, SpriteFilterMode::Point, SpriteBlendMode::AlphaBlend);

            Float2 textPos = Float2(25.0f, 25.0f);
            std::wstring fpsText = MakeString(L"Frame Time: %.2fms (%u FPS)", 1000.0f / fps, fps);
            spriteRenderer.RenderText(cmdList, font, fpsText.c_str(), textPos, Float4(1.0f, 1.0f, 0.0f, 1.0f));
            // 我们直接使用 lightmapWindowRect 作为纹理的目标绘制区域
            // SpriteTransform 的 Scale 是基于纹理原始大小的乘数，所以我们用目标尺寸除以原始尺寸
            Float2 scale;
            scale.x = lightmapWindowRect.z / bakedLightMap.Width();
            scale.y = lightmapWindowRect.w / bakedLightMap.Height();

            SpriteTransform transform;
            transform.Position = Float2(lightmapWindowRect.x, lightmapWindowRect.y);
            transform.Scale = scale;

            spriteRenderer.Render(cmdList, &previewTexture->Texture, transform);
            spriteRenderer.End();
        }

        ImGui::End();
        ImGui::PopStyleColor();
    }
}

void DXRPathTracer::BuildRTAccelerationStructure()
{
    const FormattedBuffer& idxBuffer = currentModel->IndexBuffer();
    const StructuredBuffer& vtxBuffer = currentModel->VertexBuffer();
 
    const uint64 numMeshes = currentModel->NumMeshes();
    Array<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs(numMeshes);

    const uint32 numGeometries = uint32(geometryDescs.Size());
    Array<GeometryInfo> geoInfoBufferData(numGeometries);

    for(uint64 meshIdx = 0; meshIdx < numMeshes; ++meshIdx)
    {
        const Mesh& mesh = currentModel->Meshes()[meshIdx];
        Assert_(mesh.NumMeshParts() == 1);
        const uint32 materialIdx = mesh.MeshParts()[0].MaterialIdx;
        const MeshMaterial& material = currentModel->Materials()[materialIdx];
        const bool opaque = material.Textures[uint32(MaterialTextures::Opacity)] == nullptr;

        D3D12_RAYTRACING_GEOMETRY_DESC& geometryDesc = geometryDescs[meshIdx];
        geometryDesc = { };
        geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometryDesc.Triangles.IndexBuffer = idxBuffer.GPUAddress + mesh.IndexOffset() * idxBuffer.Stride;
        geometryDesc.Triangles.IndexCount = uint32(mesh.NumIndices());
        geometryDesc.Triangles.IndexFormat = idxBuffer.Format;
        geometryDesc.Triangles.Transform3x4 = 0;
        geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometryDesc.Triangles.VertexCount = uint32(mesh.NumVertices());
        geometryDesc.Triangles.VertexBuffer.StartAddress = vtxBuffer.GPUAddress + mesh.VertexOffset() * vtxBuffer.Stride;
        geometryDesc.Triangles.VertexBuffer.StrideInBytes = vtxBuffer.Stride;
        geometryDesc.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;

        GeometryInfo& geoInfo = geoInfoBufferData[meshIdx];
        geoInfo = { };
        geoInfo.VtxOffset = uint32(mesh.VertexOffset());
        geoInfo.IdxOffset = uint32(mesh.IndexOffset());
        geoInfo.MaterialIdx = mesh.MeshParts()[0].MaterialIdx;

        Assert_(mesh.NumMeshParts() == 1);
    }

    // Get required sizes for an acceleration structure
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo = {};

    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildInfoDesc = {};
        prebuildInfoDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        prebuildInfoDesc.Flags = buildFlags;
        prebuildInfoDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        prebuildInfoDesc.pGeometryDescs = nullptr;
        prebuildInfoDesc.NumDescs = 1;
        DX12::Device->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildInfoDesc, &topLevelPrebuildInfo);
    }

    Assert_(topLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomLevelPrebuildInfo = {};

    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildInfoDesc = {};
        prebuildInfoDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        prebuildInfoDesc.Flags = buildFlags;
        prebuildInfoDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        prebuildInfoDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        prebuildInfoDesc.pGeometryDescs = geometryDescs.Data();
        prebuildInfoDesc.NumDescs = numGeometries;
        DX12::Device->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildInfoDesc, &bottomLevelPrebuildInfo);
    }

    Assert_(bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

    RawBuffer scratchBuffer;

    {
        RawBufferInit bufferInit;
        bufferInit.NumElements = Max(topLevelPrebuildInfo.ScratchDataSizeInBytes, bottomLevelPrebuildInfo.ScratchDataSizeInBytes) / RawBuffer::Stride;
        bufferInit.CreateUAV = true;
        bufferInit.InitialState = D3D12_RESOURCE_STATE_COMMON;
        bufferInit.Name = L"RT Scratch Buffer";
        scratchBuffer.Initialize(bufferInit);
    }

    {
        RawBufferInit bufferInit;
        bufferInit.NumElements = bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes / RawBuffer::Stride;
        bufferInit.CreateUAV = true;
        bufferInit.InitialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        bufferInit.Name = L"RT Bottom Level Accel Structure";
        rtBottomLevelAccelStructure.Initialize(bufferInit);
    }

    {
        RawBufferInit bufferInit;
        bufferInit.NumElements = topLevelPrebuildInfo.ResultDataMaxSizeInBytes / RawBuffer::Stride;
        bufferInit.CreateUAV = true;
        bufferInit.InitialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        bufferInit.Name = L"RT Top Level Accel Structure";
        rtTopLevelAccelStructure.Initialize(bufferInit);
    }

    // Create an instance desc for the bottom-level acceleration structure.
    D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
    instanceDesc.Transform[0][0] = instanceDesc.Transform[1][1] = instanceDesc.Transform[2][2] = 1.0f;
    instanceDesc.InstanceMask = 1;
    instanceDesc.AccelerationStructure = rtBottomLevelAccelStructure.GPUAddress;

    TempBuffer instanceBuffer = DX12::TempStructuredBuffer(1, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), false);
    memcpy(instanceBuffer.CPUAddress, &instanceDesc, sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

    // Bottom Level Acceleration Structure desc
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomLevelBuildDesc = {};
    {
        bottomLevelBuildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bottomLevelBuildDesc.Inputs.Flags = buildFlags;
        bottomLevelBuildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottomLevelBuildDesc.Inputs.NumDescs = numGeometries;
        bottomLevelBuildDesc.Inputs.pGeometryDescs = geometryDescs.Data();
        bottomLevelBuildDesc.ScratchAccelerationStructureData = scratchBuffer.GPUAddress;
        bottomLevelBuildDesc.DestAccelerationStructureData = rtBottomLevelAccelStructure.GPUAddress;
    }

    // Top Level Acceleration Structure desc
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc = bottomLevelBuildDesc;
    {
        topLevelBuildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        topLevelBuildDesc.Inputs.NumDescs = 1;
        topLevelBuildDesc.Inputs.pGeometryDescs = nullptr;
        topLevelBuildDesc.Inputs.InstanceDescs = instanceBuffer.GPUAddress;
        topLevelBuildDesc.DestAccelerationStructureData = rtTopLevelAccelStructure.GPUAddress;;
        topLevelBuildDesc.ScratchAccelerationStructureData = scratchBuffer.GPUAddress;
    }

    {
        ProfileBlock profileBlock(DX12::CmdList, "Build Acceleration Structure");

        DX12::CmdList->BuildRaytracingAccelerationStructure(&bottomLevelBuildDesc, 0, nullptr);
        rtBottomLevelAccelStructure.UAVBarrier(DX12::CmdList);

        DX12::CmdList->BuildRaytracingAccelerationStructure(&topLevelBuildDesc, 0, nullptr);
        rtTopLevelAccelStructure.UAVBarrier(DX12::CmdList);
    }

    scratchBuffer.Shutdown();

    {
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(GeometryInfo);
        sbInit.NumElements = numGeometries;
        sbInit.Name = L"Geometry Info Buffer";
        sbInit.InitData = geoInfoBufferData.Data();
        rtGeoInfoBuffer.Initialize(sbInit);
    }

    buildAccelStructure = false;
    lastBuildAccelStructureFrame = DX12::CurrentCPUFrame;
}


void DXRPathTracer::UploadDenoisedData()
{
    PIXMarker marker(DX12::CmdList, "Upload Denoised Lightmap");
    OutputDebugStringA("Uploading denoised data to GPU...\n");
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    D3D12_RESOURCE_DESC textureDesc = denoisedLightMap.Resource()->GetDesc();
    UINT64 uploadBufferSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    DX12::Device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &layout, nullptr, nullptr, &uploadBufferSize);
    const uint32_t width = static_cast<uint32_t>(textureDesc.Width);
    const uint32_t height = static_cast<uint32_t>(textureDesc.Height);

    ComPtr<ID3D12Resource> uploadBuffer;
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = uploadBufferSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    DXCall(DX12::Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer)));
    uploadBuffer->SetName(L"Temporary Lightmap Upload Buffer");

    uint8_t* mappedUploadData = nullptr;
    uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedUploadData));

    const uint8_t* srcData = reinterpret_cast<const uint8_t*>(m_denoisedCpuData.data());
    const uint32_t srcRowPitch = layout.Footprint.RowPitch;
    const uint32_t dstRowPitch = width * 4 * sizeof(float);

    for (uint32_t y = 0; y < height; ++y) {
        memcpy(mappedUploadData + y * srcRowPitch, srcData + y * dstRowPitch, dstRowPitch);
    }
    uploadBuffer->Unmap(0, nullptr);
    denoisedLightMap.Transition(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION uploadSrcLoc = {};
    uploadSrcLoc.pResource = uploadBuffer.Get();
    uploadSrcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    uploadSrcLoc.PlacedFootprint = layout;

    D3D12_TEXTURE_COPY_LOCATION finalDstLoc = {};
    finalDstLoc.pResource = denoisedLightMap.Resource();
    finalDstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    finalDstLoc.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&finalDstLoc, 0, 0, 0, &uploadSrcLoc, nullptr);
    useDenoisedLightmap = true;
    denoisedLightMap.Transition(cmdList, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* resourceToRelease = uploadBuffer.Detach();
    DX12::DeferredRelease(resourceToRelease);
    m_denoisedCpuData.clear();
    m_denoisedCpuData.shrink_to_fit();
}

void DXRPathTracer::DenoiseLightmap()
{
    PIXMarker marker(DX12::CmdList, "Denoise Lightmap with OIDN (CPU Stage)");
    OutputDebugStringA("Starting lightmap denoising process...\n");

    if (isBaking)
        isBaking = false;

    // --- 步骤 1-3: 从GPU复制到回读缓冲区 (这部分不变) ---
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;
    bakedLightMap.Transition(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    
    D3D12_RESOURCE_DESC textureDesc = bakedLightMap.Resource()->GetDesc();
    UINT64 readbackBufferSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    DX12::Device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &layout, nullptr, nullptr, &readbackBufferSize);
    m_lightmapReadbackBuffer.Initialize(readbackBufferSize);
    m_lightmapReadbackBuffer.Resource->SetName(L"Lightmap Readback Buffer");
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = bakedLightMap.Resource();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_lightmapReadbackBuffer.Resource;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = layout;
    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    bakedLightMap.Transition(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    DX12::FlushGPU();

    const uint8_t* mappedData = reinterpret_cast<const uint8_t*>(m_lightmapReadbackBuffer.Map());
    const uint32_t width = static_cast<uint32_t>(bakedLightMap.Width());
    const uint32_t height = static_cast<uint32_t>(bakedLightMap.Height());

    std::vector<float> noisyData(width * height * 4);
    const uint32_t srcRowPitch = layout.Footprint.RowPitch;
    const uint32_t dstRowPitch = width * 4 * sizeof(float);
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(noisyData.data() + (y * width * 4), mappedData + (y * srcRowPitch), dstRowPitch);
    }
    m_lightmapReadbackBuffer.Unmap();
    oidnDenoiser.Denoise(m_denoisedCpuData, noisyData, width, height);
    m_uploadDenoisedDataRequested = true;

    textureToPreview = TextureToPreview::DenoisedLightmap; 

    OutputDebugStringA("CPU denoising complete. Pending GPU upload for next frame.\n");
}

void EnableDebugLayerAndGBV()
{
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        // 1. 启用基础调试层
        // 这个会进行API参数验证，状态跟踪等
        debugController->EnableDebugLayer();
        OutputDebugStringA("D3D12 Debug Layer Enabled.\n");

        // 2. 尝试获取更高版本的调试接口以启用GPU-Based Validation
        ComPtr<ID3D12Debug1> debugController1;
        if (SUCCEEDED(debugController->QueryInterface(IID_PPV_ARGS(&debugController1))))
        {
            debugController1->SetEnableGPUBasedValidation(TRUE);
            OutputDebugStringA("GPU-Based Validation (GBV) Enabled.\n");
        }
    }
    else
    {
        OutputDebugStringA("Warning: Unable to enable D3D12 Debug Layer. Please install the Graphics Tools for Windows.\n");
    }
#endif
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    //EnableDebugLayerAndGBV(); 

    DXRPathTracer app(lpCmdLine);
    app.Run();

    return 0;
}