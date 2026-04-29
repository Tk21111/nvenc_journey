#include <iostream>
#include <d3d11.h> // New: For DirectX 11
#include "nvEncodeAPI.h"

// Link the DirectX library (or add it to CMake)
#pragma comment(lib, "d3d11.lib")

#define CK_NVENC(cmd) do { \
    NVENCSTATUS err = cmd; \
    if (err != NV_ENC_SUCCESS) { \
        std::cerr << "NVENC Error: " << err << std::endl; \
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

    std::cout << "Success! Error 6 is gone. Session opened with D3D11." << std::endl;

    // Cleanup
    nvFunctions.nvEncDestroyEncoder(hEncoder);
    pDevice->Release();
    pContext->Release();

    return 0;
}