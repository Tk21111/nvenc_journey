#include <iostream>
#include <fstream>
#include <d3d11.h> 
#include <dxgi1_2.h> 
#include "nvEncodeAPI.h"
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
// Link the DirectX library (or add it to CMake)
#pragma comment(lib, "d3d11.lib")

#define CK_NVENC(cmd) \
    do { \
        NVENCSTATUS err = cmd; \
        if (err != NV_ENC_SUCCESS) { \
            std::cerr << "NVENC Error: " << err \
                    << " at " << __FILE__ << ":" << __LINE__ \
                    << " -> " << #cmd << std::endl; \
            exit(1); \
        } \
    } while(0)

#define HR(expr)                                                    \
    do {                                                            \
        HRESULT _hr = (expr);                                       \
        if (FAILED(_hr)) {                                          \
            std::cerr << "FAILED: " #expr                           \
                      << "  hr=0x" << std::hex << _hr << "\n";     \
            return false;                                           \
        }                                                           \
    } while(0)


int main() {
    // --- STEP 1: Create a D3D11 Device ---
    ComPtr<ID3D11Device> pDevice = nullptr;
    ComPtr<ID3D11DeviceContext> pContext = nullptr;
    ComPtr<IDXGIOutputDuplication> pDuplication = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, 
        nullptr, 0, D3D11_SDK_VERSION, &pDevice, &featureLevel, &pContext
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 Device" << std::endl;
        return 1;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    HR( pDevice.As(&dxgiDevice) );
    ComPtr<IDXGIAdapter> adapter;
    HR( dxgiDevice->GetAdapter(&adapter) );
    ComPtr<IDXGIOutput> output;
    HR( adapter->EnumOutputs(0, &output) );
    ComPtr<IDXGIOutput1> output1;
    HR( output.As(&output1) );
    HR( output1->DuplicateOutput(pDevice.Get(), &pDuplication) );

    // --- STEP 2: Initialize NVENC ---
    NV_ENCODE_API_FUNCTION_LIST nvFunctions = { NV_ENCODE_API_FUNCTION_LIST_VER };
    CK_NVENC(NvEncodeAPICreateInstance(&nvFunctions));

    // --- STEP 3: Open Session with the D3D11 Device ---
    void* hEncoder = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    
    ID3D11Device* pTmpDevice = pDevice.Get();
    sessionParams.device = pTmpDevice;                // Pass the DirectX device here!
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

    std::ofstream outFile("capture.h264", std::ios::binary);
    std::cout << "Starting capture for 600 frames (approx 10 seconds)..." << std::endl;

    for (int i = 0; i < 600; ++i) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> pDesktopResource = nullptr;

        HRESULT hr = pDuplication->AcquireNextFrame(100, &frameInfo, &pDesktopResource);
        if (FAILED(hr)) continue;

        ComPtr<ID3D11Texture2D> pAcquiredTexture;

        if (FAILED(pDesktopResource.As(&pAcquiredTexture))) {
            pDuplication->ReleaseFrame();
            continue;
        }

        // --- STEP 4.1.2: Register & Map for Low-Copy ---
        NV_ENC_REGISTER_RESOURCE regParam = { NV_ENC_REGISTER_RESOURCE_VER };
        regParam.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        regParam.resourceToRegister = pAcquiredTexture.Get();
        regParam.width = 1920;
        regParam.height = 1080;
        regParam.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        nvFunctions.nvEncRegisterResource(hEncoder, &regParam);

        NV_ENC_MAP_INPUT_RESOURCE mapParam = { NV_ENC_MAP_INPUT_RESOURCE_VER };
        mapParam.registeredResource = regParam.registeredResource;
        nvFunctions.nvEncMapInputResource(hEncoder, &mapParam);

        // --- STEP 4.3: Submit to GPU ---
        NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
        picParams.inputBuffer = mapParam.mappedResource;
        picParams.outputBitstream = hBitstreamBuffer;
        picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB; // REQUIRED: Match your registration[cite: 1]
        picParams.inputWidth = 1920;  // Highly recommended[cite: 1]
        picParams.inputHeight = 1080; // Highly recommended[cite: 1]
        picParams.encodePicFlags = 0;

        CK_NVENC(nvFunctions.nvEncEncodePicture(hEncoder, &picParams));
        
        //--- STEP 4.4: Retrieve and Save to Disk
        NV_ENC_LOCK_BITSTREAM lockParams = { NV_ENC_LOCK_BITSTREAM_VER };
        lockParams.outputBitstream = hBitstreamBuffer;
        lockParams.doNotWait = 0; // Synchronous mode: wait for GPU to finish

        CK_NVENC(nvFunctions.nvEncLockBitstream(hEncoder, &lockParams));
        outFile.write((char*)lockParams.bitstreamBufferPtr, lockParams.bitstreamSizeInBytes);
        CK_NVENC(nvFunctions.nvEncUnlockBitstream(hEncoder, hBitstreamBuffer));

        CK_NVENC(nvFunctions.nvEncUnmapInputResource(hEncoder, mapParam.mappedResource));
        CK_NVENC(nvFunctions.nvEncUnregisterResource(hEncoder, regParam.registeredResource));

        pDuplication->ReleaseFrame();
    }

    NV_ENC_PIC_PARAMS eosParams = { NV_ENC_PIC_PARAMS_VER };
    eosParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS; // Signal the end[cite: 1]
    nvFunctions.nvEncEncodePicture(hEncoder, &eosParams);

    outFile.close();

    // Cleanup
    nvFunctions.nvEncDestroyEncoder(hEncoder);

    return 0;
}