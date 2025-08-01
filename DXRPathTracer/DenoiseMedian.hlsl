// DenoiseMedian.hlsl
#include <DescriptorTables.hlsl>
#include <Constants.hlsl>
#include <Quaternion.hlsl>
#include <BRDF.hlsl>
#include <RayTracing.hlsl>
#include <Sampling.hlsl>

#include "SharedTypes.h"
#include "AppSettings.hlsl"

// 一个针对9个float3元素的无分支排序网络。比循环快得多。
void Sort9(inout float3 values[9])
{
    // 这里是一系列固定的比较和交换操作。
    // 为了清晰起见，按阶段分组。
    // 注意：这里的排序是基于单个分量（如.r）的，但交换的是整个float3。
    // 对于中值滤波，只要排序依据一致即可。
    #define SWAP(i, j) { if(values[i].r > values[j].r) { float3 temp = values[i]; values[i] = values[j]; values[j] = temp; } }

    SWAP(0, 1); SWAP(3, 4); SWAP(6, 7);
    SWAP(1, 2); SWAP(4, 5); SWAP(7, 8);
    SWAP(0, 1); SWAP(3, 4); SWAP(6, 7);
    SWAP(0, 3); SWAP(3, 6);
    SWAP(1, 4); SWAP(4, 7);
    SWAP(2, 5); SWAP(5, 8);
    SWAP(0, 3); SWAP(3, 6);
    SWAP(1, 4); SWAP(4, 7);
    SWAP(2, 5);
    SWAP(1, 3);
    SWAP(2, 4);
    SWAP(5, 7);
    SWAP(2, 3);
    SWAP(5, 6);
    SWAP(4, 5);

    #undef SWAP
}


cbuffer DenoiseConstants : register(b0)
{
    int FilterRadius; // 对于3x3窗口，半径为1
    uint2 Padding;
};

// 输入：原始的、有噪声的纹理
Texture2D<float4> InputTexture : register(t0);

// 输出：一个UAV纹理，用于写入降噪结果
RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void DenoiseCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height;
    OutputTexture.GetDimensions(width, height);

    // 边界检查
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    // 收集3x3邻域的颜色值
    float3 neighborhood[9];
    int index = 0;
    for (int y = -FilterRadius; y <= FilterRadius; ++y)
    {
        for (int x = -FilterRadius; x <= FilterRadius; ++x)
        {
            // 将坐标限制在纹理边界内
            int2 coord = clamp(int2(dispatchThreadID.x + x, dispatchThreadID.y + y), 0, int2(width - 1, height - 1));

            // 使用 Load() 来获取精确的纹素值，避免任何采样或滤波
            neighborhood[index] = InputTexture.Load(int3(coord, 0)).rgb;
            index++;
        }
    }

    // 对9个颜色值进行排序
    // 更高效的方式是使用上面定义的 Sort9 函数
    // Sort9(neighborhood); // 如果使用上面的排序网络，调用此函数

    // 为了简单起见，这里我们使用一个基于亮度的插入排序
    for (int i = 1; i < 9; ++i)
    {
        float3 key = neighborhood[i];
        // 使用点积计算亮度来进行排序
        float keyLuminance = dot(key, float3(0.299, 0.587, 0.114));
        int j = i - 1;
        while (j >= 0 && dot(neighborhood[j], float3(0.299, 0.587, 0.114)) > keyLuminance)
        {
            neighborhood[j + 1] = neighborhood[j];
            j = j - 1;
        }
        neighborhood[j + 1] = key;
    }


    // 排序后，中值就是中间的元素（对于9个元素，索引是4）
    float3 medianColor = neighborhood[4];

    // 将结果写入输出纹理
    OutputTexture[dispatchThreadID.xy] = float4(medianColor, 1.0f);
}