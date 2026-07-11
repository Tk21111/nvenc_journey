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

// --- Adaptive bitrate (client decides, capture thread applies) ---
// Single-client assumption: one target shared globally.
constexpr uint32_t BITRATE_START = 5000000;
std::atomic<uint32_t> g_targetBitrate{BITRATE_START};
std::atomic<bool>     g_bitrateDirty{false};

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

        dc->onOpen([](){
            cout << "[dataCh] input channel open\n";
        });

        dc->onMessage([&] (rtc::message_variant msg){
            // Text messages = control (e.g. adaptive-bitrate requests from the client).
            if (std::holds_alternative<rtc::string>(msg)) {
                try {
                    auto j = json::parse(std::get<rtc::string>(msg));
                    if (j.value("type", "") == "bitrate") {
                        g_targetBitrate = j.value("bps", (uint32_t)BITRATE_START);
                        g_bitrateDirty = true;
                    }
                } catch (...) {}
                return;
            }

            // Binary messages = 52-byte controller input packets.
            if (!std::holds_alternative<rtc::binary>(msg)) return;
            auto& data = std::get<rtc::binary>(msg);
            if (data.size() < sizeof(InputPacket)) return;

            InputPacket packet{};
            std::memcpy(&packet, data.data(), sizeof(InputPacket));

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
    // ==== Single frame-rate knob: change this one line to switch 30 <-> 60 fps ====
    const int TARGET_FPS = 60;
    initParams.frameRateNum = TARGET_FPS;
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

    // Keyframe safety net ~every 2s (bounds worst-case corruption from a lost packet).
    // On-demand recovery is handled faster by the PLI handler.
    initParams.encodeConfig->gopLength = TARGET_FPS * 2;
    initParams.encodeConfig->encodeCodecConfig.h264Config.idrPeriod = TARGET_FPS * 2;
    initParams.encodeConfig->encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    // Prevent NVENC from injecting AUD NAL units, which confuse WebRTC packetizers
    initParams.encodeConfig->encodeCodecConfig.h264Config.outputAUD = 0;
    

    // 5. Force a Keyframe every 60 frames (1 second). Without this, the stream never starts.
    initParams.encodeConfig->encodeCodecConfig.h264Config.enableIntraRefresh = 0;  
    initParams.encodeConfig->rcParams.averageBitRate = 5000000;
    initParams.encodeConfig->rcParams.maxBitRate = 5000000;
    
    // Set VBV buffer size tight for low latency (averageBitRate / framerate).
    // One-frame VBV keeps frame sizes even -> smoother pacing, less bursty loss.
    initParams.encodeConfig->rcParams.vbvBufferSize = 5000000 / TARGET_FPS;
    initParams.encodeConfig->rcParams.vbvInitialDelay = 5000000 / TARGET_FPS;

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

    // --- 60 fps target on a 60 Hz display: event-driven, send-on-encode ---
    // We no longer pace to a fixed grid. AcquireNextFrame BLOCKS until Windows signals a
    // new desktop frame, so a screen change is captured the instant it happens and sent
    // immediately -- no grid wait, no beat jitter. On a static screen we sleep in the OS
    // wait and emit nothing (the client just holds the last frame).
    const auto minFrameInterval = std::chrono::microseconds(1000000 / TARGET_FPS); // fps cap
    const UINT ACQUIRE_TIMEOUT_MS = 100; // heartbeat: how long we block; also bounds how fast
                                         // a forced keyframe (PLI / new client) is serviced when idle

    // --- Observation log setup ---
    const auto progStart = std::chrono::steady_clock::now();
    auto prevCap0 = progStart;
    auto lastSend = progStart;
    bool haveCaptured = false;
    uint64_t frameId = 0;
    std::ofstream perfLog("perf_log.csv");
    perfLog << "id,sendMs,capUs,encUs,pipeUs,bytes,idr,dtUs\n";
    auto usSince = [](std::chrono::steady_clock::time_point a,
                      std::chrono::steady_clock::time_point b) {
        return (long long)std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    };

    while (true) {
        // --- Adaptive bitrate: apply the client's latest request (capture thread owns NVENC) ---
        if (g_bitrateDirty.exchange(false)) {
            uint32_t bps = g_targetBitrate.load();
            // Only touch the bitrate. Leave vbvBufferSize/InitialDelay at their init values
            // so rate-control state isn't reset on every tweak (avoids a reconfigure hitch).
            initParams.encodeConfig->rcParams.averageBitRate = bps;
            initParams.encodeConfig->rcParams.maxBitRate      = bps;

            NV_ENC_RECONFIGURE_PARAMS recfg = { NV_ENC_RECONFIGURE_PARAMS_VER };
            recfg.reInitEncodeParams = initParams;
            NVENCSTATUS rc = nv.nvEncReconfigureEncoder(hEncoder, &recfg);
            if (rc == NV_ENC_SUCCESS)
                cout << "[abr] bitrate -> " << (bps / 1000) << " kbps\n";
            else
                cerr << "[abr] reconfigure failed: " << rc << "\n";
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> pDesktopResource;

        // Block until the desktop actually changes (event-driven), up to the heartbeat.
        HRESULT acquireResult = pDuplication->AcquireNextFrame(ACQUIRE_TIMEOUT_MS, &frameInfo, &pDesktopResource);

        bool haveNew = SUCCEEDED(acquireResult);
        if (!haveNew && acquireResult != DXGI_ERROR_WAIT_TIMEOUT) {
            cerr << "Lost DXGI capture. Needs reinitialization." << endl;
            break;
        }

        if (haveNew) {
            ComPtr<ID3D11Texture2D> pAcquiredTex;
            pDesktopResource.As(&pAcquiredTex);
            pContext->CopyResource(pEncodeTexture.Get(), pAcquiredTex.Get());
            pDuplication->ReleaseFrame();
            haveCaptured = true;
        }

        // What to emit this iteration:
        //   new desktop content -> always send
        //   static screen       -> send only to service a forced keyframe (PLI / new client)
        bool idrRequested = forceIDR.load();
        if (!haveCaptured)              continue; // nothing captured yet
        if (!haveNew && !idrRequested) continue; // static screen, nothing to do -> block again

        // 60 fps cap: never emit faster than the target interval. On a 60 Hz display frames
        // already arrive ~16.6 ms apart, so this only bites on higher-refresh displays.
        auto sinceSend = std::chrono::steady_clock::now() - lastSend;
        if (haveNew && !idrRequested && sinceSend < minFrameInterval) {
            std::this_thread::sleep_for(minFrameInterval - sinceSend);
        }

        // --- Telemetry: mark start of this frame's pipeline ---
        auto tCap0 = std::chrono::steady_clock::now();
        long long dtUs = usSince(prevCap0, tCap0); // real inter-frame interval (content-driven now)
        prevCap0 = tCap0;

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
        if (idrRequested) {
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
            lastSend = tReady; // for the 60 fps cap

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