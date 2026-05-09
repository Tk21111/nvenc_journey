#pragma once
 
#include <windows.h>
#include <winuser.h>

#include "../protocol/packet.hpp"

class Touch
{
public:
    //known it compile time wtf do i have to know this
    static constexpr int MAX_POINTS = 5;

    Touch()  = default;
    ~Touch() = default;

    bool Init(int screenW = 1920 , int screenH = 1080);

    void Update(const TouchPoint (&points)[MAX_POINTS]);
private:
    bool m_activePointers[MAX_POINTS] = {};
    int m_screenW = 1920;
    int m_screenH = 1080;
    bool m_ready = false;
    void InjectPoint(const TouchPoint& tp , bool isStarting);
};
