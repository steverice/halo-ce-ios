#include "host_external_mode.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    int width=7,height=9,count=0;
    assert(host_external_mode_parse("preferred,1280x720,1920x1080",1,&width,&height,&count));
    assert(width==1280 && height==720 && count==3);
    assert(host_external_mode_parse("preferred,1280x720",0,&width,&height,&count));
    assert(width==0 && height==0 && count==2);
    assert(!host_external_mode_parse("1280x720,",0,&width,&height,&count));
    assert(!host_external_mode_parse("1280x720,,1920x1080",0,&width,&height,&count));
    assert(!host_external_mode_parse("-1x720",0,&width,&height,&count));
    assert(!host_external_mode_parse("1920x0",0,&width,&height,&count));
    assert(!host_external_mode_parse("999999999999999999999x720",0,&width,&height,&count));
    assert(!host_external_mode_parse("1920x1080junk",0,&width,&height,&count));
    assert(!host_external_mode_parse("1280x720,invalid",0,&width,&height,&count));
    assert(host_external_mode_parse("",0,&width,&height,&count));
    assert(width==0 && height==0 && count==1);
    puts("PASS: mode lists validate every item, reject overflow and trailing text, and select by index");
}
