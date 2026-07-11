#include <iostream>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>

#include <rtc/rtc.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/plihandler.hpp>
#include <nlohmann/json.hpp>
#include "input/controller.hpp"
#include "input/touch.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "nvEncodeAPI.h"

#include <mmsystem.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <fstream>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")


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
    shared_ptr<DataChannel> telemetry; // per-frame observation log (server -> client)
};

map<shared_ptr<WebSocket>, Peer> peers;
std::mutex peersMutex; // Protects the peers map from cross-thread crashes
std::atomic<bool> forceIDR{false};

// Resource Monitoring
double peakCpu = 0;
double peakGpu = 0;

void BroadcastMetrics(double cpu, double gpu) {
    json metrics = {
        {"type", "metrics"},
        {"cpu", cpu},
        {"gpu", gpu}
    };
    string msg = metrics.dump();
    std::lock_guard<std::mutex> lock(peersMutex);
    for (auto const& [ws, peer] : peers) {
        if (peer.dc && peer.dc->isOpen()) {
            peer.dc->send(msg);
        }
    }
}

// Helper to broadcast a per-frame telemetry record on the dedicated channel
void BroadcastTelemetry(const std::string& msg) {
    std::lock_guard<std::mutex> lock(peersMutex);
    for (auto const& [ws, peer] : peers) {
        if (peer.telemetry && peer.telemetry->isOpen()) {
            peer.telemetry->send(msg);
        }
    }
}

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

    try {

    timeBeginPeriod(1);

    Controller controller;
    if (!controller.Init()) {
        std::cerr << "[input] ViGEm init failed\n";
        return 1;
    }

    Touch touch;
    if (!touch.Init(1920, 1080)) {
        std::cerr << "[input] touch init failed\n";
        return 1;
    }

    WebSocketServer::Configuration wsConfig;
    wsConfig.port = 8081;
    auto wss = std::make_shared<WebSocketServer>(wsConfig);

    wss->onClient([&](shared_ptr<WebSocket> ws) {
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

        // Change from LongStartSequence to StartSequence to support NVENC P-frames
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtpConfig
        );
        // Respond to the client's Picture Loss Indication (PLI/FIR) by forcing an IDR.
        // This is how we recover *immediately* when the phone detects a corrupt frame,
        // instead of waiting for the next scheduled keyframe.
        auto pliHandler = std::make_shared<rtc::PliHandler>([]() {
            std::cout << "[WebRTC] PLI received -> forcing keyframe\n";
            forceIDR = true;
        });
        packetizer->addToChain(pliHandler);

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

        // --- Setup Telemetry Channel (separate from input so it never competes) ---
        auto tdc = pc->createDataChannel("telemetry");
        {
            std::lock_guard<std::mutex> lock(peersMutex);
            peers[ws].telemetry = tdc;
        }

        dc->onOpen([dc](){
            cout << "[dataCh] open\n";
            dc->send("Hello from Server");
            dc->send("aaa");
        });

        dc->onMessage([&] (rtc::message_variant msg){
            // Only handle binary messages
            if (!std::holds_alternative<rtc::binary>(msg)) return;

            auto& data = std::get<rtc::binary>(msg);

     
                std::cout << "[peer] Packet #"
                          << " - Size: " << data.size() 
                          << " bytes (expected: " << sizeof(InputPacket) << ")\n" << std::endl;
            

            if (data.size() < sizeof(InputPacket)) return;

            InputPacket packet{};
            std::memcpy(&packet, data.data(), sizeof(InputPacket));

                std::cout << "[peer] Deserialized - lx:" << packet.lx 
                          << " ly:" << packet.ly
                          << " dx:" << packet.dx
                          << " dy:" << packet.dy
                          << " buttons:0x" << std::hex << (int)packet.buttons << std::dec
                          << "\n" << std::endl;

            try {
                controller.Update(packet);
                touch.Update(packet.points);
            }
            catch (const std::exception& e) {
                std::cerr << "OnInput callback error: " << e.what() << '\n';
            }
            
        });

        pc->setLocalDescription();

        pc->onStateChange([](rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Connected) {
                std::cout << "\n[WebRTC] NETWORK CONNECTED! Blasting Keyframe...\n" << std::endl;
                forceIDR = true;
            }
        });

        ws->onMessage([pc](auto data) {
            if (!holds_alternative<string>(data)) return;
            try {
                auto msg = json::parse(get<string>(data));
                if (msg.contains("type") && msg.contains("sdp")) {
                    pc->setRemoteDescription(Description(msg["sdp"].get<string>(), msg["type"].get<string>()));
                    forceIDR = true;
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
    initParams.frameRateNum = 30;
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
    initParams.encodeConfig->profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;

    // Keyframe safety net: bound worst-case corruption from a lost packet to ~2s
    // (instead of 10s). On-demand recovery is handled faster by the PLI handler above.
    initParams.encodeConfig->gopLength = 60;
    initParams.encodeConfig->encodeCodecConfig.h264Config.idrPeriod = 60;
    initParams.encodeConfig->encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    // Prevent NVENC from injecting AUD NAL units, which confuse WebRTC packetizers
    initParams.encodeConfig->encodeCodecConfig.h264Config.outputAUD = 0;
    

    // 5. Force a Keyframe every 60 frames (1 second). Without this, the stream never starts.
    initParams.encodeConfig->encodeCodecConfig.h264Config.enableIntraRefresh = 0;  
    initParams.encodeConfig->rcParams.averageBitRate = 5000000;
    initParams.encodeConfig->rcParams.maxBitRate = 5000000;
    
    // Set VBV buffer size tight for low latency (averageBitRate / framerate)
    initParams.encodeConfig->rcParams.vbvBufferSize = 5000000 / 30;
    initParams.encodeConfig->rcParams.vbvInitialDelay = 5000000 / 30;

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

    cout << "Starting Video Capture & Stream loop..." << endl;

    const auto frameDuration = std::chrono::microseconds(33333);
    auto nextFrameTime = std::chrono::steady_clock::now();

    // --- Observation log setup ---
    const auto progStart = std::chrono::steady_clock::now();
    auto prevCap0 = progStart;
    uint64_t frameId = 0;
    std::ofstream perfLog("perf_log.csv");
    perfLog << "id,sendMs,capUs,encUs,pipeUs,bytes,idr,dtUs\n";
    auto usSince = [](std::chrono::steady_clock::time_point a,
                      std::chrono::steady_clock::time_point b) {
        return (long long)std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    };

    while (true) {
        // --- HIGH PRECISION PACER ---
        auto now = std::chrono::steady_clock::now();
        
        if (now < nextFrameTime) {
            auto timeRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(nextFrameTime - now).count();
            
            // If we have more than 2ms to wait, let the CPU rest (safely, since we boosted the timer)
            if (timeRemaining > 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            } else {
                // We are less than 2ms away! Spin-wait for perfect microsecond accuracy
                while (std::chrono::steady_clock::now() < nextFrameTime) {
                    std::this_thread::yield(); 
                }
            }
        }
        
        // Advance the clock by one frame to keep a steady cadence
        nextFrameTime += frameDuration;

        // Anti-burst: if capture/encode fell behind (nextFrameTime already in the past),
        // resync the cadence instead of firing frames back-to-back to "catch up".
        // Bursting a backlog of frames is a primary cause of UDP packet loss over WiFi.
        now = std::chrono::steady_clock::now();
        if (now > nextFrameTime) {
            nextFrameTime = now + frameDuration;
        }

        // --- Telemetry: mark start of this frame (capture) ---
        auto tCap0 = std::chrono::steady_clock::now();
        long long dtUs = usSince(prevCap0, tCap0); // real inter-frame interval
        prevCap0 = tCap0;

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> pDesktopResource;

        HRESULT acquireResult = pDuplication->AcquireNextFrame(0, &frameInfo, &pDesktopResource);

        if (SUCCEEDED(acquireResult)) {
            ComPtr<ID3D11Texture2D> pAcquiredTex;
            pDesktopResource.As(&pAcquiredTex);

            // This copy now works perfectly because NVENC is not locking the texture!
            pContext->CopyResource(pEncodeTexture.Get(), pAcquiredTex.Get());
            pDuplication->ReleaseFrame();
        }
        else if (acquireResult != DXGI_ERROR_WAIT_TIMEOUT) {
            cerr << "Lost DXGI capture. Needs reinitialization." << endl;
            break; 
        }

        // --- GPU DEADLOCK FIX: Map the texture just before encoding ---
        NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
        map.registeredResource = reg.registeredResource;
        CK_NVENC(nv.nvEncMapInputResource(hEncoder, &map));

        NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        pic.inputWidth = 1920;                       
        pic.inputHeight = 1080;                      
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME; 
        pic.outputBitstream = cb.bitstreamBuffer;

        pic.encodePicFlags = 0;
        if (forceIDR) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
            forceIDR = false;
        }

        // --- Telemetry: encode timing ---
        long long capUs = usSince(tCap0, std::chrono::steady_clock::now());
        auto tEnc0 = std::chrono::steady_clock::now();

        CK_NVENC(nv.nvEncEncodePicture(hEncoder, &pic));

        NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
        lock.outputBitstream = cb.bitstreamBuffer;
        CK_NVENC(nv.nvEncLockBitstream(hEncoder, &lock));

        auto tReady = std::chrono::steady_clock::now();
        long long encUs  = usSince(tEnc0, tReady);   // encode + lock cost
        long long pipeUs = usSince(tCap0, tReady);   // full capture -> ready-to-send
        double    sendMs = std::chrono::duration<double, std::milli>(tReady - progStart).count();
        bool      isIdr  = (lock.pictureType == NV_ENC_PIC_TYPE_IDR);
        uint64_t  bytes  = lock.bitstreamSizeInBytes;

        if (lock.bitstreamSizeInBytes > 0) {
            BroadcastH264Frame(lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);

            // Push a compact per-frame record to the client on the telemetry channel...
            json rec = {
                {"t", "f"}, {"id", frameId}, {"capUs", capUs}, {"encUs", encUs},
                {"pipeUs", pipeUs}, {"bytes", bytes}, {"idr", isIdr ? 1 : 0},
                {"dtUs", dtUs}, {"sendMs", sendMs}
            };
            BroadcastTelemetry(rec.dump());

            // ...and to the on-disk CSV for offline analysis.
            perfLog << frameId << ',' << sendMs << ',' << capUs << ',' << encUs << ','
                    << pipeUs << ',' << bytes << ',' << (isIdr ? 1 : 0) << ',' << dtUs << '\n';
            if ((frameId % 30) == 0) perfLog.flush();
            frameId++;
        }

        CK_NVENC(nv.nvEncUnlockBitstream(hEncoder, cb.bitstreamBuffer));
        
        // --- GPU DEADLOCK FIX: Unmap the texture so DXGI can safely use it next frame ---
        CK_NVENC(nv.nvEncUnmapInputResource(hEncoder, map.mappedResource));
    }

    // ========================================================================
    // 5. CLEANUP
    // ========================================================================
    nv.nvEncUnregisterResource(hEncoder, reg.registeredResource);
    nv.nvEncDestroyBitstreamBuffer(hEncoder, cb.bitstreamBuffer);
    nv.nvEncDestroyEncoder(hEncoder);

    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL ERROR] Caught exception: " << e.what() << std::endl;
        system("pause"); // Keeps the console open so you can read it
        return -1;
    } catch (...) {
        std::cerr << "\n[FATAL ERROR] Caught an unknown exception!" << std::endl;
        system("pause");
        return -1;
    }
    return 0;
}