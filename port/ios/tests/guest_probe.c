/* Exercise translated pointer layouts, stack locals, indirect calls and atomics. */
struct node { struct node *next; unsigned value; };
_Static_assert(sizeof(struct node)==8,"guest pointers must remain 32-bit");
static struct node nodes[3];
static unsigned array[128];
static volatile unsigned atomic_word;
static unsigned __attribute__((noinline)) calculate(unsigned x) { return x*7+3; }
static unsigned (*volatile callback)(unsigned)=calculate;
static unsigned long long global_pointer(void) { return (unsigned long long)(unsigned)nodes; }
/* Pointers made from sp and the frame pointer must be guest pointers too, for an
   arena at any address: passed on, stored in globals, reloaded and written through. */
static unsigned *volatile parked;
static void __attribute__((noinline)) fill(unsigned *p, unsigned n) { for(unsigned i=0;i<n;i++)p[i]=i*5; parked=p; }
static unsigned __attribute__((noinline)) stack_pointers(unsigned n) {
    unsigned fixed[16];fill(fixed,16);
    unsigned error=0,sum=0;
    if(parked!=fixed)error|=64;
    for(unsigned i=0;i<16;i++)sum+=parked[i];
    if(sum!=600)error|=64;
    unsigned variable[n];fill(variable,n);   /* alloca: sp moves and comes back */
    sum=0;for(unsigned i=0;i<n;i++)sum+=parked[i];
    if(sum!=5*n*(n-1)/2 || parked!=variable)error|=128;
    void *frame=__builtin_frame_address(0);
    if((unsigned)frame<(unsigned)fixed)error|=256;
    return error;
}
static unsigned (*volatile stack_test)(unsigned)=stack_pointers;
unsigned guest_probe(void) {
    unsigned local[32];unsigned error=0;
    for(unsigned i=0;i<32;i++)local[i]=i*3;
    for(unsigned i=0;i<128;i++)array[i]=i+1;
    nodes[0]=(struct node){&nodes[1],19};nodes[1]=(struct node){&nodes[2],23};nodes[2]=(struct node){0,29};
    unsigned sum=0;for(struct node *p=nodes;p;p=p->next)sum+=p->value;
    if(sum!=71)error|=1;
    if(callback(11)!=80)error|=2;
    sum=0;for(unsigned i=0;i<32;i++)sum+=local[i];if(sum!=1488)error|=4;
    sum=0;for(unsigned i=0;i<128;i++)sum+=array[i];if(sum!=8256)error|=8;
    atomic_word=4;if(__sync_fetch_and_add(&atomic_word,9)!=4 || atomic_word!=13)error|=16;
    error|=stack_test(40);
    return error;
}
__attribute__((section("__TEXT,__guest_header"),used))
const struct {unsigned magic; unsigned (*entry)(void); unsigned long long (*pointer)(void);} __guest_header={0x4f4c4148,guest_probe,global_pointer};
