#pragma once
#include <cstdint>


//need to be together
#pragma pack(push, 1)
struct TouchPoint
{
    int16_t x;
    int16_t y;
    int8_t id;
    int8_t state; // 0 : none , 1 : down/move , 2 : up
    int16_t pressure;
};
struct InputPacket {
    int16_t    lx, ly;       
    int16_t    dx, dy;       
    TouchPoint points[5];    
    uint8_t    buttons;      // bitmask: bit0=A, 1=B, 2=X, 3=Y
                             //         bit4=LB, 5=RB, 6=start, 7=select
};
#pragma pack(pop)

static_assert(sizeof(TouchPoint)  == 8,  "TouchPoint layout mismatch");
static_assert(sizeof(InputPacket) == 49, "InputPacket layout mismatch");