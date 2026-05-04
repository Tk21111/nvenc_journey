#include <iostream>
#include <fstream>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "nvEncodeAPI.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

#define CK_NVENC(cmd) do { NVENCSTATUS err = cmd; if (err != NV_ENC_SUCCESS) { \
    std::cerr << "NVENC Error: " << err << " at line " << __LINE__ << std::endl; exit(1); } } while(0)

int main() {
    // --- 1. Create Pure DX11 Device ---
    ComPtr<ID3D11Device> pDevice;
    ComPtr<ID3D11DeviceContext> pContext;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &pDevice, nullptr, &pContext);

    // --- 2. Setup DXGI Capture (Natively DX11) ---
    ComPtr<IDXGIDevice> dxgiDevice;
    pDevice.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output;
    adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1;
    output.As(&output1);
    ComPtr<IDXGIOutputDuplication> pDuplication;
    output1->DuplicateOutput(pDevice.Get(), &pDuplication);

    // --- 3. Initialize NVENC with DX11 Device ---
    NV_ENCODE_API_FUNCTION_LIST nv = { NV_ENCODE_API_FUNCTION_LIST_VER };
    CK_NVENC(NvEncodeAPICreateInstance(&nv));

    void* hEncoder = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    sessionParams.device = pDevice.Get(); // THE DX11 DEVICE[cite: 1]
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.apiVersion = NVENCAPI_VERSION;
    CK_NVENC(nv.nvEncOpenEncodeSessionEx(&sessionParams, &hEncoder));

    // --- 4. Configure Encoder (Low Latency)[cite: 1] ---
    NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.encodeWidth = 1920;
    initParams.encodeHeight = 1080;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY; // Best for screen capture[cite: 1]
    initParams.frameRateNum = 60;
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1; 
    CK_NVENC(nv.nvEncInitializeEncoder(hEncoder, &initParams));

    // Create a Bitstream Buffer for output[cite: 1]
    NV_ENC_CREATE_BITSTREAM_BUFFER cb = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    CK_NVENC(nv.nvEncCreateBitstreamBuffer(hEncoder, &cb));

    std::ofstream outFile("capture_dx11.h264", std::ios::binary);

    // --- 5. Simplified Encoding Loop ---
    for (int i = 0; i < 600; ++i) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> pDesktopResource;
        if (FAILED(pDuplication->AcquireNextFrame(100, &frameInfo, &pDesktopResource))) continue;

        ComPtr<ID3D11Texture2D> pAcquiredTex;
        pDesktopResource.As(&pAcquiredTex);

        // Register and Map (DX11 handles sync automatically)[cite: 1]
        NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.resourceToRegister = pAcquiredTex.Get(); // Same device as session![cite: 1]
        reg.width = 1920; reg.height = 1080;
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        CK_NVENC(nv.nvEncRegisterResource(hEncoder, &reg));

        NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
        map.registeredResource = reg.registeredResource;
        CK_NVENC(nv.nvEncMapInputResource(hEncoder, &map));

        NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        pic.outputBitstream = cb.bitstreamBuffer;
        pic.inputWidth = 1920; 
        pic.inputHeight = 1080;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        CK_NVENC(nv.nvEncEncodePicture(hEncoder, &pic));

        // Lock and Write to Disk[cite: 1]
        NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
        lock.outputBitstream = cb.bitstreamBuffer;
        CK_NVENC(nv.nvEncLockBitstream(hEncoder, &lock));
        outFile.write((char*)lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
        CK_NVENC(nv.nvEncUnlockBitstream(hEncoder, cb.bitstreamBuffer));

        // Cleanup for next frame[cite: 1]
        nv.nvEncUnmapInputResource(hEncoder, map.mappedResource);
        nv.nvEncUnregisterResource(hEncoder, reg.registeredResource);
        pDuplication->ReleaseFrame();
    }

    std::cout << "Capture complete. Saved to capture_dx11.h264" << std::endl;
    return 0;
}