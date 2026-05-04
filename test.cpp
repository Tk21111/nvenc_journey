#include <iostream>
#include <fstream>
#include <vector>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "nvEncodeAPI.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

// --- Macros for Error Checking ---
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
    const int frameCount = 600;

    // --- 1. DirectX 12 Setup ---
    ComPtr<ID3D12Device> pDevice;
    HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice)));

    ComPtr<ID3D12CommandQueue> pCommandQueue;
    D3D12_COMMAND_QUEUE_DESC queueDesc = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    HR(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pCommandQueue)));

    // Create the Fence for synchronization
    ComPtr<ID3D12Fence> pFence;
    uint64_t fenceValue = 0;
    HR(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence)));

    // --- 2. Create DX12 Resources for NVENCODE (Section 4.1.3) ---
    // Input Texture: Must be on a Default Heap
    ComPtr<ID3D12Resource> pInputResource;
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = { D3D12_HEAP_TYPE_DEFAULT };
    HR(pDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, 
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pInputResource)));

    // Output Buffer: Must be on a Readback Heap
    ComPtr<ID3D12Resource> pOutputResource;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = width * height * 4; // Recommended size
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES readbackHeap = { D3D12_HEAP_TYPE_READBACK };
    HR(pDevice->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufDesc, 
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pOutputResource)));

    // --- 3. NVENCODE Session (Section 3.1.1.4) ---
    NV_ENCODE_API_FUNCTION_LIST nv = { NV_ENCODE_API_FUNCTION_LIST_VER };
    CK_NVENC(NvEncodeAPICreateInstance(&nv));

    void* hEncoder = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    sessionParams.device = pDevice.Get(); // Pass IUnknown of DX12 device
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.apiVersion = NVENCAPI_VERSION;
    CK_NVENC(nv.nvEncOpenEncodeSessionEx(&sessionParams, &hEncoder));

    // --- 4. Register Resources (Section 4.1.3) ---
    // Registration returns a "Registered Handle" (the ID card for the resource)
    NV_ENC_REGISTER_RESOURCE regInput = { NV_ENC_REGISTER_RESOURCE_VER };
    regInput.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    regInput.resourceToRegister = pInputResource.Get();
    regInput.width = width;
    regInput.height = height;
    regInput.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    CK_NVENC(nv.nvEncRegisterResource(hEncoder, &regInput));
    void* registeredInput = regInput.registeredResource;

    NV_ENC_REGISTER_RESOURCE regOutput = { NV_ENC_REGISTER_RESOURCE_VER };
    regOutput.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    regOutput.resourceToRegister = pOutputResource.Get();
    regOutput.bufferUsage = NV_ENC_OUTPUT_BITSTREAM;
    CK_NVENC(nv.nvEncRegisterResource(hEncoder, &regOutput));
    void* registeredOutput = regOutput.registeredResource;

    // --- 5. Initialize Encoder ---
    NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.encodeWidth = width;
    initParams.encodeHeight = height;
    initParams.frameRateNum = 60;
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    CK_NVENC(nv.nvEncInitializeEncoder(hEncoder, &initParams));

    // --- 6. Encoding Loop (Section 4.3) ---
    std::ofstream outFile("capture_dx12.h264", std::ios::binary);

    for (int i = 0; i < frameCount; ++i) {
        // [Add your DXGI Capture Logic here to fill pInputResource]

        // Synchronization logic using Fence Points
        fenceValue++; 
        
        NV_ENC_INPUT_RESOURCE_D3D12 inputD3D12 = { 0 };
        inputD3D12.pInputBuffer = registeredInput;
        inputD3D12.inputFencePoint.pFence = pFence.Get();
        // inputD3D12.inputFencePoint.value = fenceValue; // Wait for capture to finish[cite: 1]

        NV_ENC_OUTPUT_RESOURCE_D3D12 outputD3D12 = { 0 };
        outputD3D12.pOutputBuffer = registeredOutput;
        outputD3D12.outputFencePoint.pFence = pFence.Get();
        // outputD3D12.outputFencePoint.value = fenceValue + 1; // Signal when encoded[cite: 1]

        NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
        picParams.inputBuffer = &inputD3D12;   // Pass the DX12 structure[cite: 1]
        // picParams.outputBuffer = &outputD3D12; // Pass the DX12 structure[cite: 1]
        picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        picParams.inputWidth = width;
        picParams.inputHeight = height;
        picParams.inputTimeStamp = i;

        CK_NVENC(nv.nvEncEncodePicture(hEncoder, &picParams));

        // Retrieve output (Section 4.4)[cite: 1]
        NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
        lock.outputBitstream = &outputD3D12; // Use the pointer to the DX12 struct[cite: 1]
        CK_NVENC(nv.nvEncLockBitstream(hEncoder, &lock));
        
        outFile.write((char*)lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
        
        CK_NVENC(nv.nvEncUnlockBitstream(hEncoder, lock.outputBitstream));
    }

    // --- 7. Cleanup ---
    nv.nvEncUnregisterResource(hEncoder, registeredInput);
    nv.nvEncUnregisterResource(hEncoder, registeredOutput);
    nv.nvEncDestroyEncoder(hEncoder);
    outFile.close();

    return 0;
}