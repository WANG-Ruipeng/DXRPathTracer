#pragma once

#include <oidn.hpp>
#include <vector>

class OidnDenoiser
{
public:
    OidnDenoiser() = default;
    ~OidnDenoiser();

    void Initialize();
    void Shutdown();

    // 为一个3通道的浮点图像 (RGB) 进行降噪
    void Denoise(std::vector<float>& denoisedOutput, const std::vector<float>& noisyInput, int width, int height);

private:
    oidn::DeviceRef device = nullptr;
    oidn::FilterRef filter = nullptr;
    int currentWidth = 0;
    int currentHeight = 0;
};