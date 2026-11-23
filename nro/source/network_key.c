#include "network_key.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

typedef struct { const char *name;uint8_t usage; } KeyName;

static bool modifier_part(const char *text,uint8_t *mask) {
    static const struct { const char *name;uint8_t mask; } names[]={
        {"CTRL",1},{"CONTROL",1},{"LCTRL",1},{"LEFTCTRL",1},{"LEFTCONTROL",1},
        {"SHIFT",2},{"LSHIFT",2},{"LEFTSHIFT",2},
        {"ALT",4},{"OPTION",4},{"LALT",4},{"LEFTALT",4},
        {"GUI",8},{"WIN",8},{"WINDOWS",8},{"META",8},{"CMD",8},{"COMMAND",8},
        {"LGUI",8},{"LEFTGUI",8},{"LEFTWIN",8},{"LEFTWINDOWS",8},
        {"RCTRL",16},{"RIGHTCTRL",16},{"RIGHTCONTROL",16},
        {"RSHIFT",32},{"RIGHTSHIFT",32},
        {"RALT",64},{"RIGHTALT",64},{"ALTGR",64},
        {"RGUI",128},{"RIGHTGUI",128},{"RIGHTWIN",128},{"RIGHTWINDOWS",128}
    };
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        if(!strcasecmp(text,names[i].name)) { *mask=names[i].mask;return true; }
    }
    return false;
}

bool network_key_parse(const char *text,uint8_t *usage) {
    if(!text || !*text || !usage) return false;

    char *end=NULL;errno=0;long number=strtol(text,&end,0);
    if(!errno && end && !*end && number>=4 && number<=231) {
        *usage=(uint8_t)number;return true;
    }

    if(text[1]=='\0') {
        unsigned char c=(unsigned char)text[0];
        if(c>='a' && c<='z') { *usage=(uint8_t)(4+c-'a');return true; }
        if(c>='A' && c<='Z') { *usage=(uint8_t)(4+c-'A');return true; }
    }

    static const KeyName names[]={
        {"ENTER",40},{"RETURN",40},{"ESC",41},{"ESCAPE",41},
        {"BACKSPACE",42},{"BKSP",42},{"TAB",43},{"SPACE",44},{"SPACEBAR",44},
        {"MINUS",45},{"HYPHEN",45},{"EQUAL",46},{"EQUALS",46},
        {"LEFTBRACKET",47},{"LBRACKET",47},{"RIGHTBRACKET",48},{"RBRACKET",48},
        {"BACKSLASH",49},{"SEMICOLON",51},{"APOSTROPHE",52},{"QUOTE",52},
        {"GRAVE",53},{"BACKTICK",53},{"COMMA",54},{"PERIOD",55},{"DOT",55},{"SLASH",56},
        {"CAPSLOCK",57},{"CAPS",57},
        {"PRINTSCREEN",70},{"PRTSC",70},{"SCROLLLOCK",71},{"PAUSE",72},
        {"INSERT",73},{"INS",73},{"HOME",74},{"PAGEUP",75},{"PGUP",75},
        {"DELETE",76},{"DEL",76},{"END",77},{"PAGEDOWN",78},{"PGDN",78},
        {"RIGHT",79},{"RIGHTARROW",79},{"LEFT",80},{"LEFTARROW",80},
        {"DOWN",81},{"DOWNARROW",81},{"UP",82},{"UPARROW",82},
        {"NUMLOCK",83},{"KPSLASH",84},{"KPDIVIDE",84},{"KPMULTIPLY",85},
        {"KPMINUS",86},{"KPPLUS",87},{"KPENTER",88},{"KP1",89},{"KP2",90},
        {"KP3",91},{"KP4",92},{"KP5",93},{"KP6",94},{"KP7",95},{"KP8",96},
        {"KP9",97},{"KP0",98},{"KPDECIMAL",99},{"KPDOT",99},
        {"APPLICATION",101},{"MENU",101},{"POWER",102},{"KPEQUAL",103},
        {"EXECUTE",116},{"HELP",117},{"SELECT",119},{"STOP",120},{"AGAIN",121},
        {"UNDO",122},{"CUT",123},{"COPY",124},{"PASTE",125},{"FIND",126},
        {"MUTE",127},{"VOLUMEUP",128},{"VOLUMEDOWN",129}
    };
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        if(!strcasecmp(text,names[i].name)) { *usage=names[i].usage;return true; }
    }
    if((text[0]=='f' || text[0]=='F') && text[1]) {
        errno=0;number=strtol(text+1,&end,10);
        if(!errno && end && !*end && number>=1 && number<=24) {
            *usage=(uint8_t)(number<=12?57+number:91+number);return true;
        }
    }
    if(!strncasecmp(text,"digit",5) && text[5] && !text[6]) {
        char digit=text[5];
        if(digit>='1' && digit<='9') { *usage=(uint8_t)(30+digit-'1');return true; }
        if(digit=='0') { *usage=39;return true; }
    }
    return false;
}

bool network_modifier_parse(const char *text,uint8_t *modifier) {
    if(!text || !*text || !modifier) return false;
    size_t length=strlen(text);if(length>=128) return false;
    char copy[128];memcpy(copy,text,length+1);uint8_t result=0;char *part=copy;
    while(part) {
        char *next=strchr(part,'+');if(next) *next++='\0';
        uint8_t mask=0;if(!*part || !modifier_part(part,&mask)) return false;
        result|=mask;part=next;
    }
    *modifier=result;return result!=0;
}

bool network_key_parse_chord(const char *text,bool allow_modifier_only,NetworkKeyStroke *stroke) {
    if(!text || !*text || !stroke) return false;
    size_t length=strlen(text);if(length>=128) return false;
    uint8_t usage=0;
    if(network_key_parse(text,&usage)) { *stroke=(NetworkKeyStroke){0,usage};return true; }

    char copy[128];memcpy(copy,text,length+1);uint8_t modifiers=0;bool have_key=false;char *part=copy;
    while(part) {
        char *next=strchr(part,'+');if(next) *next++='\0';
        uint8_t mask=0,key=0;
        if(!*part) return false;
        if(modifier_part(part,&mask)) modifiers|=mask;
        else if(!have_key && network_key_parse(part,&key)) { usage=key;have_key=true; }
        else return false;
        part=next;
    }
    if(!have_key && (!allow_modifier_only || !modifiers)) return false;
    *stroke=(NetworkKeyStroke){modifiers,usage};return true;
}

