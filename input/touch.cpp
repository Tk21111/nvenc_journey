#define _WIN32_WINNT 0x0602
#include "touch.hpp"
#include <iostream>
#include <algorithm>
#include <winuser.h>

bool Touch::Init(int screenW , int screenH) 
{
    m_screenW = screenW;
    m_screenH = screenH;

    if (!InitializeTouchInjection(MAX_POINTS, TOUCH_FEEDBACK_DEFAULT)) {
        std::cerr << "[touch] InitializeTouchInjection failed — "
                  << "error: " << GetLastError() << "\n";
        return false;
    }

    m_ready = true;
    return true;
}

void Touch::Update(const TouchPoint (&points)[MAX_POINTS])
{
    if (!m_ready) return;
 
    for (int i = 0; i < MAX_POINTS; i++) {
        const TouchPoint& tp = points[i];
        if (tp.id >= MAX_POINTS) continue;  
 
        bool isStarting = false;
 
        if (tp.state == 1 && !m_activePointers[tp.id]) {
            // First contact for this finger
            isStarting = true;
            m_activePointers[tp.id] = true;
        } else if (tp.state == 2) {
            // Finger lifted
            m_activePointers[tp.id] = false;
        }
 
        InjectPoint(tp, isStarting);
    }
}

void Touch::InjectPoint(const TouchPoint& tp, bool isStarting) {
    float x = std::clamp(
        tp.x * (float)m_screenW / INT16_MAX,
        2.0f, (float)(m_screenW  - 2)
    );
    float y = std::clamp(
        tp.y * (float)m_screenH / INT16_MAX,
        2.0f, (float)(m_screenH - 2)
    );

    POINTER_TOUCH_INFO contact{};
    contact.pointerInfo.pointerType = PT_TOUCH;
    contact.pointerInfo.pointerId = tp.id;          // ID for the first finger
    contact.pointerInfo.ptPixelLocation.x = x;
    contact.pointerInfo.ptPixelLocation.y = y;

    // Determine the action flags
    if (isStarting && tp.state == 1) {
        contact.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    } else if (tp.state == 1) {
        contact.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    } else if (tp.state == 2) {
        contact.pointerInfo.pointerFlags = POINTER_FLAG_UP;
    } else {
        return; // Nothing to do for state 0
    }

    contact.touchFlags = TOUCH_FLAG_NONE;
    contact.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    contact.rcContact.left = x - 2;
    contact.rcContact.right = x + 2;
    contact.rcContact.top = y - 2;
    contact.rcContact.bottom = y + 2;
    contact.orientation = 90;
    contact.pressure = (uint32_t)(tp.pressure / 32767.0f * 1024);

    if (!InjectTouchInput(1, &contact)) {
        // Useful for debugging why injection fails
        std::cerr << "Injection failed. Error: " << GetLastError() << std::endl;
    }
}