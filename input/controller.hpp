#pragma once

#include <windows.h> 
#include <ViGEm/Client.h>
#include "../protocol/packet.hpp"

class Controller
{
private:
    PVIGEM_CLIENT m_client = nullptr;
    PVIGEM_TARGET m_pad = nullptr;
    XUSB_REPORT m_report {};
    bool m_ready = false;
    
public:
    Controller() = default;
    ~Controller() { Shutdown();};

    bool Init();

    void Update(const InputPacket& p);

    void Shutdown();

    bool isReady() const { return m_ready;};
};


