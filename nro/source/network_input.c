#include "network_input.h"

#include "local_input.h"
#include "network_key.h"
#include "switch_gamepad.h"
#include "text.h"
#include "usb_gamepad.h"
#include "usb_keyboard.h"
#include "usb_touch.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define NETWORK_TOKEN_PATH "sdmc:/switch/NXInputRelay/network-token.txt"
#define NETWORK_TOKEN_LEGACY_PATH "sdmc:/switch/RecurCIV/network-token.txt"
#define NETWORK_LOG_PATH "sdmc:/switch/NXInputRelay/network-nro.log"
#define NETWORK_LOG_OLD_PATH "sdmc:/switch/NXInputRelay/network-nro.log.1"
#define NETWORK_LOG_MAX_BYTES (1024u*1024u)
#define NETWORK_LINE_CAPACITY 512
#define NETWORK_PAD_QUEUE_CAPACITY 128
#define NETWORK_KEY_QUEUE_CAPACITY 1024
#define NETWORK_TOUCH_QUEUE_CAPACITY 128
#define NETWORK_AUTH_TIMEOUT_MS 10000u
#define NETWORK_IDLE_TIMEOUT_MS 300000u
#define NETWORK_KEY_HOLD_MS 80u
#define NETWORK_CLICK_HOLD_MS 80u
#define NETWORK_TAP_HOLD_MS 80u
#define NETWORK_RESTART_MIN_MS 1000u
#define NETWORK_RESTART_MAX_MS 30000u
#define NETWORK_HEALTH_CHECK_MS 5000u
#define NETWORK_HEARTBEAT_MS 60000u
#define NETWORK_LINK_KEEPALIVE_MS 15000u
#define NETWORK_LINK_KEEPALIVE_BACKOFF_MS 60000u
#define NETWORK_LINK_FAILURE_LIMIT 3u

typedef enum {
    NET_DISABLED,
    NET_LISTENING,
    NET_AUTHENTICATING,
    NET_ACTIVE,
    NET_CLEANUP
} NetworkPhase;

typedef enum {
    NETWORK_TARGET_USB,
    NETWORK_TARGET_LOCAL
} NetworkTarget;

typedef struct {
    SwitchGamepadReport report;
    uint16_t hold_ms;
} PadEvent;

typedef struct {
    uint8_t modifier;
    uint8_t key;
    uint16_t hold_ms;
} KeyEvent;

typedef struct {
    bool down;
    uint16_t x,y,hold_ms;
} TouchEvent;

typedef struct {
    uint16_t bit;
    uint8_t dpad;
} ParsedButton;

typedef struct {
    NetworkPhase phase;
    NetworkTarget target;
    bool enabled;
    bool sockets_initialized;
    int listen_fd;
    int client_fd;
    uint64_t restart_due;
    unsigned restart_failures;
    unsigned listener_generation;
    uint64_t clock_now;
    uint64_t next_health_check;
    uint64_t next_health_log;
    uint32_t observed_ip;
    bool observed_ip_valid;
    bool observed_ip_set;
    bool link_usable;
    uint64_t next_link_keepalive;
    unsigned link_keepalive_failures;
    unsigned long link_keepalives_sent;
    char token[96];
    char line[NETWORK_LINE_CAPACITY];
    char client_address[64];
    char last_command[80];
    char last_recovery[112];
    size_t line_length;
    uint64_t auth_deadline;
    uint64_t last_activity;
    uint64_t session_started;
    NetworkTarget last_target;
    bool last_target_set;

    SwitchGamepadReport logical_pad;
    uint8_t logical_dpad;
    PadEvent pad_queue[NETWORK_PAD_QUEUE_CAPACITY];
    unsigned pad_head;
    unsigned pad_count;
    bool pad_waiting;
    uint16_t pad_hold_ms;
    uint64_t pad_due;

    KeyEvent key_queue[NETWORK_KEY_QUEUE_CAPACITY];
    unsigned key_head;
    unsigned key_count;
    bool key_waiting;
    uint16_t key_hold_ms;
    uint64_t key_due;
    bool keyboard_touched;
    uint32_t keyboard_failure_base;

    TouchEvent touch_queue[NETWORK_TOUCH_QUEUE_CAPACITY];
    unsigned touch_head;
    unsigned touch_count;
    bool touch_waiting;
    uint16_t touch_hold_ms;
    uint64_t touch_due;
    bool logical_touch_down;
    uint16_t logical_touch_x;
    uint16_t logical_touch_y;
    bool touch_touched;
    uint32_t touch_failure_base;

    bool cleanup_key_sent;
    bool cleanup_key_done;
    bool cleanup_pad_requested;
    bool cleanup_touch_sent;
    bool cleanup_touch_done;
} NetworkInput;

static NetworkInput network={.listen_fd=-1,.client_fd=-1};
static FILE *network_log_file;

static const char *phase_name(NetworkPhase phase) {
    static const char *names[]={"disabled","listening","authenticating","active","cleanup"};
    return phase>=NET_DISABLED && phase<=NET_CLEANUP?names[phase]:"unknown";
}

static const char *target_name(NetworkTarget target) {
    return target==NETWORK_TARGET_LOCAL?"switch1":"switch2";
}

static void open_network_log(void) {
    if(network_log_file) { fclose(network_log_file);network_log_file=NULL; }
    struct stat info;
    if(stat(NETWORK_LOG_PATH,&info)==0 && info.st_size>=(off_t)NETWORK_LOG_MAX_BYTES) {
        (void)remove(NETWORK_LOG_OLD_PATH);
        (void)rename(NETWORK_LOG_PATH,NETWORK_LOG_OLD_PATH);
    }
    network_log_file=fopen(NETWORK_LOG_PATH,"a");
    if(!network_log_file) keyboard_log("Network diagnostic log unavailable: fopen errno=%d",errno);
}

static void network_event(uint64_t now,const char *format, ...) {
    char message[512];
    va_list args;va_start(args,format);vsnprintf(message,sizeof(message),format,args);va_end(args);
    keyboard_log("NET %s",message);
    if(!network_log_file) return;
    int saved_errno=errno;
    if(fprintf(network_log_file,"[%10llu ms] %s\n",(unsigned long long)now,message)<0 || fflush(network_log_file)==EOF)
        keyboard_log("Network diagnostic log write failed errno=%d",errno);
    errno=saved_errno;
}

static SwitchGamepadReport neutral_pad(void) {
    return (SwitchGamepadReport){
        .buttons=SWITCH_GAMEPAD_NEUTRAL,
        .hat=SWITCH_GAMEPAD_HAT_NONE,
        .lx=SWITCH_GAMEPAD_STICK_CENTER,
        .ly=SWITCH_GAMEPAD_STICK_CENTER,
        .rx=SWITCH_GAMEPAD_STICK_CENTER,
        .ry=SWITCH_GAMEPAD_STICK_CENTER,
        .vendor=0
    };
}

static void close_client(void) {
    if(network.client_fd>=0) close(network.client_fd);
    network.client_fd=-1;
    network.line_length=0;
}

static void close_listener(void) {
    if(network.listen_fd>=0) close(network.listen_fd);
    network.listen_fd=-1;
}

static void close_socket_service(void) {
    close_client();
    close_listener();
    if(network.sockets_initialized) socketExit();
    network.sockets_initialized=false;
}

static uint32_t restart_delay(unsigned failures) {
    unsigned shift=failures?failures-1:0;
    if(shift>5) shift=5;
    uint32_t delay=NETWORK_RESTART_MIN_MS<<shift;
    return delay>NETWORK_RESTART_MAX_MS?NETWORK_RESTART_MAX_MS:delay;
}

static void schedule_listener_retry(uint64_t now,const char *operation,int error_number) {
    close_listener();
    if(network.sockets_initialized) socketExit();
    network.sockets_initialized=false;
    network.restart_failures++;
    uint32_t delay=restart_delay(network.restart_failures);
    network.restart_due=now+delay;
    snprintf(network.last_recovery,sizeof(network.last_recovery),"Listener %s failed (%d)",
        operation?operation:"restart",error_number);
    network_event(now,"listener %s failed code=%d phase=%s; retry in %lu ms",
        operation?operation:"restart",error_number,phase_name(network.phase),(unsigned long)delay);
}

static bool set_nonblocking(int fd) {
    int flags=fcntl(fd,F_GETFL,0);
    return flags>=0 && fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0;
}

static void reply(const char *message) {
    if(network.client_fd<0 || !message) return;
    ssize_t sent;
#ifdef MSG_NOSIGNAL
    sent=send(network.client_fd,message,strlen(message),MSG_NOSIGNAL);
#else
    sent=send(network.client_fd,message,strlen(message),0);
#endif
    if(sent<0 && errno!=EAGAIN && errno!=EWOULDBLOCK)
        network_event(network.clock_now,"client send failed fd=%d errno=%d",network.client_fd,errno);
    else if(sent>=0 && (size_t)sent<strlen(message))
        network_event(network.clock_now,"client send partial fd=%d sent=%ld expected=%lu",
            network.client_fd,(long)sent,(unsigned long)strlen(message));
}

static bool load_token(void) {
    const char *token_path=NETWORK_TOKEN_PATH;
    FILE *file=fopen(token_path,"r");
    if(!file) {
        token_path=NETWORK_TOKEN_LEGACY_PATH;
        file=fopen(token_path,"r");
        if(!file) {
            network_event(network.clock_now,"disabled: missing %s (legacy fallback %s also missing)",
                NETWORK_TOKEN_PATH,NETWORK_TOKEN_LEGACY_PATH);
            return false;
        }
        network_event(network.clock_now,"using legacy token path %s; move it to %s",
            NETWORK_TOKEN_LEGACY_PATH,NETWORK_TOKEN_PATH);
    }
    bool ok=fgets(network.token,sizeof(network.token),file)!=NULL;
    fclose(file);
    if(!ok) return false;
    size_t length=strcspn(network.token,"\r\n");
    network.token[length]=0;
    if(length<8 || length>64) {
        network_event(network.clock_now,"disabled: token must be 8-64 characters");
        return false;
    }
    for(size_t i=0;i<length;i++) {
        if(isspace((unsigned char)network.token[i]) || !isprint((unsigned char)network.token[i])) {
            network_event(network.clock_now,"disabled: token contains whitespace/control characters");
            return false;
        }
    }
    return true;
}

static bool token_matches(const char *candidate) {
    size_t expected=strlen(network.token),actual=candidate?strlen(candidate):0;
    unsigned difference=(unsigned)(expected^actual);
    size_t longest=expected>actual?expected:actual;
    for(size_t i=0;i<longest;i++) {
        unsigned a=i<expected?(unsigned char)network.token[i]:0;
        unsigned b=i<actual?(unsigned char)candidate[i]:0;
        difference|=a^b;
    }
    return difference==0;
}

static void clear_queues(void) {
    network.pad_head=network.pad_count=0;
    network.key_head=network.key_count=0;
    network.touch_head=network.touch_count=0;
}

static unsigned pad_space(void) { return NETWORK_PAD_QUEUE_CAPACITY-network.pad_count; }
static unsigned key_space(void) { return NETWORK_KEY_QUEUE_CAPACITY-network.key_count; }
static unsigned touch_space(void) { return NETWORK_TOUCH_QUEUE_CAPACITY-network.touch_count; }

static bool pad_push(SwitchGamepadReport report,uint16_t hold_ms) {
    if(!pad_space()) return false;
    unsigned tail=(network.pad_head+network.pad_count)%NETWORK_PAD_QUEUE_CAPACITY;
    network.pad_queue[tail]=(PadEvent){report,hold_ms};
    network.pad_count++;
    return true;
}

static bool key_push(uint8_t modifier,uint8_t key,uint16_t hold_ms) {
    if(!key_space()) return false;
    unsigned tail=(network.key_head+network.key_count)%NETWORK_KEY_QUEUE_CAPACITY;
    network.key_queue[tail]=(KeyEvent){modifier,key,hold_ms};
    network.key_count++;
    return true;
}

static bool touch_push(bool down,uint16_t x,uint16_t y,uint16_t hold_ms) {
    if(!touch_space()) return false;
    unsigned tail=(network.touch_head+network.touch_count)%NETWORK_TOUCH_QUEUE_CAPACITY;
    network.touch_queue[tail]=(TouchEvent){down,x,y,hold_ms};network.touch_count++;return true;
}

static void update_hat(SwitchGamepadReport *report,uint8_t dpad) {
    int x=!!(dpad&4)-!!(dpad&8);
    int y=!!(dpad&2)-!!(dpad&1);
    if(y<0) report->hat=x<0?7:x>0?1:0;
    else if(y>0) report->hat=x<0?5:x>0?3:4;
    else if(x) report->hat=x<0?6:2;
    else report->hat=SWITCH_GAMEPAD_HAT_NONE;
}

static bool parse_button(const char *name,ParsedButton *out) {
    if(!name || !out) return false;
    static const struct { const char *name;uint16_t bit; } buttons[]={
        {"Y",1u<<0},{"B",1u<<1},{"A",1u<<2},{"X",1u<<3},
        {"L",1u<<4},{"R",1u<<5},{"ZL",1u<<6},{"ZR",1u<<7},
        {"MINUS",1u<<8},{"PLUS",1u<<9},{"L3",1u<<10},{"R3",1u<<11},
        {"LSTICK",1u<<10},{"RSTICK",1u<<11},
        {"HOME",SWITCH_GAMEPAD_HOME},{"CAPTURE",SWITCH_GAMEPAD_CAPTURE}
    };
    for(size_t i=0;i<sizeof(buttons)/sizeof(buttons[0]);i++) {
        if(strcasecmp(name,buttons[i].name)==0) {
            *out=(ParsedButton){buttons[i].bit,0};return true;
        }
    }
    if(!strcasecmp(name,"DUP") || !strcasecmp(name,"DU") || !strcasecmp(name,"UP")) *out=(ParsedButton){0,1};
    else if(!strcasecmp(name,"DDOWN") || !strcasecmp(name,"DD") || !strcasecmp(name,"DOWN")) *out=(ParsedButton){0,2};
    else if(!strcasecmp(name,"DRIGHT") || !strcasecmp(name,"DR") || !strcasecmp(name,"RIGHT")) *out=(ParsedButton){0,4};
    else if(!strcasecmp(name,"DLEFT") || !strcasecmp(name,"DL") || !strcasecmp(name,"LEFT")) *out=(ParsedButton){0,8};
    else return false;
    return true;
}

static bool parse_number(const char *text,long minimum,long maximum,long *value) {
    if(!text || !*text) return false;
    char *end=NULL;errno=0;long parsed=strtol(text,&end,0);
    if(errno || !end || *end || parsed<minimum || parsed>maximum) return false;
    *value=parsed;return true;
}

static uint8_t axis_value(long value,bool invert) {
    if(invert) value=value==-32768?32767:-value;
    return (uint8_t)((value+32768)>>8);
}

static bool queue_pad_change(ParsedButton button,bool pressed) {
    SwitchGamepadReport next=network.logical_pad;
    uint8_t dpad=network.logical_dpad;
    if(button.bit) {
        if(pressed) next.buttons|=button.bit;else next.buttons&=(uint16_t)~button.bit;
    } else {
        if(pressed) dpad|=button.dpad;else dpad&=(uint8_t)~button.dpad;
        update_hat(&next,dpad);
    }
    if(!pad_push(next,0)) return false;
    network.logical_pad=next;network.logical_dpad=dpad;return true;
}

static bool queue_pad_click(ParsedButton button) {
    if(pad_space()<2) return false;
    SwitchGamepadReport pressed=network.logical_pad;
    if(button.bit) pressed.buttons|=button.bit;
    else update_hat(&pressed,(uint8_t)(network.logical_dpad|button.dpad));
    return pad_push(pressed,NETWORK_CLICK_HOLD_MS) && pad_push(network.logical_pad,0);
}

static bool queue_key_tap(uint8_t modifier,uint8_t key) {
    if(key_space()<2) return false;
    return key_push(modifier,key,NETWORK_KEY_HOLD_MS) && key_push(0,0,NETWORK_KEY_HOLD_MS);
}

static int hex_digit(char c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}

static bool queue_hex_text(const char *hex) {
    size_t digits=hex?strlen(hex):0;
    if(!digits || digits%2) return false;
    size_t length=digits/2;
    char *text=malloc(length?length:1);
    if(!text) return false;
    for(size_t i=0;i<length;i++) {
        int high=hex_digit(hex[i*2]),low=hex_digit(hex[i*2+1]);
        if(high<0 || low<0) { free(text);return false; }
        text[i]=(char)((high<<4)|low);
    }
    TextPlan plan={0};TextError error={0};
    bool ok=text_plan(text,length,false,&plan,&error);
    free(text);
    if(!ok || plan.count>key_space()/2) { text_plan_free(&plan);return false; }
    for(size_t i=0;i<plan.count;i++) {
        if(!queue_key_tap(plan.strokes[i].modifier,plan.strokes[i].key)) { ok=false;break; }
    }
    text_plan_free(&plan);return ok;
}

static void begin_cleanup(const char *reason) {
    if(network.phase==NET_CLEANUP || network.phase==NET_LISTENING || network.phase==NET_DISABLED) return;
    if(reason) network_event(network.clock_now,"session stopping: %s phase=%s client_fd=%d",
        reason,phase_name(network.phase),network.client_fd);
    if(network.phase==NET_AUTHENTICATING) {
        close_client();network.phase=NET_LISTENING;return;
    }
    close_client();
    clear_queues();
    network.phase=NET_CLEANUP;
    network.cleanup_key_sent=false;
    network.cleanup_key_done=!network.keyboard_touched;
    network.cleanup_pad_requested=false;
    network.cleanup_touch_sent=false;
    network.cleanup_touch_done=!network.touch_touched;
}

void network_input_recover(uint64_t now,const char *reason) {
    if(!network.enabled) return;
    network.clock_now=now;
    if(network.phase==NET_ACTIVE || network.phase==NET_AUTHENTICATING) begin_cleanup(reason);
    else close_client();
    close_listener();
    if(network.sockets_initialized) socketExit();
    network.sockets_initialized=false;
    network.restart_failures=0;
    network.restart_due=now;
    snprintf(network.last_recovery,sizeof(network.last_recovery),"%s",reason?reason:"socket fault");
    if(network.phase!=NET_CLEANUP) network.phase=NET_LISTENING;
    network_event(now,"listener recovery requested: %s phase=%s",
        reason?reason:"socket fault",phase_name(network.phase));
}

static bool start_session(bool allowed,uint64_t now,NetworkTarget target) {
    const KeyboardStatus *keyboard=keyboard_status();
    if(!allowed) {
        reply("ERR busy\n");close_client();network.phase=NET_LISTENING;return false;
    }
    if(target==NETWORK_TARGET_USB) {
        if(!keyboard->ready || keyboard->transfer_pending || !gamepad_ready() ||
           gamepad_exclusive_status()!=GAMEPAD_EXCLUSIVE_OFF || !gamepad_exclusive_begin()) {
            reply("ERR busy\n");close_client();network.phase=NET_LISTENING;return false;
        }
    } else {
        Result rc=0;
        if(!local_input_begin(&rc)) {
            char message[64];snprintf(message,sizeof(message),"ERR local-unavailable 0x%08lx\n",(unsigned long)rc);
            reply(message);close_client();network.phase=NET_LISTENING;
            network_event(now,"local input initialization failed rc=0x%08lx",(unsigned long)rc);
            return false;
        }
    }
    network.target=target;
    network.last_target=target;network.last_target_set=true;
    clear_queues();
    network.logical_pad=neutral_pad();network.logical_dpad=0;
    network.pad_waiting=false;network.pad_due=0;
    network.key_waiting=false;network.key_due=0;
    network.keyboard_touched=false;
    network.keyboard_failure_base=target==NETWORK_TARGET_USB?keyboard->reports_failed:0;
    network.touch_waiting=false;network.touch_due=0;network.logical_touch_down=false;
    network.logical_touch_x=network.logical_touch_y=0;network.touch_touched=false;
    network.touch_failure_base=target==NETWORK_TARGET_USB?usb_touch_status()->reports_failed:0;
    network.cleanup_key_sent=network.cleanup_key_done=network.cleanup_pad_requested=false;
    network.cleanup_touch_sent=network.cleanup_touch_done=false;
    network.last_activity=now;network.session_started=now;network.phase=NET_ACTIVE;
    char greeting[96];snprintf(greeting,sizeof(greeting),"OK NXInputRelay network input v2 target=%s\n",target_name(target));
    reply(greeting);
    network_event(now,"authenticated; target=%s session acquired client_fd=%d",target_name(target),network.client_fd);
    return true;
}

static void command_status(void) {
    char message[224];const KeyboardStatus *keyboard=keyboard_status();
    bool local_ready=local_input_ready();
    snprintf(message,sizeof(message),"OK active=1 target=%s keyboard=%u gamepad=%u touch=%u local=%u keyq=%u padq=%u touchq=%u\n",
        target_name(network.target),keyboard->ready?1u:0u,gamepad_ready()?1u:0u,
        network.target==NETWORK_TARGET_LOCAL?local_ready:usb_touch_ready(),local_ready?1u:0u,
        network.key_count,network.pad_count,network.touch_count);
    reply(message);
}

static void handle_active_command(char *line,uint64_t now) {
    char *save=NULL;char *argv[40];unsigned argc=0;
    for(char *part=strtok_r(line," \t",&save);part && argc<40;part=strtok_r(NULL," \t",&save)) argv[argc++]=part;
    if(!argc) return;
    if(argc>1 && (!strcasecmp(argv[0],"click") || !strcasecmp(argv[0],"press") ||
       !strcasecmp(argv[0],"release") || !strcasecmp(argv[0],"setStick") ||
       !strcasecmp(argv[0],"tap") || !strcasecmp(argv[0],"touchDown") ||
       !strcasecmp(argv[0],"tapDown") || !strcasecmp(argv[0],"touchMove")))
        snprintf(network.last_command,sizeof(network.last_command),"%s %s",argv[0],argv[1]);
    else snprintf(network.last_command,sizeof(network.last_command),"%s",argv[0]);
    network.last_activity=now;
    if(!strcasecmp(argv[0],"ping")) { reply("OK pong\n");return; }
    if(!strcasecmp(argv[0],"getVersion")) { reply("NXInputRelay-network-2\n");return; }
    if(!strcasecmp(argv[0],"status")) { command_status();return; }
    if(!strcasecmp(argv[0],"quit") || !strcasecmp(argv[0],"disconnect")) {
        reply("OK stopping\n");begin_cleanup("client requested stop");return;
    }
    if(!strcasecmp(argv[0],"neutral") || !strcasecmp(argv[0],"releaseAll")) {
        clear_queues();network.logical_pad=neutral_pad();network.logical_dpad=0;
        bool ok=pad_push(network.logical_pad,0) && key_push(0,0,NETWORK_KEY_HOLD_MS);
        if(ok && network.logical_touch_down)
            ok=touch_push(false,network.logical_touch_x,network.logical_touch_y,0);
        network.logical_touch_down=false;
        if(ok) network.keyboard_touched=true;
        reply(ok?"OK\n":"ERR queue-full\n");return;
    }
    if((!strcasecmp(argv[0],"click") || !strcasecmp(argv[0],"press") || !strcasecmp(argv[0],"release")) && argc==2) {
        ParsedButton button;
        if(!parse_button(argv[1],&button)) { reply("ERR invalid-button\n");return; }
        bool ok=!strcasecmp(argv[0],"click")?queue_pad_click(button):queue_pad_change(button,!strcasecmp(argv[0],"press"));
        reply(ok?"OK\n":"ERR queue-full\n");return;
    }
    if(!strcasecmp(argv[0],"setStick") && argc==4) {
        long x=0,y=0;
        if((strcasecmp(argv[1],"LEFT") && strcasecmp(argv[1],"RIGHT")) ||
           !parse_number(argv[2],-32768,32767,&x) || !parse_number(argv[3],-32768,32767,&y)) {
            reply("ERR usage: setStick LEFT|RIGHT -32768..32767 -32768..32767\n");return;
        }
        SwitchGamepadReport next=network.logical_pad;
        if(!strcasecmp(argv[1],"LEFT")) { next.lx=axis_value(x,false);next.ly=axis_value(y,true); }
        else { next.rx=axis_value(x,false);next.ry=axis_value(y,true); }
        if(!pad_push(next,0)) { reply("ERR queue-full\n");return; }
        network.logical_pad=next;reply("OK\n");return;
    }
    bool touch_available=network.target==NETWORK_TARGET_LOCAL?local_input_ready():usb_touch_ready();
    if(!strcasecmp(argv[0],"tap") && (argc==3 || argc==4)) {
        long x=0,y=0,hold=NETWORK_TAP_HOLD_MS;
        if(!touch_available) { reply("ERR touch-unavailable\n");return; }
        if(network.logical_touch_down) { reply("ERR touch-already-down\n");return; }
        if(!parse_number(argv[1],0,1279,&x) || !parse_number(argv[2],0,719,&y) ||
           (argc==4 && !parse_number(argv[3],1,5000,&hold))) {
            reply("ERR usage: tap 0..1279 0..719 [1..5000ms]\n");return;
        }
        if(touch_space()<2 || !touch_push(true,(uint16_t)x,(uint16_t)y,(uint16_t)hold) ||
           !touch_push(false,(uint16_t)x,(uint16_t)y,0)) { reply("ERR queue-full\n");return; }
        network.touch_touched=true;reply("OK\n");return;
    }
    if((!strcasecmp(argv[0],"touchDown") || !strcasecmp(argv[0],"tapDown")) && argc==3) {
        long x=0,y=0;
        if(!touch_available) { reply("ERR touch-unavailable\n");return; }
        if(network.logical_touch_down) { reply("ERR touch-already-down\n");return; }
        if(!parse_number(argv[1],0,1279,&x) || !parse_number(argv[2],0,719,&y)) {
            reply("ERR usage: touchDown 0..1279 0..719\n");return;
        }
        if(!touch_push(true,(uint16_t)x,(uint16_t)y,0)) { reply("ERR queue-full\n");return; }
        network.logical_touch_down=true;network.logical_touch_x=(uint16_t)x;network.logical_touch_y=(uint16_t)y;
        network.touch_touched=true;reply("OK\n");return;
    }
    if(!strcasecmp(argv[0],"touchMove") && argc==3) {
        long x=0,y=0;
        if(!touch_available) { reply("ERR touch-unavailable\n");return; }
        if(!network.logical_touch_down) { reply("ERR touch-not-down\n");return; }
        if(!parse_number(argv[1],0,1279,&x) || !parse_number(argv[2],0,719,&y)) {
            reply("ERR usage: touchMove 0..1279 0..719\n");return;
        }
        if(!touch_push(true,(uint16_t)x,(uint16_t)y,0)) { reply("ERR queue-full\n");return; }
        network.logical_touch_x=(uint16_t)x;network.logical_touch_y=(uint16_t)y;reply("OK\n");return;
    }
    if(!strcasecmp(argv[0],"touchUp") && argc==1) {
        if(!touch_available) { reply("ERR touch-unavailable\n");return; }
        if(!network.logical_touch_down) { reply("ERR touch-not-down\n");return; }
        if(!touch_push(false,network.logical_touch_x,network.logical_touch_y,0)) { reply("ERR queue-full\n");return; }
        network.logical_touch_down=false;reply("OK\n");return;
    }
    if(!strcasecmp(argv[0],"key") && argc>=2) {
        if(key_space()<(argc-1)*2) { reply("ERR queue-full\n");return; }
        for(unsigned i=1;i<argc;i++) {
            NetworkKeyStroke stroke={0};if(!network_key_parse_chord(argv[i],true,&stroke)) { reply("ERR invalid-key-or-chord\n");return; }
        }
        for(unsigned i=1;i<argc;i++) { NetworkKeyStroke stroke={0};(void)network_key_parse_chord(argv[i],true,&stroke);(void)queue_key_tap(stroke.modifier,stroke.usage); }
        network.keyboard_touched=true;reply("OK\n");return;
    }
    if(!strcasecmp(argv[0],"keyMod") && argc>=3 && (argc%2)==1) {
        unsigned pairs=(argc-1)/2;
        if(key_space()<pairs*2) { reply("ERR queue-full\n");return; }
        for(unsigned i=0;i<pairs;i++) {
            NetworkKeyStroke stroke={0};long modifier_index=0;uint8_t modifier_mask=0;
            bool modifier_ok=parse_number(argv[2+i*2],0,7,&modifier_index) || network_modifier_parse(argv[2+i*2],&modifier_mask);
            if(!network_key_parse_chord(argv[1+i*2],true,&stroke) || !modifier_ok) {
                reply("ERR invalid-key-or-modifier\n");return;
            }
        }
        for(unsigned i=0;i<pairs;i++) {
            NetworkKeyStroke stroke={0};long modifier_index=0;uint8_t modifier_mask=0;
            (void)network_key_parse_chord(argv[1+i*2],true,&stroke);
            if(parse_number(argv[2+i*2],0,7,&modifier_index)) modifier_mask=(uint8_t)(1u<<modifier_index);
            else (void)network_modifier_parse(argv[2+i*2],&modifier_mask);
            (void)queue_key_tap((uint8_t)(stroke.modifier|modifier_mask),stroke.usage);
        }
        network.keyboard_touched=true;reply("OK\n");return;
    }
    if(!strcasecmp(argv[0],"keyDown") && (argc==2 || argc==3)) {
        NetworkKeyStroke stroke={0};long modifier_byte=0;uint8_t modifier_mask=0;
        bool modifier_ok=argc==2 || parse_number(argv[2],0,255,&modifier_byte) || network_modifier_parse(argv[2],&modifier_mask);
        if(!network_key_parse_chord(argv[1],true,&stroke) || !modifier_ok) {
            reply("ERR invalid-key-or-modifier-byte\n");return;
        }
        if(argc==3) stroke.modifier|=(uint8_t)(modifier_mask?modifier_mask:modifier_byte);
        if(!key_push(stroke.modifier,stroke.usage,0)) { reply("ERR queue-full\n");return; }
        network.keyboard_touched=true;reply("OK\n");return;
    }
    if(!strcasecmp(argv[0],"keyUp") && argc==1) {
        if(!key_push(0,0,0)) { reply("ERR queue-full\n");return; }
        network.keyboard_touched=true;reply("OK\n");return;
    }
    if(!strcasecmp(argv[0],"typeHex") && argc==2) {
        if(!queue_hex_text(argv[1])) { reply("ERR invalid-text-or-queue-full\n");return; }
        network.keyboard_touched=true;reply("OK\n");return;
    }
    reply("ERR unknown-command\n");
}

static void handle_line(char *line,uint64_t now,bool allowed) {
    size_t length=strlen(line);if(length && line[length-1]=='\r') line[--length]=0;
    if(network.phase==NET_AUTHENTICATING) {
        char *save=NULL;char *command=strtok_r(line," \t",&save);char *token=strtok_r(NULL," \t",&save);
        char *target_text=strtok_r(NULL," \t",&save);char *extra=strtok_r(NULL," \t",&save);
        NetworkTarget target=NETWORK_TARGET_USB;
        bool target_valid=!target_text || !strcasecmp(target_text,"switch2") || !strcasecmp(target_text,"usb");
        if(target_text && (!strcasecmp(target_text,"switch1") || !strcasecmp(target_text,"local"))) {
            target=NETWORK_TARGET_LOCAL;target_valid=true;
        }
        if(command && !strcasecmp(command,"auth") && token && !extra && target_valid && token_matches(token))
            (void)start_session(allowed,now,target);
        else {
            network_event(now,"authentication failed client_fd=%d",network.client_fd);
            reply("ERR authentication\n");close_client();network.phase=NET_LISTENING;
        }
        return;
    }
    if(network.phase==NET_ACTIVE) handle_active_command(line,now);
}

static void receive_client(uint64_t now,bool allowed) {
    char buffer[256];
    for(;;) {
        ssize_t count=recv(network.client_fd,buffer,sizeof(buffer),0);
        if(count==0) {
            network_event(now,"client EOF phase=%s client_fd=%d",phase_name(network.phase),network.client_fd);
            begin_cleanup("client disconnected");return;
        }
        if(count<0) {
            if(errno==EAGAIN || errno==EWOULDBLOCK) return;
            network_event(now,"client recv failed phase=%s client_fd=%d errno=%d",
                phase_name(network.phase),network.client_fd,errno);
            begin_cleanup("socket receive failed");return;
        }
        network.last_activity=now;
        for(ssize_t i=0;i<count;i++) {
            char c=buffer[i];
            if(c=='\n') {
                network.line[network.line_length]=0;
                handle_line(network.line,now,allowed);network.line_length=0;
                if(network.phase!=NET_AUTHENTICATING && network.phase!=NET_ACTIVE) return;
            } else if(network.line_length+1<sizeof(network.line)) network.line[network.line_length++]=c;
            else { reply("ERR line-too-long\n");begin_cleanup("command line too long");return; }
        }
    }
}

static void accept_client(uint64_t now) {
    struct sockaddr_in address;socklen_t length=sizeof(address);
    int client=accept(network.listen_fd,(struct sockaddr *)&address,&length);
    if(client<0) {
        int error_number=errno;
        if(error_number==ECONNABORTED || error_number==ENETUNREACH || error_number==EHOSTUNREACH) {
            network_event(now,"accept routing failure listen_fd=%d errno=%d; recycling socket service",
                network.listen_fd,error_number);
            schedule_listener_retry(now,"accept routing",error_number);
            return;
        }
        if(error_number!=EAGAIN && error_number!=EWOULDBLOCK && error_number!=EINTR) {
            network_event(now,"accept failed listen_fd=%d errno=%d",network.listen_fd,error_number);
            network_input_recover(now,"listener accept failed");
        }
        return;
    }
    if(!set_nonblocking(client)) {
        int error_number=errno;network_event(now,"client nonblocking failed fd=%d errno=%d",client,error_number);
        close(client);return;
    }
    int keepalive=1;(void)setsockopt(client,SOL_SOCKET,SO_KEEPALIVE,&keepalive,sizeof(keepalive));
    network.client_fd=client;network.phase=NET_AUTHENTICATING;network.line_length=0;
    network.auth_deadline=now+NETWORK_AUTH_TIMEOUT_MS;network.last_activity=now;
    char peer[INET_ADDRSTRLEN]="?";
    (void)inet_ntop(AF_INET,&address.sin_addr,peer,sizeof(peer));
    snprintf(network.client_address,sizeof(network.client_address),"%s:%u",peer,(unsigned)ntohs(address.sin_port));
    network_event(now,"client accepted fd=%d peer=%s:%u",client,peer,(unsigned)ntohs(address.sin_port));
    reply("NXInputRelay network input: auth <token> [switch1|switch2]\n");
}

static void pump_keyboard(uint64_t now) {
    if(network.target==NETWORK_TARGET_LOCAL) {
        if(!local_input_ready()) { begin_cleanup("local input service unavailable");return; }
        if(network.key_waiting) {
            network.key_waiting=false;network.key_due=now+network.key_hold_ms;
        }
        if(now<network.key_due || !network.key_count) return;
        KeyEvent event=network.key_queue[network.key_head];
        Result rc=local_input_set_keyboard(event.modifier,event.key);
        if(R_FAILED(rc)) {
            network_event(now,"local keyboard update failed rc=0x%08lx",(unsigned long)rc);
            begin_cleanup("local keyboard update failed");return;
        }
        network.key_head=(network.key_head+1)%NETWORK_KEY_QUEUE_CAPACITY;network.key_count--;
        network.key_waiting=true;network.key_hold_ms=event.hold_ms;
        return;
    }
    const KeyboardStatus *status=keyboard_status();
    if(!status->ready || status->reports_failed!=network.keyboard_failure_base) {
        begin_cleanup(!status->ready?"keyboard USB disconnected":"keyboard USB transfer failed");return;
    }
    if(network.key_waiting) {
        if(status->transfer_pending) return;
        network.key_waiting=false;network.key_due=now+network.key_hold_ms;
    }
    if(status->transfer_pending || now<network.key_due || !network.key_count) return;
    KeyEvent event=network.key_queue[network.key_head];
    if(R_FAILED(keyboard_send(event.modifier,event.key,now))) return;
    network.key_head=(network.key_head+1)%NETWORK_KEY_QUEUE_CAPACITY;network.key_count--;
    network.key_waiting=true;network.key_hold_ms=event.hold_ms;
}

static void pump_gamepad(uint64_t now) {
    if(network.target==NETWORK_TARGET_LOCAL) {
        if(!local_input_ready()) { begin_cleanup("local input service unavailable");return; }
        if(network.pad_waiting) {
            network.pad_waiting=false;network.pad_due=now+network.pad_hold_ms;
        }
        if(now<network.pad_due || !network.pad_count) return;
        PadEvent event=network.pad_queue[network.pad_head];
        Result rc=local_input_set_gamepad(&event.report);
        if(R_FAILED(rc)) {
            network_event(now,"local gamepad update failed rc=0x%08lx",(unsigned long)rc);
            begin_cleanup("local gamepad update failed");return;
        }
        network.pad_head=(network.pad_head+1)%NETWORK_PAD_QUEUE_CAPACITY;network.pad_count--;
        network.pad_waiting=true;network.pad_hold_ms=event.hold_ms;
        return;
    }
    GamepadExclusiveState state=gamepad_exclusive_status();
    if(state==GAMEPAD_EXCLUSIVE_INTERRUPTED || !gamepad_ready()) {
        begin_cleanup("gamepad USB disconnected or interrupted");return;
    }
    if(state==GAMEPAD_EXCLUSIVE_SYNC || state==GAMEPAD_EXCLUSIVE_PENDING || state==GAMEPAD_EXCLUSIVE_STOPPING) return;
    if(state!=GAMEPAD_EXCLUSIVE_READY) { begin_cleanup("gamepad ownership lost");return; }
    if(network.pad_waiting) {
        network.pad_waiting=false;network.pad_due=now+network.pad_hold_ms;
    }
    if(now<network.pad_due || !network.pad_count) return;
    PadEvent event=network.pad_queue[network.pad_head];
    if(!gamepad_exclusive_submit(&event.report)) return;
    network.pad_head=(network.pad_head+1)%NETWORK_PAD_QUEUE_CAPACITY;network.pad_count--;
    network.pad_waiting=true;network.pad_hold_ms=event.hold_ms;
}

static void pump_touch(uint64_t now) {
    if(!network.touch_count && !network.touch_waiting) return;
    if(network.target==NETWORK_TARGET_LOCAL) {
        if(!local_input_ready()) { begin_cleanup("local input service unavailable");return; }
        if(network.touch_waiting) {
            network.touch_waiting=false;network.touch_due=now+network.touch_hold_ms;
        }
        if(now<network.touch_due || !network.touch_count) return;
        TouchEvent event=network.touch_queue[network.touch_head];
        Result rc=local_input_set_touch(event.down,event.x,event.y);
        if(R_FAILED(rc)) {
            network_event(now,"local touch update failed rc=0x%08lx",(unsigned long)rc);
            begin_cleanup("local touch update failed");return;
        }
        network.touch_head=(network.touch_head+1)%NETWORK_TOUCH_QUEUE_CAPACITY;network.touch_count--;
        network.touch_waiting=true;network.touch_hold_ms=event.hold_ms;return;
    }
    const UsbTouchStatus *touch=usb_touch_status();
    if(!touch->ready || touch->reports_failed!=network.touch_failure_base) {
        begin_cleanup(!touch->ready?"touch USB disconnected":"touch USB transfer failed");return;
    }
    if(network.touch_waiting) {
        if(touch->transfer_pending) return;
        network.touch_waiting=false;network.touch_due=now+network.touch_hold_ms;
    }
    if(touch->transfer_pending || now<network.touch_due || !network.touch_count) return;
    TouchEvent event=network.touch_queue[network.touch_head];
    if(R_FAILED(usb_touch_send(event.down,event.x,event.y,now))) return;
    network.touch_head=(network.touch_head+1)%NETWORK_TOUCH_QUEUE_CAPACITY;network.touch_count--;
    network.touch_waiting=true;network.touch_hold_ms=event.hold_ms;
}

static void pump_cleanup(uint64_t now) {
    if(network.target==NETWORK_TARGET_LOCAL) {
        Result rc=local_input_ready()?local_input_neutral():KERNELRESULT(InvalidState);
        if(R_FAILED(rc)) network_event(now,"local neutral cleanup failed rc=0x%08lx",(unsigned long)rc);
        local_input_end();
        network.phase=NET_LISTENING;network.keyboard_touched=network.touch_touched=false;
        network_event(now,"cleanup complete; local target released listen_fd=%d",network.listen_fd);
        return;
    }
    const KeyboardStatus *keyboard=keyboard_status();
    if(!network.cleanup_key_done) {
        if(!network.cleanup_key_sent) {
            if(keyboard->ready && !keyboard->transfer_pending && R_SUCCEEDED(keyboard_send(0,0,now)))
                network.cleanup_key_sent=true;
        } else if(!keyboard->transfer_pending) network.cleanup_key_done=true;
    }

    const UsbTouchStatus *touch=usb_touch_status();
    if(!network.cleanup_touch_done) {
        if(!touch->ready) network.cleanup_touch_done=true;
        else if(!network.cleanup_touch_sent) {
            if(touch->ready && !touch->transfer_pending &&
               R_SUCCEEDED(usb_touch_send(false,network.logical_touch_x,network.logical_touch_y,now)))
                network.cleanup_touch_sent=true;
        } else if(!touch->transfer_pending) network.cleanup_touch_done=true;
    }

    GamepadExclusiveState gamepad=gamepad_exclusive_status();
    if(gamepad==GAMEPAD_EXCLUSIVE_READY || (gamepad==GAMEPAD_EXCLUSIVE_INTERRUPTED && gamepad_ready())) {
        if(gamepad_exclusive_stop()) network.cleanup_pad_requested=true;
    } else if(gamepad==GAMEPAD_EXCLUSIVE_STOPPING) network.cleanup_pad_requested=true;
    bool pad_done=gamepad==GAMEPAD_EXCLUSIVE_OFF;
    if(network.cleanup_key_done && network.cleanup_touch_done && pad_done) {
        network.phase=NET_LISTENING;network.keyboard_touched=network.touch_touched=false;
        network_event(now,"cleanup complete; local control restored listen_fd=%d",network.listen_fd);
    }
}

static bool open_listener(uint64_t now) {
    if(network.listen_fd>=0) return true;
    Result rc=socketInitializeDefault();
    if(R_FAILED(rc)) {
        schedule_listener_retry(now,"socketInitializeDefault",(int)rc);
        return false;
    }
    network.sockets_initialized=true;
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) {
        schedule_listener_retry(now,"socket",errno);
        return false;
    }
    int yes=1;(void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    struct sockaddr_in server={0};server.sin_family=AF_INET;server.sin_addr.s_addr=INADDR_ANY;server.sin_port=htons(NETWORK_INPUT_PORT);
    if(bind(fd,(struct sockaddr *)&server,sizeof(server))<0) {
        int error_number=errno;close(fd);schedule_listener_retry(now,"bind",error_number);return false;
    }
    if(listen(fd,1)<0) {
        int error_number=errno;close(fd);schedule_listener_retry(now,"listen",error_number);return false;
    }
    if(!set_nonblocking(fd)) {
        int error_number=errno;close(fd);schedule_listener_retry(now,"nonblocking",error_number);return false;
    }
    network.listen_fd=fd;
    network.restart_failures=0;
    network.restart_due=0;
    network.listener_generation++;
    if(network.phase!=NET_ACTIVE && network.phase!=NET_CLEANUP) network.phase=NET_LISTENING;
    network_event(now,network.listener_generation==1?
        "listener ready port=%u fd=%d generation=%u":
        "listener restored port=%u fd=%d generation=%u",
        (unsigned)NETWORK_INPUT_PORT,fd,network.listener_generation);
    return true;
}

static void format_ip(uint32_t raw,char text[INET_ADDRSTRLEN]) {
    struct in_addr address={.s_addr=raw};
    if(!inet_ntop(AF_INET,&address,text,INET_ADDRSTRLEN)) snprintf(text,INET_ADDRSTRLEN,"?");
}

static bool ip_is_usable(bool valid,uint32_t ip) {
    if(!valid || !ip || ip==INADDR_LOOPBACK || ip==htonl(INADDR_LOOPBACK)) return false;
    uint32_t host=ntohl(ip);
    return (host>>24)!=127u;
}

static bool current_keepalive_target(uint32_t *out_target,Result *out_result) {
    if(out_target) *out_target=0;
    Result rc=nifmInitialize(NifmServiceType_User);
    if(R_FAILED(rc)) { if(out_result) *out_result=rc;return false; }
    uint32_t address=0,mask=0,gateway=0,dns1=0,dns2=0;
    rc=nifmGetCurrentIpConfigInfo(&address,&mask,&gateway,&dns1,&dns2);
    nifmExit();
    if(out_result) *out_result=rc;
    if(R_FAILED(rc)) return false;
    uint32_t target=dns1?dns1:gateway;
    if(!target) return false;
    if(out_target) *out_target=target;
    return true;
}

static bool send_link_keepalive(uint64_t now) {
    if(!network.sockets_initialized) return true;
    uint32_t target=0;Result rc=0;
    if(!current_keepalive_target(&target,&rc)) {
        network_event(now,"LAN keepalive skipped: DNS/gateway target unavailable rc=0x%08lx",(unsigned long)rc);
        return true;
    }
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if(fd<0) {
        network_event(now,"LAN keepalive socket failed errno=%d",errno);
        return false;
    }
    static const char payload[]="NXInputRelay";
    struct sockaddr_in destination={0};
    destination.sin_family=AF_INET;destination.sin_addr.s_addr=target;destination.sin_port=htons(9);
    ssize_t sent=sendto(fd,payload,sizeof(payload)-1,0,(struct sockaddr *)&destination,sizeof(destination));
    int send_error=sent==(ssize_t)(sizeof(payload)-1)?0:errno;
    close(fd);
    if(send_error) {
        network_event(now,"LAN keepalive send failed errno=%d",send_error);
        return false;
    }
    network.link_keepalives_sent++;
    return true;
}

static void network_health_tick(uint64_t now) {
    if(now<network.next_health_check) return;
    network.next_health_check=now+NETWORK_HEALTH_CHECK_MS;

    errno=0;
    long host_id=gethostid();
    int ip_error=errno;
    bool ip_valid=host_id!=-1;
    uint32_t ip=ip_valid?(uint32_t)host_id:0;
    bool link_usable=ip_is_usable(ip_valid,ip);
    bool previous_set=network.observed_ip_set;
    bool previous_usable=network.link_usable;
    uint32_t previous_ip=network.observed_ip;
    if(!network.observed_ip_set || network.observed_ip_valid!=ip_valid || network.observed_ip!=ip) {
        char old_text[INET_ADDRSTRLEN]="none",new_text[INET_ADDRSTRLEN]="none";
        if(network.observed_ip_valid) format_ip(network.observed_ip,old_text);
        if(ip_valid) format_ip(ip,new_text);
        network_event(now,"IP change old=%s raw=0x%08lx new=%s raw=0x%08lx query_errno=%d",
            old_text,(unsigned long)network.observed_ip,new_text,(unsigned long)ip,ip_error);
        network.observed_ip=ip;network.observed_ip_valid=ip_valid;network.observed_ip_set=true;
    }
    network.link_usable=link_usable;
    if(previous_set && (previous_usable!=link_usable || (link_usable && previous_ip!=ip))) {
        network.next_link_keepalive=now+NETWORK_LINK_KEEPALIVE_MS;
        network.link_keepalive_failures=0;
        network_event(now,"IPv4 observation changed (%s); preserving listener bound to INADDR_ANY",
            link_usable?"usable":"temporarily unusable");
    }

    if(link_usable && now>=network.next_link_keepalive) {
        network.next_link_keepalive=now+NETWORK_LINK_KEEPALIVE_MS;
        if(send_link_keepalive(now)) network.link_keepalive_failures=0;
        else {
            if(network.link_keepalive_failures<NETWORK_LINK_FAILURE_LIMIT)
                network.link_keepalive_failures++;
            if(network.link_keepalive_failures>=NETWORK_LINK_FAILURE_LIMIT) {
                /* This UDP packet is only an access-point-idle hint. An
                   isolated setup may intentionally configure an unreachable
                   gateway while its local TCP path remains healthy. */
                network.next_link_keepalive=now+NETWORK_LINK_KEEPALIVE_BACKOFF_MS;
                network_event(now,"LAN keepalive unavailable after %u attempts; preserving TCP listener and clients; retry in %u ms",
                    network.link_keepalive_failures,NETWORK_LINK_KEEPALIVE_BACKOFF_MS);
            }
        }
    }

    int flags=-1,socket_error=0,socket_query=0;
    if(network.listen_fd>=0) {
        errno=0;flags=fcntl(network.listen_fd,F_GETFL,0);
        if(flags<0) {
            int error_number=errno;
            network_event(now,"listener fcntl health failure fd=%d errno=%d",network.listen_fd,error_number);
            network_input_recover(now,"listener descriptor invalid");return;
        }
        socklen_t length=sizeof(socket_error);
        errno=0;socket_query=getsockopt(network.listen_fd,SOL_SOCKET,SO_ERROR,&socket_error,&length);
        if(socket_query<0 || socket_error) {
            int error_number=socket_query<0?errno:socket_error;
            network_event(now,"listener SO_ERROR health failure fd=%d query=%d error=%d",
                network.listen_fd,socket_query,error_number);
            network_input_recover(now,"listener socket error");return;
        }
    }

    if(now>=network.next_health_log) {
        char ip_text[INET_ADDRSTRLEN]="none";
        if(network.observed_ip_valid) format_ip(network.observed_ip,ip_text);
        uint64_t activity_age=(network.phase==NET_ACTIVE || network.phase==NET_AUTHENTICATING)?now-network.last_activity:0;
        network_event(now,"health phase=%s target=%s link=%s ip=%s raw=0x%08lx listen_fd=%d client_fd=%d flags=0x%x so_error=%d generation=%u retry_failures=%u retry_in_ms=%llu activity_age_ms=%llu keepalives=%lu keepalive_failures=%u keyq=%u padq=%u touchq=%u",
            phase_name(network.phase),target_name(network.target),network.link_usable?"up":"down",ip_text,(unsigned long)network.observed_ip,
            network.listen_fd,network.client_fd,flags,socket_error,network.listener_generation,
            network.restart_failures,
            (unsigned long long)(network.listen_fd<0 && network.restart_due>now?network.restart_due-now:0),
            (unsigned long long)activity_age,network.link_keepalives_sent,network.link_keepalive_failures,
            network.key_count,network.pad_count,network.touch_count);
        network.next_health_log=now+NETWORK_HEARTBEAT_MS;
    }
}

bool network_input_init(void) {
    network=(NetworkInput){.phase=NET_DISABLED,.listen_fd=-1,.client_fd=-1};
    open_network_log();
    network_event(0,"--- NXInputRelay network input start ---");
    if(!load_token()) return false;
    network.enabled=true;
    network.phase=NET_LISTENING;
    network.restart_due=0;
    network_health_tick(0);
    /* The listener is bound to INADDR_ANY and remains useful while NIFM is
       transitioning between loopback and the Wi-Fi address.  Do not gate
       socket creation on gethostid(), which can remain stale for minutes. */
    return open_listener(0);
}

void network_input_poll(uint64_t now,bool allow_session) {
    if(!network.enabled || network.phase==NET_DISABLED) return;
    network.clock_now=now;
    network_health_tick(now);
    if(network.listen_fd<0 && now>=network.restart_due) (void)open_listener(now);
    if(network.phase==NET_LISTENING && network.listen_fd>=0) accept_client(now);
    if(network.phase==NET_AUTHENTICATING || network.phase==NET_ACTIVE) receive_client(now,allow_session);
    if(network.phase==NET_AUTHENTICATING && now>=network.auth_deadline) {
        network_event(now,"authentication timeout client_fd=%d",network.client_fd);
        reply("ERR authentication-timeout\n");close_client();network.phase=NET_LISTENING;
    }
    if(network.phase==NET_ACTIVE && now-network.last_activity>=NETWORK_IDLE_TIMEOUT_MS) begin_cleanup("idle timeout");
    if(network.phase==NET_ACTIVE) {
        pump_keyboard(now);if(network.phase==NET_ACTIVE) pump_gamepad(now);
        if(network.phase==NET_ACTIVE) pump_touch(now);
    }
    if(network.phase==NET_CLEANUP) pump_cleanup(now);
}

bool network_input_active(void) { return network.phase==NET_ACTIVE || network.phase==NET_CLEANUP; }

void network_input_get_status(uint64_t now,NetworkInputStatus *out) {
    if(!out) return;
    memset(out,0,sizeof(*out));
    out->enabled=network.enabled;
    out->link_usable=network.link_usable;
    out->listener_ready=network.listen_fd>=0;
    out->client_connected=network.client_fd>=0;
    out->session_active=network.phase==NET_ACTIVE;
    out->cleanup_active=network.phase==NET_CLEANUP;
    out->port=NETWORK_INPUT_PORT;
    out->listener_generation=network.listener_generation;
    out->restart_failures=network.restart_failures;
    out->key_queue=network.key_count;
    out->pad_queue=network.pad_count;
    out->touch_queue=network.touch_count;
    out->keepalive_failures=network.link_keepalive_failures;
    out->keepalives_sent=network.link_keepalives_sent;
    if((out->session_active || out->cleanup_active) && now>=network.session_started)
        out->session_age_ms=now-network.session_started;
    if((network.phase==NET_ACTIVE || network.phase==NET_AUTHENTICATING) && now>=network.last_activity)
        out->activity_age_ms=now-network.last_activity;
    snprintf(out->phase,sizeof(out->phase),"%s",phase_name(network.phase));
    if(network.observed_ip_valid) format_ip(network.observed_ip,out->ip);
    else snprintf(out->ip,sizeof(out->ip),"none");
    snprintf(out->active_target,sizeof(out->active_target),"%s",
        (out->session_active || out->cleanup_active)?target_name(network.target):"None");
    snprintf(out->last_target,sizeof(out->last_target),"%s",
        network.last_target_set?target_name(network.last_target):"None");
    snprintf(out->client,sizeof(out->client),"%s",network.client_address[0]?network.client_address:"None");
    snprintf(out->last_command,sizeof(out->last_command),"%s",network.last_command[0]?network.last_command:"None");
    snprintf(out->last_recovery,sizeof(out->last_recovery),"%s",network.last_recovery[0]?network.last_recovery:"None");
}

void network_input_request_stop(void) {
    if(network.phase==NET_AUTHENTICATING) { close_client();network.phase=NET_LISTENING; }
    else if(network.phase==NET_ACTIVE) begin_cleanup("local stop requested");
}

void network_input_shutdown(void) {
    network_event(network.clock_now,"shutdown phase=%s listen_fd=%d client_fd=%d",
        phase_name(network.phase),network.listen_fd,network.client_fd);
    network.enabled=false;
    local_input_end();
    close_socket_service();
    network.phase=NET_DISABLED;
    if(network_log_file) { fclose(network_log_file);network_log_file=NULL; }
}

