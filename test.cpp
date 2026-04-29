#include <iostream>
#include <d3d11.h> // New: For DirectX 11
#include "nvEncodeAPI.h"
#include <vector>

// Link the DirectX library (or add it to CMake)
#pragma comment(lib, "d3d11.lib")

#define CK_NVENC(cmd) do { \
    NVENCSTATUS err = cmd; \
    if (err != NV_ENC_SUCCESS) { \
        std::cerr << "NVENC Error: " << err \
                  << " at " << __FILE__ << ":" << __LINE__ \
                  << " -> " << #cmd << std::endl; \
        exit(1); \
    } \
} while(0)

int main() {
    // --- STEP 1: Create a D3D11 Device ---
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, 
        nullptr, 0, D3D11_SDK_VERSION, &pDevice, &featureLevel, &pContext
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 Device" << std::endl;
        return 1;
    }

    // --- STEP 2: Initialize NVENC ---
    NV_ENCODE_API_FUNCTION_LIST nvFunctions = { NV_ENCODE_API_FUNCTION_LIST_VER };
    CK_NVENC(NvEncodeAPICreateInstance(&nvFunctions));

    // --- STEP 3: Open Session with the D3D11 Device ---
    void* hEncoder = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    
    sessionParams.device = pDevice;                // Pass the DirectX device here!
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; // Change type to DirectX
    sessionParams.apiVersion = NVENCAPI_VERSION;
    

    CK_NVENC(nvFunctions.nvEncOpenEncodeSessionEx(&sessionParams, &hEncoder));

    // Get the number of supported GUIDs
    uint32_t guidCount = 0;
    CK_NVENC(nvFunctions.nvEncGetEncodeGUIDCount(hEncoder, &guidCount));

    //get proper encoder
    std::vector<GUID> guids(guidCount);
    CK_NVENC(nvFunctions.nvEncGetEncodeGUIDs(hEncoder, guids.data(), guidCount, &guidCount));
    GUID encodeGUID = NV_ENC_CODEC_H264_GUID;


    NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } };
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    // We ask the GPU: "Give me the best settings for Low Latency H.264 using Preset P3"
    CK_NVENC(nvFunctions.nvEncGetEncodePresetConfigEx(
        hEncoder, 
        encodeGUID, 
        NV_ENC_PRESET_P3_GUID, 
        NV_ENC_TUNING_INFO_LOW_LATENCY, 
        &presetConfig
    ));

    NV_ENC_CONFIG encodeConfig = presetConfig.presetCfg;
    encodeConfig.gopLength = 60; 
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR; // Constant Bitrate
    encodeConfig.rcParams.averageBitRate = 5000000;
    encodeConfig.rcParams.maxBitRate = 5000000;
    encodeConfig.rcParams.vbvBufferSize = 5000000;
    encodeConfig.rcParams.vbvInitialDelay = 5000000;


    NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    initParams.encodeGUID = encodeGUID;
    initParams.presetGUID = NV_ENC_PRESET_P3_GUID;
    initParams.encodeWidth = 1920; 
    initParams.encodeHeight = 1080;
    initParams.encodeConfig = &encodeConfig;
    initParams.frameRateNum = 60; 
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1; //let api handle
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    initParams.darWidth = 1920;
    initParams.darHeight = 1080;
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;

    CK_NVENC(nvFunctions.nvEncInitializeEncoder(hEncoder, &initParams));

    std::cout << "nvEncInitializeEncoder" << std::endl;

    NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamBuffer = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    CK_NVENC(nvFunctions.nvEncCreateBitstreamBuffer(hEncoder, &createBitstreamBuffer));
    void* hBitstreamBuffer = createBitstreamBuffer.bitstreamBuffer;

    std::cout << "Hardware Fully Prepared. Ready for pixels!" << std::endl;

    // Cleanup
    nvFunctions.nvEncDestroyEncoder(hEncoder);
    pDevice->Release();
    pContext->Release();

    return 0;
}