#include <iostream>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>

// WebRTC Includes
#include <rtc/rtc.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <nlohmann/json.hpp>

// DirectX / NVENC Includes
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "nvEncodeAPI.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using json = nlohmann::json;
using namespace rtc;
using namespace std;
using Microsoft::WRL::ComPtr;

// NVENC Error Checking Macro
#define CK_NVENC(cmd) do { NVENCSTATUS err = cmd; if (err != NV_ENC_SUCCESS) { \
    std::cerr << "NVENC Error: " << err << " at line " << __LINE__ << std::endl; exit(1); } } while(0)

// --- Global State ---
struct Peer {
    shared_ptr<PeerConnection> pc;
    shared_ptr<DataChannel> dc;
    shared_ptr<Track> videoTrack;
};

map<shared_ptr<WebSocket>, Peer> peers;
std::mutex peersMutex; // Protects the peers map from cross-thread crashes

// Helper to broadcast H.264 NAL units to all connected WebRTC clients
void BroadcastH264Frame(const void* h264Data, size_t sizeInBytes) {
    std::lock_guard<std::mutex> lock(peersMutex);
    for (auto const& [ws, peer] : peers) {
        if (peer.videoTrack && peer.videoTrack->isOpen()) {
            auto dataPtr = reinterpret_cast<const std::byte*>(h264Data);
            peer.videoTrack->send(dataPtr, sizeInBytes);
        }
    }
}

int main() {
    // ========================================================================
    // 1. WEBRTC SERVER SETUP
    // ========================================================================
    WebSocketServer::Configuration wsConfig;
    wsConfig.port = 8080;
    auto wss = std::make_shared<WebSocketServer>(wsConfig);

    wss->onClient([](shared_ptr<WebSocket> ws) {
        cout << "[ws] new ws client connected" << endl;

        Configuration config;
        config.iceServers.emplace_back("stun:stun.l.google.com:19302");
        auto pc = make_shared<PeerConnection>(config);

        // --- Setup H.264 Video Track ---
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(96);          // Payload type 96 is standard for H.264
        video.addSSRC(42, "video-send"); // Arbitrary SSRC
        
        auto videoTrack = pc->addTrack(video);
        
        // NVENC outputs Annex-B (0x00 0x00 0x00 0x01), so we use LongStartSequence
        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            42, "video-send", 96, rtc::H264RtpPacketizer::defaultClockRate
        );
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::LongStartSequence, rtpConfig
        );
        videoTrack->setMediaHandler(packetizer);

        {
            std::lock_guard<std::mutex> lock(peersMutex);
            peers[ws] = {pc, nullptr, videoTrack};
        }

        pc->onLocalCandidate([ws](Candidate candidate) {
            json msg = {{"candidate", candidate.candidate()}, {"mid", candidate.mid()}};
            ws->send(msg.dump());
        });

        pc->onLocalDescription([ws](Description description) {
            json msg = {{"type", description.typeString()}, {"sdp", string(description)}};
            ws->send(msg.dump());
        });

        // --- Setup Data Channel ---
        auto dc = pc->createDataChannel("input");
        {
            std::lock_guard<std::mutex> lock(peersMutex);
            peers[ws].dc = dc;
        }

        dc->onOpen([dc](){
            cout << "[dataCh] open\n";
            dc->send("Hello from Server");
            dc->send("aaa");
        });

        dc->onMessage([](auto msg){
            auto &data = get<rtc::binary>(msg);
            struct Packet {
                int16_t lx; int16_t ly; uint8_t buttons;
            };
            Packet* p = (Packet*)data.data();
            bool A = p->buttons & (1<<0);
            bool B = p->buttons & (1<<1);
            // std::cout << "Input: lx=" << p->lx << " ly=" << p->ly << " A=" << A << " B=" << B << "\n";
        });

        pc->setLocalDescription();

        ws->onMessage([pc](auto data) {
            if (!holds_alternative<string>(data)) return;
            try {
                auto msg = json::parse(get<string>(data));
                if (msg.contains("type") && msg.contains("sdp")) {
                    pc->setRemoteDescription(Description(msg["sdp"].get<string>(), msg["type"].get<string>()));
                } else if (msg.contains("candidate")) {
                    pc->addRemoteCandidate(Candidate(msg["candidate"].get<string>(), msg["mid"].get<string>()));
                }
            } catch (const exception& e) {
                cerr << "[JSON Error] " << e.what() << endl;
            }
        });

        ws->onClosed([ws]() { 
            cout << "[ws] connection closed" << endl; 
            std::lock_guard<std::mutex> lock(peersMutex);
            peers.erase(ws);
        });
    });

    cout << "WebRTC Server listening on port 8080..." << endl;

    // ========================================================================
    // 2. DIRECTX 11 & DXGI DUPLICATION SETUP
    // ========================================================================
    ComPtr<ID3D11Device> pDevice;
    ComPtr<ID3D11DeviceContext> pContext;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &pDevice, nullptr, &pContext);

    ComPtr<IDXGIDevice> dxgiDevice;
    pDevice.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output;
    adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1;
    output.As(&output1);
    
    ComPtr<IDXGIOutputDuplication> pDuplication;
    HRESULT hr = output1->DuplicateOutput(pDevice.Get(), &pDuplication);
    if (FAILED(hr)) {
        cerr << "Failed to initialize DXGI Desktop Duplication. Is your display on?" << endl;
        return 1;
    }

    // ========================================================================
    // 3. NVENC SETUP
    // ========================================================================
    NV_ENCODE_API_FUNCTION_LIST nv = { NV_ENCODE_API_FUNCTION_LIST_VER };
    CK_NVENC(NvEncodeAPICreateInstance(&nv));

    void* hEncoder = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    sessionParams.device = pDevice.Get();
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.apiVersion = NVENCAPI_VERSION;
    CK_NVENC(nv.nvEncOpenEncodeSessionEx(&sessionParams, &hEncoder));

    NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = NV_ENC_PRESET_P3_GUID;
    initParams.encodeWidth = 1920;
    initParams.encodeHeight = 1080;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    initParams.frameRateNum = 60;
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1;

    NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } };
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    CK_NVENC(nv.nvEncGetEncodePresetConfigEx(
        hEncoder, 
        NV_ENC_CODEC_H264_GUID, 
        NV_ENC_PRESET_P1_GUID, 
        NV_ENC_TUNING_INFO_LOW_LATENCY, 
        &presetConfig
    ));

    initParams.encodeConfig = &presetConfig.presetCfg;    
    initParams.encodeConfig->encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    

    // 5. Force a Keyframe every 60 frames (1 second). Without this, the stream never starts.
    initParams.encodeConfig->encodeCodecConfig.h264Config.idrPeriod = 60;
    initParams.encodeConfig->rcParams.averageBitRate = 5000000;
    initParams.encodeConfig->rcParams.maxBitRate = 5000000;
    
    // Set VBV buffer size tight for low latency (averageBitRate / framerate)
    initParams.encodeConfig->rcParams.vbvBufferSize = 5000000 / 60;
    initParams.encodeConfig->rcParams.vbvInitialDelay = 5000000 / 60;

    CK_NVENC(nv.nvEncInitializeEncoder(hEncoder, &initParams));

    NV_ENC_CREATE_BITSTREAM_BUFFER cb = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    CK_NVENC(nv.nvEncCreateBitstreamBuffer(hEncoder, &cb));

    // Create a dedicated DX11 Texture for Encoding
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1920;
    desc.Height = 1080;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; 
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> pEncodeTexture;
    pDevice->CreateTexture2D(&desc, nullptr, &pEncodeTexture);

    // Register and Map Resource ONCE outside the loop[cite: 1]
    NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = pEncodeTexture.Get();
    reg.width = 1920; 
    reg.height = 1080;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB; 
    CK_NVENC(nv.nvEncRegisterResource(hEncoder, &reg));

    NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    map.registeredResource = reg.registeredResource;
    CK_NVENC(nv.nvEncMapInputResource(hEncoder, &map));

    cout << "Starting Video Capture & Stream loop..." << endl;

    // ========================================================================
    // 4. MAIN CAPTURE & STREAMING LOOP
    // ========================================================================
    while (true) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> pDesktopResource;
        
        // Wait up to 10ms for a new frame. If the screen hasn't changed, DXGI returns a timeout.
        HRESULT acquireResult = pDuplication->AcquireNextFrame(10, &frameInfo, &pDesktopResource);
        
        if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT) {
            continue; // No screen update, just loop again
        } else if (FAILED(acquireResult)) {
            cerr << "Lost DXGI capture. Needs reinitialization." << endl;
            break; 
        }

        ComPtr<ID3D11Texture2D> pAcquiredTex;
        pDesktopResource.As(&pAcquiredTex);

        // Copy the acquired desktop frame into our mapped NVENC texture
        pContext->CopyResource(pEncodeTexture.Get(), pAcquiredTex.Get());

        // Setup picture params
        NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        pic.inputWidth = 1920;                       
        pic.inputHeight = 1080;                      
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME; 
        pic.outputBitstream = cb.bitstreamBuffer;

        // Encode
        CK_NVENC(nv.nvEncEncodePicture(hEncoder, &pic));

        // Lock, Broadcast to WebRTC, and Unlock
        NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
        lock.outputBitstream = cb.bitstreamBuffer;
        CK_NVENC(nv.nvEncLockBitstream(hEncoder, &lock));
        // cout << "load" << endl;

        if (lock.bitstreamSizeInBytes > 0) {
            // cout << "send" << endl;
            // static std::ofstream dumpFile("debug_stream.h264", std::ios::binary | std::ios::app);
            // dumpFile.write(reinterpret_cast<const char*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes);
            // dumpFile.flush();
            BroadcastH264Frame(lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
        }

        CK_NVENC(nv.nvEncUnlockBitstream(hEncoder, cb.bitstreamBuffer));

        pDuplication->ReleaseFrame();
    }

    // ========================================================================
    // 5. CLEANUP
    // ========================================================================
    nv.nvEncUnmapInputResource(hEncoder, map.mappedResource);
    nv.nvEncUnregisterResource(hEncoder, reg.registeredResource);
    nv.nvEncDestroyBitstreamBuffer(hEncoder, cb.bitstreamBuffer);
    nv.nvEncDestroyEncoder(hEncoder);

    return 0;
}