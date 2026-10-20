#include "xiso.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t cancel_after, previous;
static int progress(void *context, const char *file, uint64_t done, uint64_t total) {
    (void)context;(void)file;
    if (done<previous || done>total) abort();
    previous=done;
    return !cancel_after || done<cancel_after;
}
static int print_file(void *context, const char *path, uint64_t offset, uint64_t size) {
    (void)context;
    printf("%s\t%llu\t%llu\n",path,(unsigned long long)offset,(unsigned long long)size);
    return 1;
}
int main(int argc, char **argv) {
    char error[1024]={0};int ok;
    if (argc<3) return 2;
    if (!strcmp(argv[1],"plan")) {
        /* plan IMAGE RECEIVED TOTAL: the ranges an import reads, one "start end" a line */
        static struct xiso_range ranges[XISO_PLAN_RANGES];
        int count;
        if (argc<5) return 2;
        count=xiso_plan(argv[2],strtoull(argv[3],NULL,10),strtoull(argv[4],NULL,10),ranges,XISO_PLAN_RANGES);
        for (int i=0;i<count;i++) printf("%llu %llu\n",(unsigned long long)ranges[i].start,(unsigned long long)ranges[i].end);
        return count?0:1;
    }
    if (!strcmp(argv[1],"list")) {
        /* list IMAGE: every file, "path offset size" tab separated a line */
        ok=xiso_list(argv[2],print_file,NULL,error,sizeof(error));
        if (!ok) printf("ERROR: %s\n",error);
        return ok?0:1;
    }
    if (!strcmp(argv[1],"consume")) {
        if (argc<4) return 2;
        ok=xiso_extract_maps_consuming(argv[2],argv[3],progress,NULL,error,sizeof(error));
        printf("%s: %s\n",ok?"OK":"ERROR",ok?"maps validated":error);
        return ok?0:1;
    }
    if (!strcmp(argv[1],"ready")) ok=xiso_maps_ready(argv[2],error,sizeof(error));
    else {
        if (argc<4) return 2;
        if (argc>4) cancel_after=strtoull(argv[4],NULL,10);
        ok=xiso_extract_maps(argv[2],argv[3],progress,NULL,error,sizeof(error));
    }
    printf("%s: %s\n",ok?"OK":"ERROR",ok?"maps validated":error);
    return ok?0:1;
}
