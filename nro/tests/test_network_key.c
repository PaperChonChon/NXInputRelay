#include "network_key.h"

#include <assert.h>
#include <stdio.h>

static void expect(const char *name,unsigned expected) {
    uint8_t usage=0;assert(network_key_parse(name,&usage));assert(usage==expected);
}

int main(void) {
    expect("82",82);expect("0x52",82);
    expect("up",82);expect("UPARROW",82);expect("down",81);
    expect("left",80);expect("right",79);
    expect("enter",40);expect("escape",41);expect("space",44);
    expect("home",74);expect("end",77);expect("pageup",75);expect("pgdn",78);
    expect("a",4);expect("Z",29);expect("digit1",30);expect("DIGIT0",39);
    expect("f1",58);expect("F12",69);expect("f13",104);expect("F24",115);
    expect("minus",45);expect("slash",56);expect("kpplus",87);expect("copy",124);
    NetworkKeyStroke stroke={0};
    assert(network_key_parse_chord("ctrl+a",false,&stroke) && stroke.modifier==1 && stroke.usage==4);
    assert(network_key_parse_chord("alt+F4",false,&stroke) && stroke.modifier==4 && stroke.usage==61);
    assert(network_key_parse_chord("CTRL+shift+up",false,&stroke) && stroke.modifier==3 && stroke.usage==82);
    assert(network_key_parse_chord("rctrl+digit0",false,&stroke) && stroke.modifier==16 && stroke.usage==39);
    assert(network_key_parse_chord("ctrl",true,&stroke) && stroke.modifier==1 && stroke.usage==0);
    assert(!network_key_parse_chord("ctrl",false,&stroke));
    uint8_t modifiers=0;assert(network_modifier_parse("ctrl+shift+ralt",&modifiers) && modifiers==67);
    assert(!network_modifier_parse("ctrl+banana",&modifiers));
    uint8_t usage=0;assert(!network_key_parse("",&usage));
    assert(!network_key_parse("not-a-key",&usage));assert(!network_key_parse("3",&usage));
    assert(!network_key_parse("232",&usage));assert(!network_key_parse("f25",&usage));
    puts("PASS: numeric and descriptive network keyboard usages.");return 0;
}

