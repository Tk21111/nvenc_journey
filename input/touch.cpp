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

    // Scale factor: normalized-delta -> pixel-delta, then reduced by TOUCH_SENS.
    const float sxx = ((float)m_screenW / INT16_MAX) * TOUCH_SENS;
    const float syy = ((float)m_screenH / INT16_MAX) * TOUCH_SENS;

    for (int i = 0; i < MAX_POINTS; i++) {
        const TouchPoint& tp = points[i];
        if (tp.id < 0 || tp.id >= MAX_POINTS) continue;
        PointerState& ps = m_ptr[tp.id];

        if (tp.state == 1 && !ps.active) {
            // First contact: anchor the virtual pointer WHERE the finger actually landed
            // (so taps hit the right spot and multi-touch keeps its spread), then from here
            // we only move by the *scaled* finger delta.
            ps.active   = true;
            ps.lastRawX = tp.x;
            ps.lastRawY = tp.y;
            ps.virtX = std::clamp(tp.x * (float)m_screenW / INT16_MAX, 2.0f, (float)(m_screenW - 2));
            ps.virtY = std::clamp(tp.y * (float)m_screenH / INT16_MAX, 2.0f, (float)(m_screenH - 2));
            InjectPoint(tp.id, ps.virtX, ps.virtY, tp.pressure, 1 /*down*/);
        }
        else if (tp.state == 1 && ps.active) {
            // Move: advance the virtual pointer by the *scaled* finger delta (relative look).
            ps.virtX += (float)(tp.x - ps.lastRawX) * sxx;
            ps.virtY += (float)(tp.y - ps.lastRawY) * syy;
            ps.lastRawX = tp.x;
            ps.lastRawY = tp.y;
            ps.virtX = std::clamp(ps.virtX, 2.0f, (float)(m_screenW - 2));
            ps.virtY = std::clamp(ps.virtY, 2.0f, (float)(m_screenH - 2));
            InjectPoint(tp.id, ps.virtX, ps.virtY, tp.pressure, 2 /*update*/);
        }
        else if (tp.state == 2 && ps.active) {
            ps.active = false;
            InjectPoint(tp.id, ps.virtX, ps.virtY, tp.pressure, 3 /*up*/);
        }
    }
}

void Touch::InjectPoint(int id, float x, float y, int16_t pressure, int action) {
    POINTER_TOUCH_INFO contact{};
    contact.pointerInfo.pointerType = PT_TOUCH;
    contact.pointerInfo.pointerId = (UINT32)id;
    contact.pointerInfo.ptPixelLocation.x = (LONG)x;
    contact.pointerInfo.ptPixelLocation.y = (LONG)y;

    if (action == 1) {        // down
        contact.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    } else if (action == 2) { // update / move
        contact.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    } else if (action == 3) { // up
        contact.pointerInfo.pointerFlags = POINTER_FLAG_UP;
    } else {
        return;
    }

    contact.touchFlags = TOUCH_FLAG_NONE;
    contact.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    contact.rcContact.left   = (LONG)x - 2;
    contact.rcContact.right  = (LONG)x + 2;
    contact.rcContact.top    = (LONG)y - 2;
    contact.rcContact.bottom = (LONG)y + 2;
    contact.orientation = 90;
    contact.pressure = (uint32_t)(pressure / 32767.0f * 1024);

    if (!InjectTouchInput(1, &contact)) {
        std::cerr << "Injection failed. Error: " << GetLastError() << std::endl;
    }
}