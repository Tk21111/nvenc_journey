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
    int16_t    lx, ly;       // left  stick
    int16_t    dx, dy;       // right stick
    TouchPoint points[5];    // 40 bytes
    uint16_t   buttons;      // bitmask, see below (16-bit so every Xbox button fits)
    uint8_t    lt, rt;       // analog triggers, 0..255
};
#pragma pack(pop)

// buttons bitmask layout (client and server MUST agree):
//   bit0=A   bit1=B   bit2=X   bit3=Y
//   bit4=LB  bit5=RB  bit6=Start(Menu)  bit7=Back(View)
//   bit8=DPadUp  bit9=DPadDown  bit10=DPadLeft  bit11=DPadRight
//   bit12=L3(LeftThumb)  bit13=R3(RightThumb)  bit14=Guide
enum InputButton : uint16_t {
    BTN_A = 1u<<0,  BTN_B = 1u<<1,  BTN_X = 1u<<2,  BTN_Y = 1u<<3,
    BTN_LB = 1u<<4, BTN_RB = 1u<<5, BTN_START = 1u<<6, BTN_BACK = 1u<<7,
    BTN_DUP = 1u<<8, BTN_DDOWN = 1u<<9, BTN_DLEFT = 1u<<10, BTN_DRIGHT = 1u<<11,
    BTN_L3 = 1u<<12, BTN_R3 = 1u<<13, BTN_GUIDE = 1u<<14,
};

static_assert(sizeof(TouchPoint)  == 8,  "TouchPoint layout mismatch");
static_assert(sizeof(InputPacket) == 52, "InputPacket layout mismatch");