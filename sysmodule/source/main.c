#include <switch.h>

#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>

#include "network_input.h"
#include "usb_gamepad.h"
#include "usb_keyboard.h"

/* Keep this boot2 sysmodule small enough to coexist with am and other custom
   sysmodules. A sys-botbase-sized heap exhausted the global boot resource pool. */
#define INNER_HEAP_SIZE (1U * 1024U * 1024U)

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

static uint8_t inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void) {
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    rc = fsdevMountSdmc();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    /* Keep SM available: usb:ds and BSD are initialized and may be recovered
       from the main loop after __appInit() returns. */
}

void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    smExit();
}

static uint64_t now_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000U;
}

static void prepare_data_directory(void) {
    if (mkdir("sdmc:/switch", 0777) != 0 && errno != EEXIST)
        return;
    (void)mkdir("sdmc:/switch/RecurCIV", 0777);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    prepare_data_directory();

    /* Networking must not wait for usb:ds. A foreground NRO may own USB for
       minutes, but port 6002 and the Switch 1 hid:dbg target can still work. */
    (void)network_input_init();
    bool usb_online = false;
    uint64_t usb_retry_due = 0;

    for (;;) {
        uint64_t now = now_ms();

        if (!usb_online && now >= usb_retry_due) {
            Result usb_rc = keyboard_init();
            Result gamepad_rc = R_SUCCEEDED(usb_rc) ? gamepad_init() : usb_rc;
            if (R_SUCCEEDED(usb_rc) && R_SUCCEEDED(gamepad_rc)) {
                usb_online = true;
                keyboard_log("RecurCIV sysmodule USB acquired; TCP port=%u remains active",
                             NETWORK_INPUT_PORT);
            } else {
                keyboard_log("Sysmodule USB unavailable: keyboard=0x%08lx gamepad=0x%08lx; port %u remains active; retrying in 5 seconds",
                             (unsigned long)usb_rc, (unsigned long)gamepad_rc,
                             NETWORK_INPUT_PORT);
                keyboard_exit();
                usb_retry_due = now + 5000;
            }
        }

        if (usb_online) {
            keyboard_poll(now);
            gamepad_poll(now);
        }
        network_input_poll(now, true);

        if (usb_online && keyboard_idle_due(now))
            (void)keyboard_send(0, 0, now);

        svcSleepThread(1000000LL);
    }
}
