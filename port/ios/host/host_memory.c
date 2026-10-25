/* Reserve an aligned arena and emulate Xbox pages on Apple's 16 KB pages. */
#include "ios_host.h"
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define PAGE 0x4000u
#define POOL_START 0x01000000u
#define POOL_END 0x78000000u
#define POOL_PAGES ((POOL_END-POOL_START)/PAGE)
#define WINDOW_PAGES (HALO_GUEST_WINDOW_SIZE/PAGE)
uintptr_t host_arena;
static unsigned char allocated[POOL_PAGES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t serial = 1;
static uint32_t generations[WINDOW_PAGES];
static volatile unsigned char protected_pages[WINDOW_PAGES];
static unsigned char watched_pages[WINDOW_PAGES];
static unsigned page_locks[WINDOW_PAGES];
static int watch_enabled;
static struct sigaction old_bus, old_segv;

static size_t rounded(size_t n) { return (n + PAGE - 1) & ~(size_t)(PAGE-1); }
static int in_window(uint32_t p, uint64_t n) {
    return p >= HALO_GUEST_WINDOW_BASE && (uint64_t)p+n <= (uint64_t)HALO_GUEST_WINDOW_BASE+HALO_GUEST_WINDOW_SIZE;
}

/* The arena was refused (first seen on an Apple TV HD: A8, 2 GB): log what the
   process's address space does have, since that decides what could replace a
   4 GB region at a multiple of 4 GB. */
static void arena_report(void) {
    task_vm_info_data_t info;mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&count)==KERN_SUCCESS)
        host_logf(HOST_LOG_ERROR,"address space: %llx to %llx (%llu MB), %llu MB of it mapped",
            (unsigned long long)info.min_address,(unsigned long long)info.max_address,
            (unsigned long long)(info.max_address-info.min_address)>>20,(unsigned long long)info.virtual_size>>20);
    static const unsigned megabytes[]={4096,3584,3072,2560,2048,1536,1024,512};
    for (size_t i=0;i<sizeof(megabytes)/sizeof(*megabytes);i++) {
        vm_address_t probe=0;vm_size_t size=(vm_size_t)megabytes[i]<<20;
        kern_return_t result=vm_map(mach_task_self(),&probe,size,0,VM_FLAGS_ANYWHERE,MACH_PORT_NULL,0,FALSE,
            VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_NONE);
        if (result==KERN_SUCCESS) {
            host_logf(HOST_LOG_ERROR,"address space: %u MB at any address fits, at %llx",megabytes[i],(unsigned long long)probe);
            vm_deallocate(mach_task_self(),probe,size);
            break;
        }
        host_logf(HOST_LOG_ERROR,"address space: %u MB at any address is refused too (%d)",megabytes[i],result);
    }
}

/* Set to 0 by a test, or by HALO_ARENA_SHORT=1 in the app's environment, to
   exercise the shorter reservation. */
int host_arena_try_full = 1;
uint64_t host_arena_size;

/* The guest image ends near 0x8A70B000, so a reservation of about 2.2 GiB covers
   every address the guest uses. A device that refuses 4 GiB (first seen on an
   Apple TV HD) may still give this much. Guest pointers keep their values either
   way; with the shorter range, a stray guest pointer above its end no longer
   faults inside the arena. Approach from CoreCE PR #1 (CC0). */
#define ARENA_MINIMUM_END 0x8B000000ull

static uint64_t arena_short_size(uint32_t image_base, uint32_t image_size) {
    uint64_t size = (uint64_t)image_base + image_size;
    size = (size + 0xffffffull) & ~0xffffffull;
    return size < ARENA_MINIMUM_END ? ARENA_MINIMUM_END : size;
}

/* Reserve 4 GiB at a multiple of 4 GiB, else the shorter range. Logs nothing, so it can run before main. */
static kern_return_t arena_reserve(uint64_t short_size, vm_address_t *arena, uint64_t *size, kern_return_t *full_result) {
    const char *force_short = getenv("HALO_ARENA_SHORT");
    if (force_short && *force_short && strcmp(force_short, "0")) host_arena_try_full = 0;
    *arena = 0; *size = 0x100000000ull;
    kern_return_t result = host_arena_try_full ? vm_map(mach_task_self(), arena, *size, 0xffffffffull,
        VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_NONE) : KERN_FAILURE;
    *full_result = result;
    if (result == KERN_SUCCESS && !(*arena & 0xffffffffull)) return KERN_SUCCESS;
    if (result == KERN_SUCCESS) vm_deallocate(mach_task_self(), *arena, *size);
    *arena = 0; *size = short_size;
    result = vm_map(mach_task_self(), arena, *size, 0xffffffffull,
        VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_NONE);
    if (result == KERN_SUCCESS && (*arena & 0xffffffffull)) {
        vm_deallocate(mach_task_self(), *arena, *size);
        result = KERN_NO_SPACE;
    }
    return result;
}

/* The reservation host_memory_reserve_early made, kept for host_memory_initialize. */
static vm_address_t early_arena;
static uint64_t early_size;
static kern_return_t early_full_result, early_result = KERN_FAILURE;
static int early_attempted;

void host_memory_reserve_early(uint32_t image_base, uint32_t image_size) {
    early_attempted = 1;
    early_result = arena_reserve(arena_short_size(image_base, image_size), &early_arena, &early_size, &early_full_result);
}

int host_memory_initialize(uint32_t image_base, uint32_t image_size) {
    uint64_t short_size = arena_short_size(image_base, image_size);
    vm_address_t arena;
    uint64_t size;
    kern_return_t full_result, result;
    const char *when;
    if (early_attempted && early_result == KERN_SUCCESS && early_size >= short_size) {
        arena = early_arena; size = early_size; full_result = early_full_result; result = KERN_SUCCESS;
        when = "at launch";
    } else {
        if (early_attempted && early_result == KERN_SUCCESS) vm_deallocate(mach_task_self(), early_arena, early_size);
        if (early_attempted && early_result != KERN_SUCCESS)
            host_logf(HOST_LOG_WARN,"cannot reserve the guest arena at launch (%d); trying again",early_result);
        result = arena_reserve(short_size, &arena, &size, &full_result);
        when = "now";
    }
    early_attempted = 0;
    if (full_result != KERN_SUCCESS && host_arena_try_full)
        host_logf(HOST_LOG_WARN,"cannot reserve the 4 GB guest arena (%d); trying %llu MB",full_result,(unsigned long long)(short_size>>20));
    if (result != KERN_SUCCESS) {
        host_logf(HOST_LOG_ERROR,"cannot reserve aligned guest arena: %d",result);
        arena_report();
        return -1;
    }
    host_arena_size = size;
    host_logf(HOST_LOG_INFO,"guest arena size %llu MB%s, reserved %s",(unsigned long long)(size>>20),size==0x100000000ull?"":" (reduced)",when);
    host_arena = arena;
    if (mprotect(host_pointer(HALO_GUEST_WINDOW_BASE),HALO_GUEST_WINDOW_SIZE,PROT_READ|PROT_WRITE)) return -1;
    host_logf(HOST_LOG_INFO,"guest arena at %llx, native pages %d",(unsigned long long)arena,getpagesize());
    return 0;
}

void *host_low_map(size_t size, int protection) {
    size_t pages=rounded(size)/PAGE, run=0;
    if (!pages || pages>POOL_PAGES) return NULL;
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<POOL_PAGES;i++) {
        run = allocated[i] ? 0 : run+1;
        if (run == pages) {
            size_t first=i+1-pages;
            void *p=host_pointer(POOL_START+first*PAGE);
            if (mmap(p,pages*PAGE,protection & ~PROT_EXEC,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)!=p) break;
            memset(allocated+first,1,pages);
            pthread_mutex_unlock(&lock); return p;
        }
    }
    pthread_mutex_unlock(&lock); return NULL;
}
void host_low_unmap(void *address,size_t size) {
    uint32_t p=guest_pointer(address);
    size_t pages=rounded(size)/PAGE;
    if ((p&(PAGE-1)) || p<POOL_START || (uint64_t)p+pages*PAGE>POOL_END) return;
    pthread_mutex_lock(&lock);
    mprotect(address,pages*PAGE,PROT_NONE);
    madvise(address,pages*PAGE,MADV_DONTNEED);
    memset(allocated+(p-POOL_START)/PAGE,0,pages);
    pthread_mutex_unlock(&lock);
}
/* A fixed request inside the pool takes exactly the pages asked for, if they
   are all free (Custom Edition's tag cache is linked to 0x40440000). */
static long low_map_fixed(uint32_t p,size_t size,long taken) {
    size_t first=(p-POOL_START)/PAGE,pages=rounded(size)/PAGE;
    pthread_mutex_lock(&lock);
    for (size_t i=first;i<first+pages;i++) if (allocated[i]) { pthread_mutex_unlock(&lock); return taken; }
    if (mmap(host_pointer(p),pages*PAGE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)!=host_pointer(p)) { pthread_mutex_unlock(&lock); return -12; }
    memset(allocated+first,1,pages);
    pthread_mutex_unlock(&lock); return 0;
}
/* For the replacement textures' budget (port/linux/src/texture_replacement_cache.h):
   the device's memory, in MB (0: not known). host_memory_pressure.m says what the
   system still lets the app take. */
uint32_t host_memory_physical_megabytes(void) {
    uint64_t memory=0; size_t size=sizeof(memory);
    if (sysctlbyname("hw.memsize",&memory,&size,NULL,0)) return 0;
    return (uint32_t)(memory>>20);
}
int host_low_owns(uintptr_t p,size_t n) {
    return host_arena && p>=host_arena && p-host_arena+n<=host_arena_size;
}

/* Bounded by the reservation actually made: with the shorter arena, a range
   above its end would reach host memory past it. */
static int guest_range_valid(uint64_t address,uint64_t size) {
    return size && address<host_arena_size && size<=host_arena_size-address;
}

/* Linux mmap constants are deliberately decoded here instead of using Darwin's. */
long host_guest_mmap(uint64_t address,uint64_t size,int protection,int flags,int fd,int64_t offset) {
    if (!guest_range_valid(address,size) || size>0x70000000ull) return -22;
    void *p;
    int low_allocation=0;
    if ((flags & (0x10|0x100000)) && !(address&(PAGE-1)) && address>=POOL_START && address+size<=POOL_END) {
        /* MAP_FIXED_NOREPLACE fails with EEXIST as on Linux; a plain MAP_FIXED
           is refused too rather than clobber another allocation. */
        long error=low_map_fixed((uint32_t)address,size,flags&0x100000?-17:-22);
        if (error) return error;
        p=host_pointer(address);
        low_allocation=1;
    } else if (flags & (0x10|0x100000)) {
        if (!in_window((uint32_t)address,size)) return -22;
        p=host_pointer(address);
        uint32_t first=(uint32_t)address & ~(PAGE-1);
        size_t length=rounded(address+size-first);
        if (mprotect(host_pointer(first),length,PROT_READ|PROT_WRITE)) return -12;
        memset(p,0,size);
    } else {
        p=host_low_map(size,PROT_READ|PROT_WRITE);
        if (!p) return -12;
        low_allocation=1;
    }
    if (!(flags&0x20) && fd>=0) {
        ssize_t n=pread(fd,p,size,offset);
        if (n<0) {
            if (low_allocation) host_low_unmap(p,size);
            return -5;
        }
    }
    /* Xbox 4 KB allocations can share an Apple page: protection of the
       physical window is handled by the coarser write tracker. */
    if (!in_window(guest_pointer(p),size) && mprotect(p,rounded(size),protection & ~PROT_EXEC)) {
        if (low_allocation) host_low_unmap(p,size);
        return -12;
    }
    return guest_pointer(p);
}
long host_guest_munmap(uint64_t address,uint64_t size) {
    if (!guest_range_valid(address,size)) return -22;
    if (in_window(address,size)) return 0;
    host_low_unmap(host_pointer(address),size); return 0;
}
long host_guest_mprotect(uint64_t address,uint64_t size,int protection) {
    if (!guest_range_valid(address,size)) return -22;
    uint32_t first=(uint32_t)address & ~(PAGE-1);
    if(in_window((uint32_t)address,size) && !(protection&PROT_WRITE)) {
        /* Xbox allocations/protection boundaries are 4 KB. Rounding a read-only
           buffer outwards also protects a neighboring writable zlib buffer.
           Enforce read-only only on complete 16 KB pages; boundary pages stay
           writable. VirtualProtect still records exact Xbox permissions in
           xbox_memory.c. Texture write tracking independently covers them. */
        uint64_t end=address+size;
        if(address!=first && mprotect(host_pointer(first),PAGE,PROT_READ|PROT_WRITE))return -12;
        if((end&(PAGE-1)) && mprotect(host_pointer(end&~(uint64_t)(PAGE-1)),PAGE,PROT_READ|PROT_WRITE))return -12;
        uint64_t inner_start=rounded(address),inner_end=end&~(uint64_t)(PAGE-1);
        return inner_end>inner_start && mprotect(host_pointer(inner_start),inner_end-inner_start,protection&~PROT_EXEC) ? -12:0;
    }
    return mprotect(host_pointer(first),rounded(address+size-first),protection & ~PROT_EXEC) ? -12 : 0;
}

/* A streaming worker can fault while the render thread protects the same
   16 KB page. Serialize the flag and VM change together. This lock never
   encloses guest-memory accesses, so a synchronous fault cannot reenter it. */
static void page_lock(unsigned page) {
    while(__atomic_exchange_n(&page_locks[page],1,__ATOMIC_ACQUIRE)) {}
}
static void page_unlock(unsigned page) { __atomic_store_n(&page_locks[page],0,__ATOMIC_RELEASE); }
static void mark_written(unsigned page) {
    if(mprotect(host_pointer(HALO_GUEST_WINDOW_BASE+page*PAGE),PAGE,PROT_READ|PROT_WRITE))_exit(128+SIGBUS);
    __atomic_store_n(&generations[page],__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED),__ATOMIC_RELEASE);
    protected_pages[page]=0;
}
static void fault(int sig,siginfo_t *info,void *context) {
    uintptr_t p=(uintptr_t)info->si_addr;
    if (host_low_owns(p,1) && in_window((uint32_t)p,1)) {
        unsigned page=((uint32_t)p-HALO_GUEST_WINDOW_BASE)/PAGE;
        page_lock(page);
        /* A second writer may already have repaired this page after our fault
           was queued. Remember that it was tracked even when now writable. */
        if (watched_pages[page]) { mark_written(page);page_unlock(page);return; }
        page_unlock(page);
    }
    struct sigaction *old=sig==SIGBUS?&old_bus:&old_segv;
    sigaction(sig,old,NULL);
    if ((old->sa_flags&SA_SIGINFO) && old->sa_sigaction) old->sa_sigaction(sig,info,context);
    else if (old->sa_handler!=SIG_DFL && old->sa_handler!=SIG_IGN) old->sa_handler(sig);
}
void host_install_signal_handlers(void) {
    struct sigaction action={0}; action.sa_sigaction=fault;
    action.sa_flags=SA_SIGINFO|SA_ONSTACK; sigemptyset(&action.sa_mask);
    sigaction(SIGBUS,&action,&old_bus);sigaction(SIGSEGV,&action,&old_segv);
}
void host_memory_watch_initialize(void) { watch_enabled=1; }
void host_memory_watch_protect(uint32_t address,uint32_t size) {
    if (!watch_enabled || !size || !in_window(address,size)) return;
    unsigned first=(address-HALO_GUEST_WINDOW_BASE)/PAGE;
    unsigned last=(address+size-1-HALO_GUEST_WINDOW_BASE)/PAGE;
    for(unsigned i=first;i<=last;i++) {
        page_lock(i);
        if(!protected_pages[i]) {
            watched_pages[i]=1;
            if(!mprotect(host_pointer(HALO_GUEST_WINDOW_BASE+i*PAGE),PAGE,PROT_READ))protected_pages[i]=1;
        }
        page_unlock(i);
    }
}
uint32_t host_memory_watch_serial(void) { return __atomic_load_n(&serial,__ATOMIC_RELAXED); }
uint32_t host_memory_watch_generation(uint32_t address,uint32_t size) {
    if (!size || !in_window(address,size)) return 0;
    uint32_t result=0;
    for(unsigned i=(address-HALO_GUEST_WINDOW_BASE)/PAGE;i<=(address+size-1-HALO_GUEST_WINDOW_BASE)/PAGE;i++) {
        uint32_t generation=__atomic_load_n(&generations[i],__ATOMIC_ACQUIRE);
        if(generation>result)result=generation;
    }
    return result;
}
void host_memory_watch_prepare_write(uint32_t address,uint32_t size) {
    if (!size || !in_window(address,size)) return;
    for(unsigned i=(address-HALO_GUEST_WINDOW_BASE)/PAGE;i<=(address+size-1-HALO_GUEST_WINDOW_BASE)/PAGE;i++) {
        page_lock(i);if(protected_pages[i])mark_written(i);page_unlock(i);
    }
}
void host_memory_watch_forget(uint32_t address,uint32_t size) {
    if (!size || !in_window(address,size)) return;
    for(unsigned i=(address-HALO_GUEST_WINDOW_BASE)/PAGE;i<=(address+size-1-HALO_GUEST_WINDOW_BASE)/PAGE;i++) {
        page_lock(i);mark_written(i);page_unlock(i);
    }
}
