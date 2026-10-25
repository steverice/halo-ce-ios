/* Exercise durable publication and lifecycle handoffs without a model or game process. */
#import "host_texture_upscale.m"
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
const struct texture_policy_table texture_policy_embedded={.low=12,.high=30,.sigma=1,.iterations=3,.damage_share=.1f,
    .damage_structure_loss=-1,.damage_bpf_structure_loss=-1,.damage_measure_version=1};
void host_config_string(const char *name,const char *fallback,char *out,size_t size) {
    snprintf(out,size,"%s",!strcmp(name,"debug.texture_upscale_generate")?"true":fallback);
}
void host_logf(int level,const char *format,...) {(void)level;(void)format;}
int texture_upscale_model_available(void) {return 0;}
int host_device_profile_peek(const char *root,const char *name,char *text,size_t size) {(void)root;(void)name;snprintf(text,size,"true");return 1;}
struct texture_upscale_generator *texture_upscale_generate_create(char *error,size_t size) {
    snprintf(error,size,"synthetic model failure");return NULL;
}
void texture_upscale_generate_stats(struct texture_upscale_generator *g,struct texture_upscale_model_stats *s) {(void)g;memset(s,0,sizeof(*s));}
int texture_upscale_generate(struct texture_upscale_generator *g,const struct texture_upscale_generate_input *in,
    struct texture_upscale_generate_output *out,char *error,size_t size) {(void)g;(void)in;(void)out;(void)error;(void)size;return -1;}
void texture_upscale_generate_free_output(struct texture_upscale_generate_output *out) {(void)out;}
int texture_upscale_generate_raw4x(struct texture_upscale_generator *g,const struct texture_upscale_generate_input *in,
    float **pixels,unsigned *channels,char *error,size_t size) {(void)g;(void)in;(void)pixels;(void)channels;(void)error;(void)size;return -1;}
const char *texture_upscale_encoder_name(uint32_t format) {(void)format;return "bc7-v1";}
enum texture_upscale_encode_result texture_upscale_encode_record(const struct texture_upscale_rgba_level *levels,unsigned count,
    uint32_t w,uint32_t h,uint64_t hash,uint64_t recipe,uint32_t format,unsigned alpha_mode,unsigned threads,void **bytes,size_t *size,
    struct texture_upscale_encode_metrics *metrics) {
    (void)alpha_mode;(void)levels;(void)count;(void)w;(void)h;(void)hash;(void)recipe;(void)format;(void)threads;(void)bytes;(void)size;(void)metrics;
    assert(!"unexpected encoder call");return TEXTURE_UPSCALE_ENCODE_ERROR;
}
int host_texture_source_scan(const char *root,host_texture_map_callback callback,void *context) {(void)root;(void)callback;(void)context;return 1;}
int host_texture_source_walk_map(const char *path,host_texture_source_callback callback,void *context) {(void)path;(void)callback;(void)context;return 1;}
FILE *host_texture_source_open_policy_map(const char *path,uint64_t *size,char *error,size_t capacity) {(void)path;(void)size;(void)error;(void)capacity;return NULL;}
struct texture_policy_catalog *texture_policy_catalog_new(void) {return NULL;}
void texture_policy_catalog_free(struct texture_policy_catalog *catalog) {(void)catalog;}
int texture_policy_add_map(struct texture_policy_catalog *catalog,const unsigned char *data,size_t size,char *error,size_t capacity) {
    (void)catalog;(void)data;(void)size;(void)error;(void)capacity;return -1;
}
size_t texture_policy_classify(struct texture_policy_catalog *catalog,const struct texture_policy_table *policy,
    struct texture_policy_decision **out,texture_policy_log_proc log,void *context) {(void)catalog;(void)policy;(void)out;(void)log;(void)context;return 0;}
const char *texture_policy_kind_name(enum texture_policy_kind kind) {(void)kind;return "graphic";}
const char *texture_policy_result_name(enum texture_policy_result result) {
    static const char *names[]={"s4g","bpf","original"};return names[result];
}
int main(void) {@autoreleasepool {
    char pattern[]="/tmp/visr-upscale-driver-XXXXXX";char *directory=mkdtemp(pattern);assert(directory);
    host_texture_upscale_init(directory);
    assert(!upscale_enabled&&!upscale_worker&&!generator);
    upscale_root=@(directory);texture_log=[NSMutableDictionary new];thermal_log=[NSMutableArray new];started=now();
    assert([NSFileManager.defaultManager createDirectoryAtPath:cache_directory() withIntermediateDirectories:YES attributes:nil error:nil]);
    struct texture_cache_record initial={.hash=1,.width=4,.height=4,.result=TEXTURE_POLICY_S4G,.treatment=TEXTURE_POLICY_COLOR,
        .recipe_key=11,.global_key=22,.bytes=4};
    assert(publish_record(&initial)&&manifest_count==1&&manifest[0].recipe_key==11);
    NSString *manifest_path=[cache_directory() stringByAppendingPathComponent:@"manifest.tsv"];
    assert(!unlink(manifest_path.fileSystemRepresentation));assert(!mkdir(manifest_path.fileSystemRepresentation,0700));
    struct texture_cache_record changed=initial;changed.recipe_key=33;
    assert(!publish_record(&changed)&&manifest_count==1&&manifest[0].recipe_key==11);
    NSString *entry=[cache_directory() stringByAppendingPathComponent:@"0000000000000001-4x4.bin"];
    assert([[@"test" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:entry atomically:YES]);
    decisions=calloc(1,sizeof(*decisions));decision_count=1;
    decisions[0]=(struct texture_policy_decision){.hash=1,.width=4,.height=4,.result=TEXTURE_POLICY_ORIGINAL};
    assert(!reconcile_manifest()&&manifest_count==1&&[NSFileManager.defaultManager fileExistsAtPath:entry]);
    assert(!rmdir(manifest_path.fileSystemRepresentation));assert(reconcile_manifest()&&manifest_count==0);
    assert(![NSFileManager.defaultManager fileExistsAtPath:entry]);
    struct texture_policy_decision d={.hash=2,.width=4,.height=4,.kind=TEXTURE_POLICY_GRAPHIC,.result=TEXTURE_POLICY_S4G,
        .treatment=TEXTURE_POLICY_COLOR,.scale=2};
    log_texture_error(&d,"generate","synthetic failure",.25);
    NSData *log=[NSData dataWithContentsOfFile:[upscale_root stringByAppendingPathComponent:@"texture-upscale-generate.json"]];
    NSDictionary *json=[NSJSONSerialization JSONObjectWithData:log options:0 error:nil];
    assert([json[@"textures"] count]==1&&[json[@"textures"][0][@"failed_stage"] isEqual:@"generate"]);
    /* A menu wake arriving while a pump is exiting must survive its queued flag. */
    upscale_enabled=YES;atomic_store(&pump_queued,true);atomic_store(&pump_requested,false);
    host_texture_upscale_game_started(0);assert(atomic_load(&pump_requested));
    /* Rules 4 and 6: a level's game stops the work at once; a pause lets it run only after two seconds; the game
       resuming stops it again; the next map's load frees it and makes that map first. */
    work_group=dispatch_group_create();activity_token=[NSObject new];
    host_texture_upscale_game_started(1);assert(stop_flag&&atomic_load(&upscale_playing));
    host_texture_upscale_paused(1);assert(stop_flag);
    activity.paused_since-=TEXTURE_UPSCALE_PAUSE_SECONDS;host_texture_upscale_paused(1);assert(stop_flag);
    resume_if_allowed();assert(!stop_flag&&!atomic_load(&upscale_playing));
    host_texture_upscale_paused(0);assert(stop_flag);
    unsigned serial=atomic_load(&priority_serial);
    host_texture_upscale_map_will_load("levels\\b30\\b30");
    assert(!stop_flag&&[priority_snapshot() isEqual:@"b30"]&&atomic_load(&priority_serial)==serial+1);
    /* the loading screen's wait: a level whose textures are not listed is checked, then plays original; a skip
       always does */
    assert(host_texture_upscale_level_begin("levels\\a10\\a10")&&[level_name isEqual:@"a10"]);
    assert(host_texture_upscale_level_poll(NULL,NULL)==TEXTURE_UPSCALE_LEVEL_CHECKING);
    /* a queue that stops without listing the level (here: no catalog) ends the wait */
    atomic_store(&pump_queued,false);atomic_fetch_add(&pumps_ended,1);
    assert(host_texture_upscale_level_poll(NULL,NULL)==TEXTURE_UPSCALE_LEVEL_DONE);
    host_texture_upscale_level_end(1);assert(atomic_load(&urgent)==0);
    assert(!host_texture_upscale_level_ready());
    assert(!host_texture_upscale_level_ready());   /* not listed: original, too */
    upscale_mode=TEXTURE_UPSCALE_MODE_AUTO;profile_upscales=0;
    char status[64];host_texture_upscale_status(status,sizeof(status));assert(!strcmp(status,"Not used on this device"));
    upscale_enabled=NO;
    assert(host_texture_upscale_level_ready()&&host_texture_upscale_level_poll(NULL,NULL)==TEXTURE_UPSCALE_LEVEL_NONE);
    assert(!host_texture_upscale_prepare_import(directory,NULL,NULL));
    free(decisions);free(manifest);decisions=NULL;manifest=NULL;
    assert([NSFileManager.defaultManager removeItemAtPath:upscale_root error:nil]);
    puts("texture upscale driver: publication rollback, original reconciliation, failure JSON, unavailable model, menu wake, gameplay stop, pause, level wait and readiness PASS");
}}
