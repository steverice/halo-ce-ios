/* Stress the exact render/streaming race on the host's native page size. */
#include "ios_host.h"
#include "guest_host.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

void host_logf(int level,const char *format,...) {
    (void)level;va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);fputc('\n',stderr);va_end(ap);
}
static unsigned finished, started;
static void *writer(void *arg) {
    volatile unsigned *p=host_pointer(0x80010000+(uintptr_t)arg*4);
    __atomic_add_fetch(&started,1,__ATOMIC_RELEASE);
    unsigned n=0;while(!__atomic_load_n(&finished,__ATOMIC_RELAXED))*p=++n;
    return NULL;
}
int main(int argc,char **argv) {
    int reduced=argc>1&&!strcmp(argv[1],"reduced");
    int early=argc>1&&!strcmp(argv[1],"early");
    if(reduced)host_arena_try_full=0;
    /* the app reserves before main (host_loader.c); initialize then adopts that reservation */
    if(early)host_memory_reserve_early(0,0x8a70b000u);
    if(host_memory_initialize(0,0x8a70b000u))return 1;
    if(reduced?host_arena_size!=0x8b000000ull:host_arena_size!=0x100000000ull)return 30;
    if(!host_low_owns((uintptr_t)host_pointer(0x8a000000u),4))return 31;
    if(reduced&&host_low_owns((uintptr_t)host_pointer(0x8c000000u),4))return 32;
    /* past the shorter arena's end a guest range is refused, not applied to host memory */
    if(reduced&&host_guest_mprotect(0x8c000000u,0x4000,PROT_READ)!=-22)return 33;
    if(reduced&&host_guest_munmap(0x8c000000u,0x4000)!=-22)return 34;
    host_install_signal_handlers();host_memory_watch_initialize();
    /* Reproduce the map inflater's writable 4 KB tail beside a read-only
       buffer. The boundary Apple page must remain writable. */
    if(host_guest_mprotect(0x80020000,0x6000,PROT_READ))return 4;
    *(volatile unsigned *)host_pointer(0x80026014)=42;
    pthread_t threads[4];
    for(uintptr_t i=0;i<4;i++)pthread_create(&threads[i],NULL,writer,(void *)i);
    while(__atomic_load_n(&started,__ATOMIC_ACQUIRE)<4)sched_yield();
    for(int i=0;i<40000;i++){host_memory_watch_protect(0x80010000,4096);if(!(i%10))sched_yield();}
    __atomic_store_n(&finished,1,__ATOMIC_RELAXED);
    for(int i=0;i<4;i++)pthread_join(threads[i],NULL);
    unsigned generation=host_memory_watch_generation(0x80010000,4096);
    if(!generation)return 2;
    host_memory_watch_prepare_write(0x80010000,4096);
    unsigned *p=host_low_map(16384,PROT_READ|PROT_WRITE);*p=1234;
    host_low_unmap(p,16384);p=host_low_map(16384,PROT_READ|PROT_WRITE);
    if(*p)return 3;
    /* Custom Edition's tag cache: Linux MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|
       MAP_FIXED_NOREPLACE at 0x40440000, inside the pool. Overlaps are refused. */
    if(host_guest_mmap(0x40440000,0x1700000,PROT_READ|PROT_WRITE,0x104022,-1,0)!=0x40440000)return 5;
    *(volatile unsigned *)host_pointer(0x41b3fffc)=7;
    if(host_guest_mmap(0x41b3c000,0x8000,PROT_READ|PROT_WRITE,0x100022,-1,0)!=-17)return 6;
    if(host_guest_mmap(0x41b3c000,0x8000,PROT_READ|PROT_WRITE,0x32,-1,0)!=-22)return 6;
    if(host_guest_munmap(0x40440000,0x1700000))return 7;
    if(host_guest_mmap(0x40440000,0x1700000,PROT_READ|PROT_WRITE,0x104022,-1,0)!=0x40440000)return 7;
    if(*(volatile unsigned *)host_pointer(0x41b3fffc))return 7;
    /* A failed file-backed fixed map must release its low-pool allocation. */
    if(host_guest_mmap(0x42000000,0x4000,PROT_READ,0x100002,999999,0)!=-5)return 8;
    if(host_guest_mmap(0x42000000,0x4000,PROT_READ|PROT_WRITE,0x100022,-1,0)!=0x42000000)return 9;
    if(host_guest_mprotect(0x42000000,0x4000,PROT_READ|PROT_WRITE))return 10;
    if(host_guest_munmap(0x42000000,0x4000))return 11;
    FILE *file=tmpfile();if(!file)return 16;
    const char bytes[]="CE-map-data";
    if(fwrite(bytes,1,sizeof(bytes),file)!=sizeof(bytes)||fflush(file))return 17;
    if(host_guest_mmap(0x43000000,0x4000,PROT_READ|PROT_WRITE,0x100002,fileno(file),0)!=0x43000000)return 18;
    if(memcmp(host_pointer(0x43000000),bytes,sizeof(bytes)))return 19;
    if(host_guest_munmap(0x43000000,0x4000))return 20;
    fclose(file);
    if(host_guest_mmap(0x43000000,0x4000,-1,0x100022,-1,0)!=-12)return 21;
    if(host_guest_mmap(0x43000000,0x4000,PROT_READ|PROT_WRITE,0x100022,-1,0)!=0x43000000)return 22;
    if(host_guest_munmap(0x43000000,0x4000))return 23;
    if(host_guest_mmap(0x40441000,0x4000,PROT_READ|PROT_WRITE,0x100022,-1,0)!=-22)return 12;
    /* A wrapped 64-bit request must never become a low-pool bitmap index. */
    if(host_guest_mmap(UINT64_MAX-0x3fff,0x8000,PROT_READ|PROT_WRITE,0x100022,-1,0)!=-22)return 13;
    if(host_guest_munmap(UINT64_MAX-0x3fff,0x8000)!=-22)return 14;
    if(host_guest_mprotect(UINT64_MAX-0x3fff,0x8000,PROT_READ)!=-22)return 15;
    printf("PASS: concurrent page tracking, generation %u; reused mmap zeroed; fixed pool mapping\n",generation);
    return 0;
}
