#include "controller.hpp"
#include <iostream>
#include <cstring>


bool Controller::Init() 
{
    m_client = vigem_alloc();
    if(!m_client) {
        std::cerr << "[ctrl] vigem_alloc_fail\n";
        return false;
    }
    if(vigem_connect(m_client) != VIGEM_ERROR_NONE) {
        std::cerr << "[ctrl] vigem_connect failed — "
                     "is ViGEm Bus driver installed?\n";
        vigem_free(m_client);
        m_client = nullptr;
        return false;
    }

    m_pad = vigem_target_x360_alloc();
    if (vigem_target_add(m_client, m_pad) != VIGEM_ERROR_NONE) {
        std::cerr << "[ctrl] vigem_target_add failed\n";
        return false;
    }

    memset(&m_report , 0 , sizeof(m_report));

    m_ready = true;
    std::cout << "[ctrl] virtual Xbox 360 controller ready\n";
    return true;
}

void Controller::Update(const InputPacket& p)
{
    if(!m_ready) return;

    m_report.sThumbLX = p.lx;
    m_report.sThumbLY = p.ly;
    m_report.sThumbRX = p.dx; 
    m_report.sThumbRY = p.dy; 

    m_report.wButtons = 0;
    if (p.buttons & (1 << 0)) m_report.wButtons |= XUSB_GAMEPAD_A;
    if (p.buttons & (1 << 1)) m_report.wButtons |= XUSB_GAMEPAD_B;
    if (p.buttons & (1 << 2)) m_report.wButtons |= XUSB_GAMEPAD_X;
    if (p.buttons & (1 << 3)) m_report.wButtons |= XUSB_GAMEPAD_Y;
    if (p.buttons & (1 << 4)) m_report.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (p.buttons & (1 << 5)) m_report.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (p.buttons & (1 << 6)) m_report.wButtons |= XUSB_GAMEPAD_START;
    if (p.buttons & (1 << 7)) m_report.wButtons |= XUSB_GAMEPAD_BACK;

    vigem_target_x360_update(m_client, m_pad, m_report);
}

void Controller::Shutdown()
{
    if(m_pad && m_client) {
        vigem_target_remove(m_client, m_pad);
        vigem_target_free(m_pad);
        m_pad = nullptr;
    }
    if (m_client) {
        vigem_disconnect(m_client);
        vigem_free(m_client);
        m_client = nullptr;
    }

    m_ready = false;
}