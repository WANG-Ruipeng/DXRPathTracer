//=================================================================================================
//
//  DXR Path Tracer
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#pragma once

#include <PCH.h>

#include <App.h>
#include <InterfacePointers.h>
#include <Input.h>
#include <Graphics/Camera.h>
#include <Graphics/Model.h>
#include <Graphics/Skybox.h>
#include <Graphics/GraphicsTypes.h>

#include <xatlas.h>

#include "PostProcessor.h"
#include "MeshRenderer.h"

using namespace SampleFramework12;

class DXRPathTracer : public App
{

protected:

    FirstPersonCamera camera;

    Skybox skybox;
    SkyCache skyCache;

    PostProcessor postProcessor;

    // Model
    Model sceneModels[uint64(Scenes::NumValues)];
    Model* currentModel = nullptr;
    MeshRenderer meshRenderer;

    RenderTexture mainTarget;
    RenderTexture resolveTarget;
    RenderTexture deferredMSAATarget;
    DepthBuffer depthBuffer;

    Array<SpotLight> spotLights;
    ConstantBuffer spotLightBuffer;
    StructuredBuffer spotLightBoundsBuffer;
    StructuredBuffer spotLightInstanceBuffer;
    RawBuffer spotLightClusterBuffer;
    uint64 numIntersectingSpotLights = 0;

    ID3D12RootSignature* clusterRS = nullptr;
    CompiledShaderPtr clusterVS;
    CompiledShaderPtr clusterFrontFacePS;
    CompiledShaderPtr clusterBackFacePS;
    CompiledShaderPtr clusterIntersectingPS;
    ID3D12PipelineState* clusterFrontFacePSO = nullptr;
    ID3D12PipelineState* clusterBackFacePSO = nullptr;
    ID3D12PipelineState* clusterIntersectingPSO = nullptr;
    RenderTexture clusterMSAATarget;

    StructuredBuffer spotLightClusterVtxBuffer;
    FormattedBuffer spotLightClusterIdxBuffer;
    Array<Float3> coneVertices;

    CompiledShaderPtr fullScreenTriVS;
    CompiledShaderPtr resolvePS[NumMSAAModes];
    ID3D12RootSignature* resolveRootSignature = nullptr;
    ID3D12PipelineState* resolvePSO = nullptr;

    bool32 stablePowerState = false;

    RenderTexture bakedLightMap;
    RenderTexture uvLayoutMap;

    RenderTexture surfaceMap;
    RenderTexture surfaceMapNormal;
    RenderTexture accumulationBuffer;

    // 烘焙管线状态
    bool isBaking = false;
    uint32 bakingSampleIndex = 0;
    enum class TextureToPreview { UVLayout, SurfaceMap, Accumulation, FinalLightmap };
    TextureToPreview textureToPreview = TextureToPreview::FinalLightmap;

    // 用于生成 SurfaceMap 的资源
    CompiledShaderPtr surfaceMapVS;
    CompiledShaderPtr surfaceMapPS;
    ID3D12RootSignature* surfaceMapRS = nullptr;
    ID3D12PipelineState* surfaceMapPSO = nullptr;

    // 用于烘焙的 DXR 资源
    CompiledShaderPtr bakingLib;
    ID3D12StateObject* bakingPSO = nullptr;
    StructuredBuffer bakingRayGenTable;
    StructuredBuffer bakingHitTable;
    StructuredBuffer bakingMissTable;

    bool showLightmapWindow = true;
    //bool bakeRequested = false;
    Float4 lightmapWindowRect = { 25.0f, 50.0f, 512.0f, 512.0f };
    bool isFirstFrame = true;
    bool uvVisualizationRequested = false;

    CompiledShaderPtr uvVisVS;
    CompiledShaderPtr uvVisPS;
    ID3D12RootSignature* uvVisRS = nullptr; // 可以复用一个简单的根签名，或者新建一个
    ID3D12PipelineState* uvVisPSO = nullptr;
    void VisualizeUVs(Model* model);

    // Ray tracing resources
    CompiledShaderPtr rayTraceLib;
    RenderTexture rtTarget;
    ID3D12RootSignature* rtRootSignature = nullptr;
    ID3D12StateObject* rtPSO = nullptr;
    bool buildAccelStructure = true;
    uint64 lastBuildAccelStructureFrame = uint64(-1);
    RawBuffer rtBottomLevelAccelStructure;
    RawBuffer rtTopLevelAccelStructure;
    StructuredBuffer rtRayGenTable;
    StructuredBuffer rtHitTable;
    StructuredBuffer rtMissTable;
    StructuredBuffer rtGeoInfoBuffer;
    FirstPersonCamera rtCurrCamera;
    bool rtShouldRestartPathTrace = false;
    uint32 rtCurrSampleIdx = 0;

    virtual void Initialize() override;
    virtual void Shutdown() override;

    virtual void Render(const Timer& timer) override;
    virtual void Update(const Timer& timer) override;

    virtual void BeforeReset() override;
    virtual void AfterReset() override;

    virtual void CreatePSOs() override;
    virtual void DestroyPSOs() override;

    void CreateRenderTargets();
    void InitializeScene();

    void InitRayTracing();
    void CreateRayTracingPSOs();

    void UpdateLights();

    void RenderClusters();
    void RenderForward();
    void RenderResolve();
    void RenderRayTracing();
    void RenderHUD(const Timer& timer);

    void BuildRTAccelerationStructure();

    void RenderBakingPass();
    void RenderSurfaceMap();
    void RenderBakingPass_Progressive();

    D3D12_CPU_DESCRIPTOR_HANDLE g_NullUAV;

public:
    DXRPathTracer(const wchar* cmdLine);
};
