#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NETWORK_INPUT_PORT 6001

typedef struct {
    bool enabled;
    bool link_usable;
    bool listener_ready;
    bool client_connected;
    bool session_active;
    bool cleanup_active;
    unsigned port;
    unsigned listener_generation;
    unsigned restart_failures;
    unsigned key_queue;
    unsigned pad_queue;
    unsigned touch_queue;
    unsigned keepalive_failures;
    unsigned long keepalives_sent;
    uint64_t session_age_ms;
    uint64_t activity_age_ms;
    char phase[20];
    char ip[32];
    char active_target[16];
    char last_target[16];
    char client[64];
    char last_command[80];
    char last_recovery[112];
} NetworkInputStatus;

/* The listener is always non-blocking. A client can acquire USB output only
   while allow_session is true; once acquired, network_input_active() remains
   true through neutral cleanup so local producers cannot race the session. */
bool network_input_init(void);
void network_input_poll(uint64_t now_ms, bool allow_session);
bool network_input_active(void);
void network_input_get_status(uint64_t now_ms, NetworkInputStatus *out);
/* Recreate the socket service/listener after sleep, applet suspension, or an
   unrecoverable listener error. Active HID ownership enters neutral cleanup
   before another client can authenticate. Failed binds retry from poll(). */
void network_input_recover(uint64_t now_ms, const char *reason);
void network_input_request_stop(void);
void network_input_shutdown(void);

