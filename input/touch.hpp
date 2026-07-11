#pragma once
 
#include <windows.h>
#include <winuser.h>

#include "../protocol/packet.hpp"

class Touch
{
public:
    //known it compile time wtf do i have to know this
    static constexpr int MAX_POINTS = 5;

    // Relative "look-pad" sensitivity. 1.0 == same magnitude as a raw 1:1 screen map;
    // lower == the camera moves less than your finger. Tune this one number by feel.
    static constexpr float TOUCH_SENS = 0.4f;

    Touch()  = default;
    ~Touch() = default;

    bool Init(int screenW = 1920 , int screenH = 1080);

    void Update(const TouchPoint (&points)[MAX_POINTS]);
private:
    struct PointerState {
        bool  active = false;
        float lastRawX = 0, lastRawY = 0; // last normalized input we saw
        float virtX = 0,    virtY = 0;    // current virtual output position (pixels)
    };
    PointerState m_ptr[MAX_POINTS];
    int m_screenW = 1920;
    int m_screenH = 1080;
    bool m_ready = false;
    void InjectPoint(int id, float x, float y, int16_t pressure, int action);
};
