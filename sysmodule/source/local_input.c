#include "local_input.h"

#include <stdlib.h>
#include <string.h>

#define LOCAL_HDLS_WORK_SIZE 0x1000u

typedef struct {
    bool service_open;
    bool work_attached;
    bool device_attached;
    bool keyboard_active;
    bool touch_active;
    void *work;
    HiddbgHdlsSessionId session;
    HiddbgHdlsHandle handle;
} LocalInput;

static LocalInput local;

static s32 stick_value(uint8_t value, bool invert) {
    s32 result=((s32)value-128)*256;
    if(invert) result=-result;
    if(result>32767) result=32767;
    if(result< -32768) result=-32768;
    return result;
}

static u64 gamepad_buttons(const SwitchGamepadReport *report) {
    u64 buttons=0;
    if(report->buttons&BIT(0)) buttons|=HidNpadButton_Y;
    if(report->buttons&BIT(1)) buttons|=HidNpadButton_B;
    if(report->buttons&BIT(2)) buttons|=HidNpadButton_A;
    if(report->buttons&BIT(3)) buttons|=HidNpadButton_X;
    if(report->buttons&BIT(4)) buttons|=HidNpadButton_L;
    if(report->buttons&BIT(5)) buttons|=HidNpadButton_R;
    if(report->buttons&BIT(6)) buttons|=HidNpadButton_ZL;
    if(report->buttons&BIT(7)) buttons|=HidNpadButton_ZR;
    if(report->buttons&BIT(8)) buttons|=HidNpadButton_Minus;
    if(report->buttons&BIT(9)) buttons|=HidNpadButton_Plus;
    if(report->buttons&BIT(10)) buttons|=HidNpadButton_StickL;
    if(report->buttons&BIT(11)) buttons|=HidNpadButton_StickR;
    if(report->buttons&SWITCH_GAMEPAD_HOME) buttons|=HiddbgNpadButton_Home;
    if(report->buttons&SWITCH_GAMEPAD_CAPTURE) buttons|=HiddbgNpadButton_Capture;

    switch(report->hat) {
        case 0: buttons|=HidNpadButton_Up;break;
        case 1: buttons|=HidNpadButton_Up|HidNpadButton_Right;break;
        case 2: buttons|=HidNpadButton_Right;break;
        case 3: buttons|=HidNpadButton_Right|HidNpadButton_Down;break;
        case 4: buttons|=HidNpadButton_Down;break;
        case 5: buttons|=HidNpadButton_Down|HidNpadButton_Left;break;
        case 6: buttons|=HidNpadButton_Left;break;
        case 7: buttons|=HidNpadButton_Left|HidNpadButton_Up;break;
        default: break;
    }
    return buttons;
}

static HiddbgHdlsState gamepad_state(const SwitchGamepadReport *report) {
    HiddbgHdlsState state={0};
    state.battery_level=4;
    state.flags=1;
    state.buttons=gamepad_buttons(report);
    state.analog_stick_l.x=stick_value(report->lx,false);
    state.analog_stick_l.y=stick_value(report->ly,true);
    state.analog_stick_r.x=stick_value(report->rx,false);
    state.analog_stick_r.y=stick_value(report->ry,true);
    return state;
}

static u64 keyboard_modifiers(uint8_t modifier) {
    u64 result=0;
    if(modifier&(BIT(0)|BIT(4))) result|=HidKeyboardModifier_Control;
    if(modifier&(BIT(1)|BIT(5))) result|=HidKeyboardModifier_Shift;
    if(modifier&BIT(2)) result|=HidKeyboardModifier_LeftAlt;
    if(modifier&BIT(6)) result|=HidKeyboardModifier_RightAlt;
    if(modifier&(BIT(3)|BIT(7))) result|=HidKeyboardModifier_Gui;
    return result;
}

static void teardown(void) {
    if(local.touch_active) (void)hiddbgUnsetTouchScreenAutoPilotState();
    if(local.keyboard_active) (void)hiddbgUnsetKeyboardAutoPilotState();
    if(local.device_attached) (void)hiddbgDetachHdlsVirtualDevice(local.handle);
    if(local.work_attached) (void)hiddbgReleaseHdlsWorkBuffer(local.session);
    if(local.service_open) hiddbgExit();
    free(local.work);
    memset(&local,0,sizeof(local));
}

bool local_input_begin(Result *out_result) {
    if(out_result) *out_result=0;
    if(local_input_ready()) return true;
    teardown();

    Result rc=hiddbgInitialize();
    if(R_FAILED(rc)) goto fail;
    local.service_open=true;

    local.work=aligned_alloc(LOCAL_HDLS_WORK_SIZE,LOCAL_HDLS_WORK_SIZE);
    if(!local.work) { rc=MAKERESULT(Module_Libnx,LibnxError_OutOfMemory);goto fail; }
    memset(local.work,0,LOCAL_HDLS_WORK_SIZE);

    rc=hiddbgAttachHdlsWorkBuffer(&local.session,local.work,LOCAL_HDLS_WORK_SIZE);
    if(R_FAILED(rc)) goto fail;
    local.work_attached=true;

    HiddbgHdlsDeviceInfo device={0};
    device.deviceType=HidDeviceType_FullKey3;
    device.npadInterfaceType=HidNpadInterfaceType_Bluetooth;
    device.singleColorBody=RGBA8_MAXALPHA(64,128,255);
    device.singleColorButtons=RGBA8_MAXALPHA(20,20,20);
    device.colorLeftGrip=RGBA8_MAXALPHA(64,128,255);
    device.colorRightGrip=RGBA8_MAXALPHA(64,128,255);
    rc=hiddbgAttachHdlsVirtualDevice(&local.handle,&device);
    if(R_FAILED(rc)) goto fail;
    local.device_attached=true;

    SwitchGamepadReport neutral={
        .buttons=SWITCH_GAMEPAD_NEUTRAL,.hat=SWITCH_GAMEPAD_HAT_NONE,
        .lx=SWITCH_GAMEPAD_STICK_CENTER,.ly=SWITCH_GAMEPAD_STICK_CENTER,
        .rx=SWITCH_GAMEPAD_STICK_CENTER,.ry=SWITCH_GAMEPAD_STICK_CENTER
    };
    HiddbgHdlsState state=gamepad_state(&neutral);
    rc=hiddbgSetHdlsState(local.handle,&state);
    if(R_FAILED(rc)) goto fail;
    return true;

fail:
    if(out_result) *out_result=rc;
    teardown();
    return false;
}

bool local_input_ready(void) {
    return local.service_open && local.work_attached && local.device_attached;
}

Result local_input_set_gamepad(const SwitchGamepadReport *report) {
    if(!report || !local_input_ready()) return KERNELRESULT(InvalidState);
    HiddbgHdlsState state=gamepad_state(report);
    return hiddbgSetHdlsState(local.handle,&state);
}

Result local_input_set_keyboard(uint8_t modifier,uint8_t key) {
    if(!local_input_ready()) return KERNELRESULT(InvalidState);
    if(!modifier && !key) {
        if(!local.keyboard_active) return 0;
        Result rc=hiddbgUnsetKeyboardAutoPilotState();
        if(R_SUCCEEDED(rc)) local.keyboard_active=false;
        return rc;
    }
    HiddbgKeyboardAutoPilotState state={0};
    state.modifiers=keyboard_modifiers(modifier);
    state.keys[key/64]|=BITL(key%64);
    Result rc=hiddbgSetKeyboardAutoPilotState(&state);
    if(R_SUCCEEDED(rc)) local.keyboard_active=true;
    return rc;
}

Result local_input_set_touch(bool down,uint16_t x,uint16_t y) {
    if(!local_input_ready() || x>1279 || y>719) return KERNELRESULT(InvalidState);
    if(!down) {
        if(!local.touch_active) return 0;
        Result rc=hiddbgUnsetTouchScreenAutoPilotState();
        if(R_SUCCEEDED(rc)) local.touch_active=false;
        return rc;
    }
    HidTouchState state={0};
    state.finger_id=0;state.x=x;state.y=y;state.diameter_x=8;state.diameter_y=8;
    Result rc=hiddbgSetTouchScreenAutoPilotState(&state,1);
    if(R_SUCCEEDED(rc)) local.touch_active=true;
    return rc;
}

Result local_input_neutral(void) {
    if(!local_input_ready()) return KERNELRESULT(InvalidState);
    Result keyboard_rc=local_input_set_keyboard(0,0);
    SwitchGamepadReport neutral={
        .buttons=SWITCH_GAMEPAD_NEUTRAL,.hat=SWITCH_GAMEPAD_HAT_NONE,
        .lx=SWITCH_GAMEPAD_STICK_CENTER,.ly=SWITCH_GAMEPAD_STICK_CENTER,
        .rx=SWITCH_GAMEPAD_STICK_CENTER,.ry=SWITCH_GAMEPAD_STICK_CENTER
    };
    Result gamepad_rc=local_input_set_gamepad(&neutral);
    Result touch_rc=local_input_set_touch(false,0,0);
    if(R_FAILED(keyboard_rc)) return keyboard_rc;
    if(R_FAILED(gamepad_rc)) return gamepad_rc;
    return touch_rc;
}

void local_input_end(void) {
    if(local_input_ready()) (void)local_input_neutral();
    teardown();
}

