#include <PCH.h>
#include "OidnDenoiser.h"
#include <iostream>

OidnDenoiser::~OidnDenoiser()
{
    Shutdown();
}

void OidnDenoiser::Initialize()
{
    // 创建一个 OIDN 设备
    device = oidn::newDevice();

    // [可选] 设置错误回调函数，将错误信息打印到控制台
    device.setErrorFunction([](void* userPtr, oidn::Error error, const char* message)
    {
        std::cerr << "OIDN Error: " << message << std::endl;
        OutputDebugStringA("OIDN Error: ");
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    });

    device.commit();
    std::cout << "OIDN device initialized." << std::endl;
}

void OidnDenoiser::Shutdown()
{
    // OIDN 对象是引用计数的，会自动释放，但显式释放是好习惯
    filter.release();
    device.release();
}

// =================================================================================
//  完整修正版的 Denoise 函数
// =================================================================================
void OidnDenoiser::Denoise(std::vector<float>& denoisedOutput, const std::vector<float>& noisyInput, int width, int height)
{
    if (!device)
    {
        return;
    }

    // 如果需要，为最终的4通道输出缓冲区分配空间
    if (denoisedOutput.size() != noisyInput.size())
    {
        denoisedOutput.resize(noisyInput.size());
    }

    // 如果图像尺寸发生变化，则（重新）创建过滤器
    if (width != currentWidth || height != currentHeight)
    {
        filter = device.newFilter("RTLightmap"); // 使用为光照贴图优化的专用过滤器
        currentWidth = width;
        currentHeight = height;
    }

    const size_t numPixels = width * height;
    const size_t numValues3CH = numPixels * 3; // 3通道 (RGB) 的浮点数数量
    const size_t bufferBytes3CH = numValues3CH * sizeof(float); // 3通道缓冲区的总字节数

    // ------------------- 变化点 1: 创建设备可访问的 OIDN Buffer -------------------
    oidn::BufferRef colorBuf = device.newBuffer(bufferBytes3CH);
    oidn::BufferRef outputBuf = device.newBuffer(bufferBytes3CH);

    // --- CPU端准备数据 ---
    // 这个临时CPU缓冲区仍然是必要的，用来完成 4通道 -> 3通道 的转换
    std::vector<float> colorBuffer3CH(numValues3CH);
    for (size_t i = 0; i < numPixels; ++i)
    {
        colorBuffer3CH[i * 3 + 0] = noisyInput[i * 4 + 0]; // R
        colorBuffer3CH[i * 3 + 1] = noisyInput[i * 4 + 1]; // G
        colorBuffer3CH[i * 3 + 2] = noisyInput[i * 4 + 2]; // B
    }

    // ------------------- 变化点 2: 将CPU数据写入 OIDN Buffer -------------------
    // 工作流: std::vector (CPU) -> oidn::Buffer (设备)
    colorBuf.write(0, bufferBytes3CH, colorBuffer3CH.data());

    // ------------------- 变化点 3: 将 OIDN Buffer 绑定到滤镜 -------------------
    // 现在我们传递的是 Buffer 对象，而不是原始指针
    filter.setImage("color", colorBuf, oidn::Format::Float3, width, height);
    filter.setImage("output", outputBuf, oidn::Format::Float3, width, height);

    // 为光照贴图这类HDR数据设置提示
    filter.set("hdr", true);

    // 提交对过滤器的更改
    filter.commit();

    // 执行降噪
    filter.execute();

    // 检查执行过程中是否有错误
    const char* errorMessage;
    if (device.getError(errorMessage) != oidn::Error::None)
        return;

    // --- CPU端准备接收数据 ---
    // 仍然需要一个临时的CPU缓冲区来接收3通道的结果
    std::vector<float> outputBuffer3CH(numValues3CH);

    // ------------------- 变化点 4: 从 OIDN Buffer 读回数据到CPU -------------------
    // 工作流: oidn::Buffer (设备) -> std::vector (CPU)
    outputBuf.read(0, bufferBytes3CH, outputBuffer3CH.data());

    // 将 OIDN 的3通道输出转换回我们最终的4通道缓冲区
    // 并将 Alpha 通道设置为 1.0
    for (size_t i = 0; i < numPixels; ++i)
    {
        denoisedOutput[i * 4 + 0] = outputBuffer3CH[i * 3 + 0];
        denoisedOutput[i * 4 + 1] = outputBuffer3CH[i * 3 + 1];
        denoisedOutput[i * 4 + 2] = outputBuffer3CH[i * 3 + 2];
        denoisedOutput[i * 4 + 3] = 1.0f;
    }
}