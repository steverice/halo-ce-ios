#include "host_external_mode.h"
#include <math.h>

int host_external_mode_choose(const struct host_external_mode *modes, size_t count,
    int preferred, int cap, int forced_width, int forced_height)
{
    int best = -1, fallback = -1, best_matches = 0;
    double ratio = preferred >= 0 && (size_t)preferred < count && modes[preferred].height > 0 ?
        (double)modes[preferred].width / modes[preferred].height : 0;
    for (size_t i = 0; i < count; ++i) {
        const struct host_external_mode *m = modes + i;
        if (m->refused || m->width <= 0 || m->height <= 0) continue;
        if (forced_width || forced_height) {
            if (m->width == forced_width && m->height == forced_height) return (int)i;
            continue;
        }
        if (fabs(m->pixel_aspect - 1.0) > 0.001 || (cap > 0 && m->height > cap)) continue;
        if (fallback < 0) fallback = (int)i;
        if (m->height == 576 || m->height == 288) continue;
        if ((int)i == preferred) return preferred;
        int matches = ratio > 0 && fabs((double)m->width / m->height / ratio - 1.0) <= 0.01;
        if (best < 0 || matches > best_matches || (matches == best_matches &&
            (m->height > modes[best].height || (m->height == modes[best].height && m->width > modes[best].width)))) {
            best = (int)i;
            best_matches = matches;
        }
    }
    if (!forced_width && !forced_height && best < 0 && fallback < 0 && cap > 0)
        return host_external_mode_choose(modes,count,preferred,0,0,0);
    return forced_width || forced_height ? -1 : best >= 0 ? best : fallback;
}

#include <stdlib.h>
#include <string.h>
#include <errno.h>

int host_external_mode_parse(const char *text, int index, int *width, int *height, int *count)
{
    int selected_width=0,selected_height=0,items=0;
    if(!text || !*text) {
        if(index!=0)return 0;
        *width=*height=0;*count=1;return 1;
    }
    if(strlen(text)>511 || index<0)return 0;
    const char *cursor=text;
    for(;;) {
        const char *end=cursor+strcspn(cursor,",");
        int w=0,h=0;
        if(end-cursor!=9 || memcmp(cursor,"preferred",9)) {
            char *x,*tail;
            if(*cursor<'1' || *cursor>'9')return 0;
            errno=0;
            long a=strtol(cursor,&x,10);
            if(errno || x==cursor || *x!='x' || x[1]<'1' || x[1]>'9')return 0;
            long b=strtol(x+1,&tail,10);
            if(errno || tail!=end || a<1 || a>32768 || b<1 || b>32768)return 0;
            w=(int)a;h=(int)b;
        }
        if(items==index){selected_width=w;selected_height=h;}
        if(++items>32)return 0;
        if(!*end)break;
        cursor=end+1;
        if(!*cursor)return 0;
    }
    if(index>=items)return 0;
    *width=selected_width;*height=selected_height;*count=items;
    return 1;
}

/* Bound allocation even when a receiver exposes only a 4K physical mode.
   The width bound keeps unusually wide modes inside the same pixel budget. */
int host_external_mode_drawable(int width,int height,int cap,int *out_width,int *out_height)
{
    if(width<=0 || height<=0)return 0;
    double scale=1.0;
    if(cap>0) {
        scale=fmin(scale,(double)cap/height);
        scale=fmin(scale,((double)cap*16.0/9.0)/width);
    }
    *out_width=(int)fmax(1,floor(width*scale));
    *out_height=(int)fmax(1,floor(height*scale));
    return 1;
}
