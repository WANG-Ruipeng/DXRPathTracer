//OidnDenoiser.cpp
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

    if (denoisedOutput.size() != noisyInput.size())
    {
        denoisedOutput.resize(noisyInput.size());
    }

    if (width != currentWidth || height != currentHeight)
    {
        filter = device.newFilter("RTLightmap");
        currentWidth = width;
        currentHeight = height;
    }

    const size_t numPixels = width * height;
    const size_t numValues3CH = numPixels * 3;
    const size_t bufferBytes3CH = numValues3CH * sizeof(float);

    oidn::BufferRef colorBuf = device.newBuffer(bufferBytes3CH);
    oidn::BufferRef outputBuf = device.newBuffer(bufferBytes3CH);

    std::vector<float> colorBuffer3CH(numValues3CH);
    for (size_t i = 0; i < numPixels; ++i)
    {
        colorBuffer3CH[i * 3 + 0] = noisyInput[i * 4 + 0]; // R
        colorBuffer3CH[i * 3 + 1] = noisyInput[i * 4 + 1]; // G
        colorBuffer3CH[i * 3 + 2] = noisyInput[i * 4 + 2]; // B
    }

    colorBuf.write(0, bufferBytes3CH, colorBuffer3CH.data());

    filter.setImage("color", colorBuf, oidn::Format::Float3, width, height);
    filter.setImage("output", outputBuf, oidn::Format::Float3, width, height);
    filter.set("hdr", true);
    filter.commit();
    filter.execute();
    const char* errorMessage;
    if (device.getError(errorMessage) != oidn::Error::None)
        return;

    std::vector<float> outputBuffer3CH(numValues3CH);
    outputBuf.read(0, bufferBytes3CH, outputBuffer3CH.data());

    for (size_t i = 0; i < numPixels; ++i)
    {
        denoisedOutput[i * 4 + 0] = outputBuffer3CH[i * 3 + 0];
        denoisedOutput[i * 4 + 1] = outputBuffer3CH[i * 3 + 1];
        denoisedOutput[i * 4 + 2] = outputBuffer3CH[i * 3 + 2];
        denoisedOutput[i * 4 + 3] = 1.0f;
    }
}