#include <iostream>
#include <fstream>
#include <vector>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "nvEncodeAPI.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

// --- Error Handling Macros ---
#define CK_NVENC(cmd) do { \
    NVENCSTATUS err = cmd; \
    if (err != NV_ENC_SUCCESS) { \
        std::cerr << "NVENC Error: " << err << " at " << __LINE__ << std::endl; \
        exit(1); \
    } \
} while(0)

#define HR(expr) do { \
    HRESULT hr = (expr); \
    if (FAILED(hr)) { \
        std::cerr << "DirectX Error: 0x" << std::hex << hr << " at " << __LINE__ << std::endl; \
        exit(1); \
    } \
} while(0)

int main() {
    const int width = 1920;
    const int height = 1080;

    // --- 1. Initialize Both Devices ---
    ComPtr<ID3D11Device> pDevice11;
    ComPtr<ID3D11DeviceContext> pContext11;
    HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &pDevice11, nullptr, &pContext11));

    ComPtr<ID3D12Device> pDevice12;
    HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice12)));

    // --- 2. Setup DXGI Capture (D3D11) ---
    ComPtr<IDXGIDevice> dxgiDevice;
    HR(pDevice11.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    HR(dxgiDevice->GetAdapter(&adapter));
    ComPtr<IDXGIOutput> output;
    HR(adapter->EnumOutputs(0, &output));
    ComPtr<IDXGIOutput1> output1;
    HR(output.As(&output1));
    ComPtr<IDXGIOutputDuplication> pDuplication;
    HR(output1->DuplicateOutput(pDevice11.Get(), &pDuplication));

    // --- 3. Create the Shared Handle Bridge ---
    // Create a texture in D3D11 that can be shared with D3D12
    ComPtr<ID3D11Texture2D> pSharedTex11;
    D3D11_TEXTURE2D_DESC desc11 = {};
    desc11.Width = width;
    desc11.Height = height;
    desc11.MipLevels = 1;
    desc11.ArraySize = 1;
    desc11.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc11.SampleDesc.Count = 1;
    desc11.Usage = D3D11_USAGE_DEFAULT;
    desc11.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc11.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED; // CRITICAL

    HR(pDevice11->CreateTexture2D(&desc11, nullptr, &pSharedTex11));

    // Get the NT Handle for the bridge
    HANDLE sharedHandle = nullptr;
    ComPtr<IDXGIResource1> dxgiResource;
    HR(pSharedTex11.As(&dxgiResource));
    HR(dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedHandle));

    // Open the bridge in D3D12
    ComPtr<ID3D12Resource> pSharedTex12;
    HR(pDevice12->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&pSharedTex12)));
    CloseHandle(sharedHandle);

    // --- 4. Initialize NVENC (D3D12) ---
    NV_ENCODE_API_FUNCTION_LIST nv = { NV_ENCODE_API_FUNCTION_LIST_VER };
    CK_NVENC(NvEncodeAPICreateInstance(&nv));

    void* hEncoder = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    sessionParams.device = pDevice12.Get(); // Registering with DX12 Device
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.apiVersion = NVENCAPI_VERSION;
    CK_NVENC(nv.nvEncOpenEncodeSessionEx(&sessionParams, &hEncoder));

    // Register the D3D12 view of the bridge
    NV_ENC_REGISTER_RESOURCE regInput = { NV_ENC_REGISTER_RESOURCE_VER };
    regInput.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    regInput.resourceToRegister = pSharedTex12.Get(); // Now owned by D3D12
    regInput.width = width;
    regInput.height = height;
    regInput.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    CK_NVENC(nv.nvEncRegisterResource(hEncoder, &regInput));

    // (Standard Initialization: InitializeEncoder, CreateBitstreamBuffer, etc.)
    // ... [Omitted for brevity, use your existing init code here] ...

    // --- 5. Encoding Loop ---
    for (int i = 0; i < 600; ++i) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> pDesktopResource;
        if (FAILED(pDuplication->AcquireNextFrame(100, &frameInfo, &pDesktopResource))) continue;

        ComPtr<ID3D11Texture2D> pAcquiredTex;
        HR(pDesktopResource.As(&pAcquiredTex));

        // Transfer pixels: Capture (D3D11) -> Bridge (D3D11)
        pContext11->CopyResource(pSharedTex11.Get(), pAcquiredTex.Get());
        
        // At this point, pSharedTex12 (the D3D12 side) has the data![cite: 1]

        // (Standard Encode logic: Setup NV_ENC_PIC_PARAMS and call nvEncEncodePicture[cite: 1])
        // Remember to use DX12 Fences to sync the D3D11 copy and the D3D12 encode[cite: 1]

        pDuplication->ReleaseFrame();
    }

    return 0;
}