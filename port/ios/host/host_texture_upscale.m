/* Apache-2.0. The on-device upscaler's scheduler (spec 2026-10-24-halo-ce-ios-on-device-upscaling-design.md, phase
3): the device profile is asked first; at import the work runs behind a progress bar with Skip, then in menus, while
a level loads and in pauses, never while a level is played; the level being loaded goes first and its loading screen
waits for it unless the player skips. One texture at a time; the manifest is the durable progress record. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#import "host_texture_upscale.h"
#import "host_astc_store.h"
#include "host_config.h"
#include "ios_host.h"
#include "texture_cache.h"
#include "texture_record.h"
#include "texture_upscale_generate.h"
#include "texture_upscale_schedule.h"
#include "host_profile.h"
#include "texture_refine.h"
#include "texture_upscale_big.h"
#include <sys/stat.h>
#include <stdatomic.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>

static dispatch_queue_t upscale_worker;
/* the level being loaded and the data root, kept with the generator off too (readiness without the generator) */
static char loading_name[64];
static NSString *any_root;
static NSString *upscale_root;
static BOOL upscale_enabled, export_enabled;
/* upscale_playing: the work must stop (a level is played); stop_flag mirrors it for Core ML and the encoders */
static atomic_bool upscale_playing, scan_pending, pump_queued, thermal_blocked, pump_requested;
static volatile int stop_flag;
/* the texture in hand and its stage (a string literal): what a stop for gameplay interrupts */
static const char *_Atomic work_stage;
static _Atomic uint64_t work_hash;
static struct texture_upscale_activity activity;
static NSObject *activity_token;
/* textures between the start of their predictions and the end of their encode: the pause handoff waits for them */
static dispatch_group_t work_group;
/* the import phase or a level's wait is on screen: the worker runs at user-initiated priority, else utility */
static atomic_int urgent;
/* the gate (rule 1): debug.texture_upscale_generate's mode, the profile's display.upscaled_textures (-1 not yet
   read), the player's switch */
static enum texture_upscale_mode upscale_mode;
static int profile_upscales=-1, player_upscales=1;
/* pumps that ended without another queued: the import phase ends at one that began after it did */
static atomic_uint pumps_ended;
/* why the last pump ended (a string literal): the import phase and the log say so; an error retries later */
static const char *_Atomic last_end;
#define RETRY_SECONDS 120
/* the level being loaded (its name as priority_name has it): its textures' keys, whether its walk is done for the
   serial it was asked for, and whether its wait was skipped */
static NSMutableDictionary<NSString *, NSData *> *map_keys;
static NSArray<NSString *> *published_paths;
static atomic_uint level_walked_serial, level_serial;
static atomic_bool level_skipped;
/* the last texture stopped for lack of storage: the pump ends "low storage" and retries later */
static atomic_bool storage_short;
/* pumps_ended when the level's wait began: a queue that stopped since without finishing the level (an error, a
   catalog that cannot be read, critical thermal state) ends the wait rather than holding the loading screen */
static atomic_uint level_pumps;
static NSString *level_name;
static NSMutableArray *generator_releases;
static NSMutableArray<NSString *> *map_paths;
static NSMutableDictionary<NSString *, NSNumber *> *decision_slots;
static NSMutableSet<NSString *> *pass_hashes;
static NSMutableDictionary<NSString *, NSDictionary *> *texture_log;
static NSMutableArray *thermal_log;
static struct texture_policy_decision *decisions;
static size_t decision_count;
static struct texture_cache_record *manifest;
static size_t manifest_count;
static struct texture_upscale_generator *generator;
static uint32_t record_format;
static uint64_t peak_footprint;
static double started;
static uint64_t footprint_before_model, generator_peak;
static double last_texture_seconds;    /* when the last texture was finished, from the generator's start */
static dispatch_queue_t finisher;    /* the second step of one texture at a time (texture_upscale_generate_end) */
static uint64_t finisher_estimate;    /* the float top of the texture the finisher holds */
#define OVERLAP_BUDGET (128ull<<20)
static atomic_bool finisher_failed;
static NSObject *state_token;    /* the manifest, texture_log and the files written from them */
static BOOL catalog_ready;
static NSString *priority_name,*sorting_priority;
static atomic_uint priority_serial;

static NSString *key(uint64_t hash, uint32_t width, uint32_t height) {
    return [NSString stringWithFormat:@"%016llx-%ux%u",(unsigned long long)hash,width,height];
}
static double now(void) { return NSProcessInfo.processInfo.systemUptime; }
/* The current footprint; peak_footprint keeps the process's ledger peak, which includes the app's own launch. */
static uint64_t footprint(void) {
    task_vm_info_data_t info={0};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&count)!=KERN_SUCCESS)return 0;
    peak_footprint=MAX(peak_footprint,(uint64_t)info.ledger_phys_footprint_peak);
    generator_peak=MAX(generator_peak,(uint64_t)info.phys_footprint);
    return info.phys_footprint;
}
/* The cache folder. On iPhone, iPad and Vision Pro: Library/Caches/texture-cache, which the system may purge under
   storage pressure (never while the app runs), is never backed up and is not shown in the Files app; a purged cache is
   made again, and readiness reads the files, so a partly purged one plays original. On tvOS the data root is already
   in Library/Caches; Macs keep it under their data folder (the runners'). */
static NSString *cache_folder_for(NSString *root) {
#if (TARGET_OS_IOS && !TARGET_OS_MACCATALYST) || TARGET_OS_VISION
    (void)root;
    return [NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES).firstObject stringByAppendingPathComponent:@TEXTURE_CACHE_DIRECTORY];
#else
    return [root stringByAppendingPathComponent:@TEXTURE_CACHE_DIRECTORY];
#endif
}
static NSString *cache_directory(void) { return cache_folder_for(upscale_root); }
/* the free space the system would let a purgeable cache use (opportunistic capacity), else the volume's free space */
static uint64_t storage_available(NSString *folder) {
#if !TARGET_OS_TV
    /* (tvOS does not offer opportunistic capacity: its whole app storage is purgeable; the volume's free space) */
    if(@available(iOS 11.0,macOS 10.13,*)){
        NSNumber *value=nil;NSError *error=nil;
        if([[NSURL fileURLWithPath:folder] getResourceValue:&value forKey:NSURLVolumeAvailableCapacityForOpportunisticUsageKey error:&error]&&value)
            return value.unsignedLongLongValue;
    }
#endif
    struct statvfs disk;
    return statvfs(folder.fileSystemRepresentation,&disk)?0:(uint64_t)disk.f_bavail*disk.f_frsize;
}
static const char *treatment_name(enum texture_policy_treatment value) {
    static const char *names[]={"skip","color","first-person","emissive","multipurpose","bump"};
    return value<=TEXTURE_POLICY_BUMP_MAP?names[value]:"skip";
}
static int bool_setting(const char *setting,const char *environment) {
    const char *override=getenv(environment);char value[32];host_config_string(setting,"false",value,sizeof(value));
    return !strcmp(override?override:value,"true")||!strcmp(override?override:value,"1");
}
static NSDictionary *model_totals(void) {
    struct texture_upscale_model_stats stats={0};if(generator)texture_upscale_generate_stats(generator,&stats);
    return @{@"calls":@(stats.predictions),@"tiles":@(stats.tiles),@"neural_engine":@(stats.seconds),@"pack":@(stats.pack_seconds),
        @"consume":@(stats.consume_seconds),@"footprint_before":@(stats.footprint_before),@"footprint_after":@(stats.footprint_after)};
}
static void save_log(void) {@synchronized(state_token){
    peak_footprint=MAX(peak_footprint,footprint());
    [thermal_log addObject:@{@"seconds":@(now()-started),@"state":@(NSProcessInfo.processInfo.thermalState)}];
    NSMutableDictionary *stage_totals=[NSMutableDictionary new];uint64_t ready=0,excluded=0;
    for(NSDictionary *row in texture_log.allValues){
        if([row[@"result_status"] isEqual:@"complete"])ready++;
        if([row[@"result_status"] isEqual:@"compression-excluded"])excluded++;
        for(NSString *stage in row[@"stage_seconds"])stage_totals[stage]=@([stage_totals[stage] doubleValue]+[row[@"stage_seconds"][stage] doubleValue]);
    }
    NSDictionary *contents=@{@"textures":texture_log.allValues,@"thermal":thermal_log,@"generator_releases":generator_releases?:@[],
        @"peak_footprint":@(peak_footprint),@"footprint_before_model":@(footprint_before_model),@"generator_peak":@(generator_peak),@"model":model_totals(),@"totals":@{@"textures":@(texture_log.count),@"catalog":@(decision_count),@"seconds":@(now()-started),@"last_texture_seconds":@(last_texture_seconds),@"ready":@(ready),@"excluded":@(excluded),@"stage_seconds":stage_totals}};
    NSData *data=[NSJSONSerialization dataWithJSONObject:contents options:NSJSONWritingPrettyPrinted error:nil];
    [data writeToFile:[upscale_root stringByAppendingPathComponent:@"texture-upscale-generate.json"] options:NSDataWritingAtomic error:nil];
}
}
static void export_table(void) {@synchronized(state_token){
    if(!export_enabled)return;
    NSMutableString *table=[NSMutableString stringWithString:@"hash\twidth\theight\tkind\tresult\ttreatment\tscale\tdamage_share\tstructure_loss\n"];
    for(size_t i=0;i<decision_count;i++) {
        struct texture_policy_decision *d=&decisions[i];NSDictionary *row=texture_log[key(d->hash,d->width,d->height)];
        [table appendFormat:@"%016llx\t%u\t%u\t%s\t%s\t%s\t%u\t%.6f\t%.6f\n",(unsigned long long)d->hash,d->width,d->height,
            texture_policy_kind_name(d->kind),row? [row[@"result"] UTF8String]:texture_policy_result_name(d->result),treatment_name(d->treatment),
            row?[row[@"scale"] unsignedIntValue]:d->scale,row?[row[@"damage_share"] doubleValue]:-1.0,row?[row[@"structure_loss"] doubleValue]:-1.0];
    }
    NSString *folder=[upscale_root stringByAppendingPathComponent:@"texture-upscale-export"];
    [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    [table writeToFile:[folder stringByAppendingPathComponent:@"policy.tsv"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
}
static int collect_map(void *context,const char *path) { (void)context;[map_paths addObject:@(path)];return 1; }
static NSString *map_base(NSString *path) { return path.lastPathComponent.stringByDeletingPathExtension.lowercaseString; }
/* The order after the level being loaded (spec "Order"): the main menu's map, the campaign in order, then the
   multiplayer maps, then community maps by name. */
static NSInteger map_rank(NSString *path) {
    static NSArray *campaign;static dispatch_once_t once;
    dispatch_once(&once,^{campaign=@[@"ui",@"a10",@"a30",@"a50",@"b30",@"b40",@"c10",@"c20",@"c40",@"d20",@"d40"];});
    NSUInteger index=[campaign indexOfObject:map_base(path)];
    if(index!=NSNotFound)return (NSInteger)index;
    return [path.stringByDeletingLastPathComponent.lastPathComponent isEqual:@"custom_maps"]?2000:1000;
}
static void order_maps(NSMutableArray<NSString *> *paths) {
    [paths sortWithOptions:NSSortStable usingComparator:^NSComparisonResult(NSString *a,NSString *b){
        NSInteger x=map_rank(a),y=map_rank(b);return x<y?NSOrderedAscending:x>y?NSOrderedDescending:NSOrderedSame;}];
}
static BOOL build_catalog(void) {
    struct texture_policy_catalog *catalog=texture_policy_catalog_new();if(!catalog)return NO;
    map_paths=[NSMutableArray new];decision_slots=[NSMutableDictionary new];
    host_texture_source_scan(upscale_root.fileSystemRepresentation,collect_map,NULL);
    BOOL ok=YES;NSMutableArray<NSString *> *classified_maps=[NSMutableArray new];
    for(NSString *path in map_paths) {
        if(atomic_load(&upscale_playing)){ok=NO;break;}
        char error[256]={0};
        uint64_t map_size=0;
        errno=0;FILE *map=host_texture_source_open_policy_map(path.fileSystemRepresentation,&map_size,error,sizeof(error));
        /* a map that cannot be read is left out (its level plays original), never the whole catalog */
        if(!map){int reason=errno;host_logf(HOST_LOG_WARN,"Upscale catalog: %s%s",error,reason==ENOTSUP?"":"; skipping the map");continue;}
        const unsigned char *mapping=mmap(NULL,(size_t)map_size,PROT_READ,MAP_PRIVATE,fileno(map),0);
        int classified=mapping!=MAP_FAILED?texture_policy_add_map(catalog,mapping,(size_t)map_size,error,sizeof(error)):-1;
        if(mapping!=MAP_FAILED)munmap((void *)mapping,(size_t)map_size);fclose(map);
        if(classified){host_logf(HOST_LOG_WARN,"Upscale catalog: %s; skipping %s",error,path.lastPathComponent.UTF8String);continue;}
        [classified_maps addObject:path];
    }
    if(ok){
        struct texture_policy_decision *classified=NULL;size_t count=texture_policy_classify(catalog,&texture_policy_embedded,&classified,NULL,NULL);
        ok=classified!=NULL;order_maps(classified_maps);map_paths=classified_maps;
        /* (the level's wait and readiness read these from other threads) */
        @synchronized(state_token){free(decisions);decisions=classified;decision_count=ok?count:0;published_paths=[classified_maps copy];}
        @synchronized(map_keys){[map_keys removeAllObjects];}
        for(size_t i=0;ok&&i<decision_count;i++)decision_slots[key(decisions[i].hash,decisions[i].width,decisions[i].height)]=@(i);
    }
    texture_policy_catalog_free(catalog);return ok;
}
static struct texture_recipe_globals recipe_globals(void) {
    const struct texture_policy_table *policy=&texture_policy_embedded;
    return (struct texture_recipe_globals){.model_id="S4g: PBRify SPAN V4 per channel",
        .model_sha256="d1f1fa1af7a5ab23fcf143e75aac4951e6c52d56bf77d9ccd4d004a4831d0461",.pipeline_version={1,1,1,1,1,1},
        .low=policy->low,.high=policy->high,.sigma=policy->sigma,.iterations=(uint32_t)policy->iterations,.post_version=1,
        .encoder=texture_upscale_encoder_name(record_format),.damage_share=policy->damage_share,.damage_structure_loss=policy->damage_structure_loss,
        .damage_bpf_structure_loss=policy->damage_bpf_structure_loss,.damage_measure_version=(uint32_t)policy->damage_measure_version};
}
/* One map's texture keys, from its own classification: what a level needs ready to play upscaled. Cached per map
   until the next catalog; nil when the map is not installed or cannot be read. Any thread. */
static NSData *classify_map_keys(NSString *path,NSString *name);
static NSData *keys_for_map(NSString *name) {
    if(!name.length)return nil;
    @synchronized(map_keys){NSData *known=map_keys[name];if(known)return known;}
    NSString *path=nil;
    @synchronized(state_token){for(NSString *candidate in published_paths)if([map_base(candidate) isEqual:name]){path=candidate;break;}}
    if(!path)return nil;
    NSData *keys=classify_map_keys(path,name);
    if(keys)@synchronized(map_keys){map_keys[name]=keys;}
    return keys;
}
/* a map file's texture keys, from its own classification; nil if it cannot be read */
static NSData *classify_map_keys(NSString *path,NSString *name) {
    char error[256]={0};uint64_t size=0;FILE *map=host_texture_source_open_policy_map(path.fileSystemRepresentation,&size,error,sizeof(error));
    if(!map){host_logf(HOST_LOG_WARN,"Upscale level %s: %s",name.UTF8String,error);return nil;}
    const unsigned char *mapping=mmap(NULL,(size_t)size,PROT_READ,MAP_PRIVATE,fileno(map),0);
    struct texture_policy_catalog *catalog=texture_policy_catalog_new();struct texture_policy_decision *found=NULL;size_t count=0;
    if(catalog&&mapping!=MAP_FAILED&&!texture_policy_add_map(catalog,mapping,(size_t)size,error,sizeof(error)))
        count=texture_policy_classify(catalog,&texture_policy_embedded,&found,NULL,NULL);
    if(mapping!=MAP_FAILED)munmap((void *)mapping,(size_t)size);fclose(map);texture_policy_catalog_free(catalog);
    if(!found)return nil;
    NSMutableData *keys=[NSMutableData dataWithLength:count*sizeof(struct texture_cache_key)];struct texture_cache_key *k=keys.mutableBytes;
    for(size_t i=0;i<count;i++)k[i]=(struct texture_cache_key){found[i].hash,found[i].width,found[i].height};
    free(found);
    return keys;
}
/* how much of a level (keys) or of the whole game (nil) has current entries */
static struct texture_cache_progress progress_of(NSData *keys) {
    struct texture_cache_progress progress={0};struct texture_recipe_globals globals=recipe_globals();
    @synchronized(state_token){
        texture_cache_progress(keys?keys.bytes:NULL,keys.length/sizeof(struct texture_cache_key),decisions,decision_count,manifest,manifest_count,&globals,&progress);
    }
    return progress;
}
/* A classifier change to original must stop the reader from serving a stale replacement. */
static BOOL reconcile_manifest(void) {
    struct texture_cache_record *kept=calloc(manifest_count?manifest_count:1,sizeof(*kept));if(!kept)return NO;
    size_t count=0;NSMutableArray *removed=[NSMutableArray new];
    for(size_t i=0;i<manifest_count;i++) {
        BOOL referenced=NO;
        for(size_t j=0;j<decision_count;j++)if(manifest[i].hash==decisions[j].hash&&manifest[i].width==decisions[j].width&&manifest[i].height==decisions[j].height) {
            referenced=decisions[j].result!=TEXTURE_POLICY_ORIGINAL;break;
        }
        if(referenced)kept[count++]=manifest[i];
        else [removed addObject:[cache_directory() stringByAppendingPathComponent:[key(manifest[i].hash,manifest[i].width,manifest[i].height) stringByAppendingString:@".bin"]]];
    }
    if(count==manifest_count){free(kept);return YES;}
    if(texture_cache_manifest_save(cache_directory().fileSystemRepresentation,kept,count)){free(kept);return NO;}
    free(manifest);manifest=kept;manifest_count=count;
    for(NSString *path in removed)[NSFileManager.defaultManager removeItemAtPath:path error:nil];
    return YES;
}
static BOOL publish_record(const struct texture_cache_record *record) {
    size_t slot=manifest_count;
    for(size_t i=0;i<manifest_count;i++)if(manifest[i].hash==record->hash&&manifest[i].width==record->width&&manifest[i].height==record->height){slot=i;break;}
    size_t count=manifest_count+(slot==manifest_count);
    struct texture_cache_record *candidate=malloc(count*sizeof(*candidate));if(!candidate)return NO;
    if(manifest_count)memcpy(candidate,manifest,manifest_count*sizeof(*candidate));candidate[slot]=*record;
    if(texture_cache_manifest_save(cache_directory().fileSystemRepresentation,candidate,count)){free(candidate);return NO;}
    free(manifest);manifest=candidate;manifest_count=count;return YES;
}
static struct texture_cache_record *find_record(const struct texture_policy_decision *d) {
    for(size_t i=0;i<manifest_count;i++)if(manifest[i].hash==d->hash&&manifest[i].width==d->width&&manifest[i].height==d->height)return &manifest[i];
    return NULL;
}
static BOOL valid_record(const struct texture_cache_record *r) {
    if(!r->bytes)return r->result==TEXTURE_POLICY_ORIGINAL;
    NSString *path=[cache_directory() stringByAppendingPathComponent:[key(r->hash,r->width,r->height) stringByAppendingString:@".bin"]];
    NSData *bytes=[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];struct texture_record_layout layout;
    return bytes&&bytes.length==r->bytes&&!texture_record_check(bytes.bytes,bytes.length,bytes.length,r->hash,r->width,r->height,record_format,&layout)&&layout.recipe_key==r->recipe_key&&!texture_record_check_payload(&layout,(const unsigned char *)bytes.bytes+layout.header_size);
}
/* Keep only one raw float prediction resident during the independent half-offset check. */
static NSArray *half_offset_scores(const struct texture_upscale_generate_input *input,char *error,size_t error_size) {
    if(!input->wrap)return nil;
    float *raw=NULL;unsigned channels=0;FILE *direct=tmpfile();if(!direct)return nil;
    uint32_t width=input->decision.width*4,height=input->decision.height*4;
    size_t values=(size_t)width*height;
    if(texture_upscale_generate_raw4x(generator,input,&raw,&channels,error,error_size)){fclose(direct);return nil;}
    if(fwrite(raw,sizeof(float)*channels,values,direct)!=values){free(raw);fclose(direct);return nil;}
    free(raw);raw=NULL;
    size_t source_bytes=(size_t)input->decision.width*input->decision.height*4;
    uint8_t *rolled=malloc(source_bytes);if(!rolled){fclose(direct);return nil;}
    uint32_t w=input->decision.width,h=input->decision.height;
    for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++) {
        uint32_t sy=(y+h-h/2)%h,sx=(x+w-w/2)%w;
        memcpy(rolled+((size_t)y*w+x)*4,input->levels[0].pixels+((size_t)sy*w+sx)*4,4);
    }
    struct texture_upscale_rgba_level level={rolled,w,h};struct texture_upscale_generate_input shifted=*input;
    shifted.levels=&level;shifted.level_count=1;
    unsigned shifted_channels=0;
    if(texture_upscale_generate_raw4x(generator,&shifted,&raw,&shifted_channels,error,error_size)||shifted_channels!=channels){free(raw);free(rolled);fclose(direct);return nil;}
    free(rolled);rewind(direct);
    float *row=malloc((size_t)width*channels*sizeof(float));double sums[4]={0};BOOL ok=row!=NULL;
    for(uint32_t y=0;ok&&y<height;y++) {
        if(fread(row,sizeof(float)*channels,width,direct)!=width){ok=NO;break;}
        uint32_t sy=(y+(h/2)*4)%height;
        for(uint32_t x=0;x<width;x++) {
            uint32_t sx=(x+(w/2)*4)%width;
            for(unsigned c=0;c<channels;c++){double delta=(double)row[(size_t)x*channels+c]-raw[((size_t)sy*width+sx)*channels+c];sums[c]+=delta*delta;}
        }
    }
    free(row);free(raw);fclose(direct);if(!ok)return nil;
    NSMutableArray *scores=[NSMutableArray new];
    for(unsigned c=0;c<channels;c++)[scores addObject:@(sums[c]==0?99:fmin(99,10*log10(values/sums[c])))];
    return scores;
}
static void log_texture_error(const struct texture_policy_decision *d,const char *stage,const char *message,double elapsed) {@synchronized(state_token){
    texture_log[key(d->hash,d->width,d->height)]=@{@"hash":[NSString stringWithFormat:@"%016llx",(unsigned long long)d->hash],
        @"width":@(d->width),@"height":@(d->height),@"kind":@(texture_policy_kind_name(d->kind)),
        @"treatment":@(treatment_name(d->treatment)),@"result":@(texture_policy_result_name(d->result)),@"scale":@(d->scale),
        @"result_status":@"error",@"failed_stage":@(stage),@"error":@(message),@"stage_seconds":@{@"total":@(elapsed)},
        @"damage_share":@(-1),@"structure_loss":@(-1)};
    save_log();export_table();
}
}
/* Every core, at most eight; half of them from serious thermal state on. */
static unsigned encoder_threads(void) {
    unsigned cores=(unsigned)MIN(MAX(NSProcessInfo.processInfo.activeProcessorCount,1),8);
    return NSProcessInfo.processInfo.thermalState>=NSProcessInfoThermalStateSerious?MAX(cores/2,1):cores;
}
struct generation_context { const struct texture_policy_decision *decision; BOOL found,failed; };
static int generate_source(void *opaque,const struct host_texture_source *source) {
    struct generation_context *context=opaque;const struct texture_policy_decision *d=context->decision;
    if(source->hash!=d->hash||source->width!=d->width||source->height!=d->height)return !atomic_load(&upscale_playing);
    context->found=YES;
    if(atomic_load(&upscale_playing)){context->failed=YES;return 0;}
    enum texture_upscale_alpha alpha=TEXTURE_UPSCALE_ALPHA_NONE;BOOL binary=YES;
    for(size_t i=0;i<(size_t)d->width*d->height;i++){uint8_t a=source->rgba[i*4+3];if(a!=255)alpha=TEXTURE_UPSCALE_ALPHA_BINARY;if(a!=0&&a!=255)binary=NO;}
    if(!binary)alpha=TEXTURE_UPSCALE_ALPHA_GRADED;
    BOOL dilate=alpha!=TEXTURE_UPSCALE_ALPHA_NONE&&(d->dilates||alpha==TEXTURE_UPSCALE_ALPHA_BINARY)&&d->treatment!=TEXTURE_POLICY_MULTIPURPOSE&&d->treatment!=TEXTURE_POLICY_BUMP_MAP;
    const struct texture_policy_table *policy=&texture_policy_embedded;
    struct texture_recipe_globals globals=recipe_globals();
    struct texture_recipe recipe={d->hash,d->width,d->height,d->result,d->treatment,d->wrap,dilate,alpha,d->scale};
    struct texture_cache_record desired={d->hash,d->width,d->height,d->result,d->treatment,
        texture_recipe_key(&globals,&recipe),texture_recipe_global_key(&globals,d->treatment,d->result),0};
    BOOL current=NO,exported=NO;
    @synchronized(state_token){
        struct texture_cache_record *previous=find_record(d);
        current=previous&&previous->recipe_key==desired.recipe_key&&previous->global_key==desired.global_key&&valid_record(previous);
        /* An export redoes a cached texture once per launch, not on every pass a thermal or foreground change starts. */
        exported=export_enabled&&texture_log[key(d->hash,d->width,d->height)]!=nil;
    }
    if(current&&(!export_enabled||exported))return 0;
    uint64_t top_width=(uint64_t)d->width*d->scale,top_height=(uint64_t)d->height*d->scale;
    unsigned channels=alpha!=TEXTURE_UPSCALE_ALPHA_NONE||d->treatment==TEXTURE_POLICY_MULTIPURPOSE?4:3;
    if((uint64_t)d->width*d->height>(1024ull*1024)||top_width>TEXTURE_RECORD_MAXIMUM_DIMENSION||top_height>TEXTURE_RECORD_MAXIMUM_DIMENSION||
        top_width*top_height*channels*sizeof(float)>(256ull<<20)) {
        desired.result=TEXTURE_POLICY_ORIGINAL;
        @synchronized(state_token){context->failed=!publish_record(&desired);
        texture_log[key(d->hash,d->width,d->height)]=@{@"hash":[NSString stringWithFormat:@"%016llx",(unsigned long long)d->hash],
            @"width":@(d->width),@"height":@(d->height),@"kind":@(texture_policy_kind_name(d->kind)),@"result":@"original",@"scale":@1,
            @"treatment":@(treatment_name(d->treatment)),@"result_status":@"working-set-excluded",@"damage_share":@(-1),@"structure_loss":@(-1)};
        }
        host_logf(HOST_LOG_WARN,"Upscale %016llx: output exceeds the bounded working set",(unsigned long long)d->hash);
        save_log();export_table();return 0;
    }
    uint64_t projected=(uint64_t)d->width*d->height*d->scale*d->scale*2+4096;
    uint64_t available=storage_available(cache_directory());
    if(!texture_cache_background_may_run(available,projected)){
        /* (the queue stops here and is tried again later; not this texture's error) */
        host_logf(HOST_LOG_WARN,"Upscale: stopped for storage: %llu MB the system offers, %llu MB needed for %016llx plus the reserve",
            (unsigned long long)(available>>20),(unsigned long long)(projected>>20),(unsigned long long)d->hash);
        atomic_store(&storage_short,true);context->failed=YES;return 0;
    }
    /* The walk's source is valid only during this call; the job keeps its own copy until the finisher is done. */
    size_t source_bytes=0;
    {uint32_t w=d->width,h=d->height;for(unsigned i=0;i<source->level_count;i++){source_bytes+=(size_t)w*h*4;w=MAX(1u,w/2);h=MAX(1u,h/2);}}
    uint8_t *owned=texture_upscale_big_alloc(source_bytes?source_bytes:1);struct texture_upscale_rgba_level *levels=calloc(source->level_count?source->level_count:1,sizeof(*levels));
    if(!owned||!levels){texture_upscale_big_free(owned);free(levels);log_texture_error(d,"generate","could not copy the source levels",0);context->failed=YES;return 0;}
    {uint32_t w=d->width,h=d->height;size_t offset=0;
        for(unsigned i=0;i<source->level_count;i++){memcpy(owned+offset,source->rgba+source->level_offsets[i],(size_t)w*h*4);
            levels[i]=(struct texture_upscale_rgba_level){owned+offset,w,h};offset+=(size_t)w*h*4;w=MAX(1u,w/2);h=MAX(1u,h/2);}}
    struct texture_upscale_generate_input input={*d,policy,levels,source->level_count,alpha,d->wrap,dilate,d->pinned};
    char error[512]={0};double start=now();
    dispatch_group_enter(work_group);
    atomic_store(&work_hash,d->hash);atomic_store(&work_stage,"predictions");
    if(!generator){footprint_before_model=footprint();generator=texture_upscale_generate_create(error,sizeof(error));
        host_logf(HOST_LOG_INFO,"Upscale: made the generator at %llu MB",(unsigned long long)(footprint_before_model>>20));}
    /* Two textures overlap only while both float tops fit in OVERLAP_BUDGET together, so the peak is set by the
       largest texture alone; before a large one, freed memory goes back to the system. */
    uint64_t estimate=(uint64_t)d->width*d->height*d->scale*d->scale*16;
    if(estimate+finisher_estimate>OVERLAP_BUDGET){dispatch_sync(finisher,^{});malloc_zone_pressure_relief(NULL,0);}
    struct texture_upscale_job *job=NULL;
    if(!generator||texture_upscale_generate_begin(generator,&input,&job,error,sizeof(error))){
        /* (a stop for gameplay ends predictions with an error: the texture is simply made later) */
        if(stop_flag)host_logf(HOST_LOG_INFO,"Upscale %016llx: stopped for gameplay",(unsigned long long)d->hash);
        else {peak_footprint=MAX(peak_footprint,footprint());log_texture_error(d,"generate",error,now()-start);
            host_logf(HOST_LOG_WARN,"Upscale %016llx: %s",(unsigned long long)d->hash,error);}
        context->failed=YES;texture_upscale_big_free(owned);free(levels);dispatch_group_leave(work_group);return 0;
    }
    double begun=now();
    /* One texture's CPU work, encode and publication run on the finisher while the walk goes on to predict the next. */
    dispatch_sync(finisher,^{});
    finisher_estimate=estimate;
    struct texture_policy_decision decision=*d;
    dispatch_async(finisher,^{@autoreleasepool {
      void (^finish)(void)=^{
        const struct texture_policy_decision *d=&decision;
        texture_upscale_encode_set_cancel(&stop_flag);
        atomic_store(&work_hash,decision.hash);atomic_store(&work_stage,"refinement, measures and compression");
        /* gameplay began while the predictions ran: nothing more, the texture is made later */
        if(stop_flag){texture_upscale_generate_abandon(job);texture_upscale_big_free(owned);free(levels);finisher_estimate=0;return;}
        struct texture_cache_record record=desired;
        struct texture_upscale_generate_input job_input=input;
        struct texture_upscale_generate_output output={0};char error[512]={0};
        BOOL failed=NO;
        double end_start=now();
        if(texture_upscale_generate_end(generator,job,&output,error,sizeof(error))){
            if(stop_flag){texture_upscale_big_free(owned);free(levels);finisher_estimate=0;return;}
            peak_footprint=MAX(peak_footprint,footprint());log_texture_error(d,"generate",error,now()-start);
            host_logf(HOST_LOG_WARN,"Upscale %016llx: %s",(unsigned long long)d->hash,error);atomic_store(&finisher_failed,true);texture_upscale_big_free(owned);free(levels);return;
        }
        struct texture_upscale_encode_metrics metrics={0};void *bytes=NULL;size_t size=0;double encode_start=now();
        enum texture_upscale_encode_result encoded=TEXTURE_UPSCALE_ENCODE_OK;
        if(output.result!=TEXTURE_POLICY_ORIGINAL)encoded=texture_upscale_encode_record(output.levels,output.level_count,d->width,d->height,d->hash,
            record.recipe_key,record_format,(unsigned)alpha|(d->treatment==TEXTURE_POLICY_BUMP_MAP?TEXTURE_UPSCALE_ENCODE_NORMALS:0),encoder_threads(),&bytes,&size,&metrics);
        if(stop_flag&&encoded==TEXTURE_UPSCALE_ENCODE_ERROR){free(bytes);texture_upscale_generate_free_output(&output);texture_upscale_big_free(owned);free(levels);finisher_estimate=0;
            host_logf(HOST_LOG_INFO,"Upscale %016llx: stopped for gameplay",(unsigned long long)d->hash);return;}
        uint64_t encoded_footprint=footprint();
        NSString *steps=[NSString stringWithFormat:@"%s encoded %llu",texture_upscale_generate_trace(generator),(unsigned long long)(encoded_footprint>>20)];
        double encode_seconds=now()-encode_start;BOOL excluded=encoded==TEXTURE_UPSCALE_ENCODE_EXCLUDED;
        if(encoded==TEXTURE_UPSCALE_ENCODE_ERROR){peak_footprint=MAX(peak_footprint,footprint());log_texture_error(d,"encode","encoder failed",now()-start);atomic_store(&finisher_failed,true);texture_upscale_generate_free_output(&output);texture_upscale_big_free(owned);free(levels);return;}
        record.result=excluded?TEXTURE_POLICY_ORIGINAL:output.result;record.bytes=size;
        double write_start=now();
        if(size&&texture_cache_write_entry(cache_directory().fileSystemRepresentation,&record,bytes,size))failed=YES;
        @synchronized(state_token){if(!failed&&!publish_record(&record))failed=YES;}
        if(export_enabled&&output.level_count){
            NSString *folder=[upscale_root stringByAppendingPathComponent:@"texture-upscale-export"];
            [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
            uint32_t header[3]={output.levels[0].width,output.levels[0].height,1};
            NSString *path=[folder stringByAppendingPathComponent:[NSString stringWithFormat:@"%016llx.rgba",(unsigned long long)d->hash]];
            NSString *temporary=[path stringByAppendingString:@".tmp"];FILE *file=fopen(temporary.fileSystemRepresentation,"wb");
            if(file){BOOL ok=fwrite(header,1,sizeof(header),file)==sizeof(header)&&fwrite(output.levels[0].pixels,4,(size_t)header[0]*header[1],file)==(size_t)header[0]*header[1];
                ok=fclose(file)==0&&ok;if(ok)rename(temporary.fileSystemRepresentation,path.fileSystemRepresentation);else unlink(temporary.fileSystemRepresentation);}
        }
        struct texture_upscale_model_stats stats={0};texture_upscale_generate_stats(generator,&stats);
        NSDictionary *row=@{@"hash":[NSString stringWithFormat:@"%016llx",(unsigned long long)d->hash],@"width":@(d->width),@"height":@(d->height),
            @"kind":@(texture_policy_kind_name(d->kind)),@"result":@(texture_policy_result_name(record.result)),@"treatment":@(treatment_name(d->treatment)),
            @"scale":@(record.result==TEXTURE_POLICY_ORIGINAL?1:output.scale),@"format":metrics.raw?@"bgra8":record_format==TEXTURE_RECORD_BC7?@"bc7":@"astc4x4",@"alpha_psnr":@(isfinite(metrics.alpha_psnr)?metrics.alpha_psnr:999),@"mean_angle":@(metrics.mean_angle),
            @"stage_seconds":@{@"predict":@(output.seconds.predict),@"refine":@(output.seconds.refine),@"damage":@(output.seconds.damage),@"bump":@(output.seconds.bump),@"mips":@(output.seconds.mips),@"encode":@(encode_seconds),@"write":@(now()-write_start),
                @"begin":@(begun-start),@"end":@(encode_start-end_start),@"total":@(now()-start)},
            @"psnr":@(isfinite(metrics.rgb_psnr)?metrics.rgb_psnr:999),@"binary_alpha_max_error":@(metrics.binary_alpha_max_error),@"psnr_mask":@"alpha>0",
            @"compression_gate":@(!excluded),@"encoder_attempts":@(metrics.attempts),@"encoder_preset":@(metrics.preset),@"result_status":failed?@"write-error":excluded?@"compression-excluded":metrics.raw?@"complete-raw":@"complete",
            @"damage_share":@(output.damage_share),@"structure_loss":@(output.structure_loss_s4g),
            @"footprint_peak":@(MAX(stats.texture_peak,encoded_footprint)),@"footprint_steps":steps};
        host_logf(HOST_LOG_INFO,"Upscale %016llx: %s, damage %.4f in %.3fs, compression %.2f dB",(unsigned long long)d->hash,
            failed?"write failed":excluded?"kept original":"complete",output.damage_share,output.seconds.damage,metrics.rgb_psnr);
        free(bytes);texture_upscale_generate_free_output(&output);
        if(export_enabled&&job_input.wrap&&!failed) {
            double seam_start=now();NSArray *scores=half_offset_scores(&job_input,error,sizeof(error));
            NSMutableDictionary *seam=[row mutableCopy];
            if(scores){seam[@"half_offset_psnr"]=scores;seam[@"half_offset_stage"]=@"raw4x";}
            else {seam[@"seam_error"]=@(error[0]?error:"half-offset prediction failed");host_logf(HOST_LOG_WARN,"Upscale seam %016llx: %s",(unsigned long long)d->hash,error);}
            NSMutableDictionary *times=[seam[@"stage_seconds"] mutableCopy];times[@"seam"]=@(now()-seam_start);seam[@"stage_seconds"]=times;
            row=seam;
        }
        if(failed)atomic_store(&finisher_failed,true);
        texture_upscale_big_free(owned);free(levels);
        if(estimate>=(32ull<<20))malloc_zone_pressure_relief(NULL,0);
        finisher_estimate=0;
        @synchronized(state_token){peak_footprint=MAX(peak_footprint,stats.peak_footprint);generator_peak=MAX(generator_peak,stats.sampled_peak);
            last_texture_seconds=now()-started;texture_log[key(d->hash,d->width,d->height)]=row;save_log();export_table();}
      };
      finish();dispatch_group_leave(work_group);
    }});
    return 0;
}
static BOOL thermal_may_run(void) {
    NSProcessInfoThermalState state=NSProcessInfo.processInfo.thermalState;
    if(state==NSProcessInfoThermalStateCritical)atomic_store(&thermal_blocked,true);
    if(state<=NSProcessInfoThermalStateFair)atomic_store(&thermal_blocked,false);
    return !atomic_load(&thermal_blocked);
}
struct map_generation_context { unsigned serial; BOOL failed; };
static int generate_map_source(void *opaque,const struct host_texture_source *source) {
    struct map_generation_context *map=opaque;
    if(atomic_load(&finisher_failed)){map->failed=YES;return 0;}
    if(atomic_load(&upscale_playing)||!thermal_may_run()||atomic_load(&scan_pending)||map->serial!=atomic_load(&priority_serial))return 0;
    NSString *name=key(source->hash,source->width,source->height);NSNumber *slot=decision_slots[name];
    if(!slot||[pass_hashes containsObject:name])return 1;
    const struct texture_policy_decision *decision=&decisions[slot.unsignedLongLongValue];
    if(decision->result==TEXTURE_POLICY_ORIGINAL)return 1;
    struct generation_context context={decision,NO,NO};generate_source(&context,source);
    map->failed=context.failed;
    if(!context.failed)[pass_hashes addObject:name];
    return !map->failed&&!atomic_load(&upscale_playing)&&thermal_may_run()&&!atomic_load(&scan_pending)&&map->serial==atomic_load(&priority_serial);
}
static void schedule_pump(void);
static NSString *priority_snapshot(void) { @synchronized(activity_token){return priority_name;} }
/* The generator holds the model and its working memory; it goes whenever the queue stops (empty, gameplay, an
   error), so none of it is kept across gameplay, and is made again when there is work. */
static void release_generator(const char *why) {
    if(!generator)return;
    dispatch_sync(finisher,^{});
    uint64_t before=footprint();
    texture_upscale_generate_destroy(generator);generator=NULL;
    malloc_zone_pressure_relief(NULL,0);
    uint64_t after=footprint();
    host_logf(HOST_LOG_INFO,"Upscale: released the generator (%s): footprint %llu MB, then %llu MB; %llu MB before it was made",
        why,(unsigned long long)(before>>20),(unsigned long long)(after>>20),(unsigned long long)(footprint_before_model>>20));
    @synchronized(state_token){[generator_releases addObject:@{@"seconds":@(now()-started),@"why":@(why),@"before":@(before),
        @"after":@(after),@"before_model":@(footprint_before_model)}];}
}
static BOOL text_true(const char *text);
/* display.upscaled_textures as config.toml has it now (the Settings app writes it during the launch) */
static BOOL player_switch(void) {
    char player[16]="";const char *environment=getenv("HALO_UPSCALED_TEXTURES");
    if(environment)snprintf(player,sizeof(player),"%s",environment);else host_config_string("display.upscaled_textures",profile_upscales?"true":"false",player,sizeof(player));
    return text_true(player);
}
/* B4: the Settings app's delete runs on the worker, between textures, so no write can follow it from memory */
static atomic_bool delete_requested;
static void delete_cache(void) {
    if(texture_cache_delete_all(cache_directory().fileSystemRepresentation))host_logf(HOST_LOG_WARN,"Upscale: could not delete the whole texture cache");
    @synchronized(state_token){free(manifest);manifest=NULL;manifest_count=0;[texture_log removeAllObjects];}
    [NSFileManager.defaultManager createDirectoryAtPath:cache_directory() withIntermediateDirectories:YES attributes:nil error:nil];
    host_logf(HOST_LOG_INFO,"Upscale: deleted the upscaled textures (Settings)");
}
static void pump(void) {
    @autoreleasepool {
        NSMutableArray<NSString *> *ordered=nil;
        NSString *first=nil;
        BOOL complete=YES;
        unsigned serial=0;
        const char *why="queue empty";
        atomic_store(&pump_requested,false);
        if(atomic_load(&upscale_playing)){why="gameplay";goto done;}
        if(!thermal_may_run()){why="critical thermal state";goto done;}
        if(upscale_mode==TEXTURE_UPSCALE_MODE_AUTO&&!player_switch()){why="the player's switch is off";goto done;}
        {uint64_t available=storage_available(cache_directory());
            if(!texture_cache_background_may_run(available,1<<20)){
                host_logf(HOST_LOG_WARN,"Upscale: not starting: %llu MB of storage the system offers, under the reserve",(unsigned long long)(available>>20));
                why="low storage";goto done;}}
        if(atomic_exchange(&delete_requested,false))delete_cache();
        atomic_store(&finisher_failed,false);
        if(atomic_exchange(&scan_pending,false)){
            catalog_ready=NO;
            /* (the Settings app's delete may have emptied the folder since the manifest was read) */
            struct texture_cache_record *loaded=NULL;size_t count=0;
            if(!texture_cache_manifest_load(cache_directory().fileSystemRepresentation,&loaded,&count))
                @synchronized(state_token){free(manifest);manifest=loaded;manifest_count=count;}
        }
        if(!catalog_ready){catalog_ready=build_catalog();if(!catalog_ready){why="catalog";goto done;}
            BOOL reconciled;@synchronized(state_token){reconciled=reconcile_manifest();
                /* (for readiness while the generator is off: texture_cache_decisions_load) */
                if(texture_cache_decisions_save(cache_directory().fileSystemRepresentation,decisions,decision_count))
                    host_logf(HOST_LOG_WARN,"Upscale: could not save the decisions");}
            if(!reconciled){catalog_ready=NO;why="manifest";goto done;}}
        serial=atomic_load(&priority_serial);
        sorting_priority=priority_snapshot();
        ordered=[map_paths mutableCopy];pass_hashes=[NSMutableSet new];
        if(sorting_priority.length) {
            for(NSString *path in map_paths)if([map_base(path) isEqual:sorting_priority]) {
                [ordered removeObject:path];[ordered insertObject:path atIndex:0];first=path;break;
            }
            /* (the level's wait shows its progress from these; a map not installed has nothing to wait for) */
            keys_for_map(sorting_priority);
            if(!first)atomic_store(&level_walked_serial,serial);
        }
        /* Every prediction uses the same 288x288 shape, so walking each map
           once preserves shape grouping without inflating it for each texture. */
        for(NSString *path in ordered){@autoreleasepool {
            if(serial!=atomic_load(&priority_serial)||atomic_load(&scan_pending)||atomic_load(&upscale_playing)||!thermal_may_run()){complete=NO;break;}
            struct map_generation_context context={serial,NO};
            int result=host_texture_source_walk_map(path.fileSystemRepresentation,generate_map_source,&context);
            if(result<0)host_logf(HOST_LOG_WARN,"Upscale: malformed source map");
            if(result!=1||context.failed){complete=NO;break;}
            /* the level being loaded is done once its last texture is encoded */
            if(path==first&&serial==atomic_load(&priority_serial)){dispatch_sync(finisher,^{});atomic_store(&level_walked_serial,serial);}
        }}
        dispatch_sync(finisher,^{});
        pass_hashes=nil;
        export_table();save_log();
        if(!complete)why=atomic_exchange(&storage_short,false)?"low storage":atomic_load(&upscale_playing)?"gameplay":atomic_load(&finisher_failed)?"error":"interrupted";
    done:
        atomic_store(&pump_queued,false);
        if((atomic_load(&scan_pending)||atomic_load(&pump_requested))&&!atomic_load(&upscale_playing)&&thermal_may_run())schedule_pump();
        else {release_generator(why);save_log();atomic_store(&last_end,why);atomic_fetch_add(&pumps_ended,1);
            /* an error (one texture, the catalog, the manifest) is tried again later rather than left until the next
               map load or foreground */
            if(!strcmp(why,"error")||!strcmp(why,"catalog")||!strcmp(why,"manifest")||!strcmp(why,"low storage"))
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)RETRY_SECONDS*NSEC_PER_SEC),dispatch_get_main_queue(),^{
                    if(!atomic_load(&upscale_playing))schedule_pump();});}
    }
}
/* A user-initiated block on the worker's serial queue raises the pump already running there to that priority. */
static void boost(void) { if(upscale_worker)dispatch_async(upscale_worker,dispatch_block_create_with_qos_class(DISPATCH_BLOCK_ENFORCE_QOS_CLASS,QOS_CLASS_USER_INITIATED,0,^{})); }
/* One pump at a time; at user-initiated priority while the import phase or a level's wait is on screen. */
static void schedule_pump(void) {
    if(!upscale_enabled)return;
    atomic_store(&pump_requested,true);
    if(!atomic_exchange(&pump_queued,true)) {
        dispatch_qos_class_t qos=atomic_load(&urgent)>0?QOS_CLASS_USER_INITIATED:QOS_CLASS_UTILITY;
        dispatch_async(upscale_worker,dispatch_block_create_with_qos_class(DISPATCH_BLOCK_ENFORCE_QOS_CLASS,qos,0,^{pump();}));
    }
}
static void set_stop(BOOL stop) { stop_flag=stop?1:0;atomic_store(&upscale_playing,stop); }
/* Rule 4: a level's game is about to run. Predictions stop before their next call and the encoders at their next
   slice; this waits for the texture in hand to let go, so the first gameplay frame never shares the device. */
static void stop_for_gameplay(const char *why) {
    if(!upscale_enabled)return;
    /* what the stop interrupts: none, or a texture in its predictions or its CPU stage (for the log, and the proof) */
    const char *stage=atomic_load(&work_stage);uint64_t hash=atomic_load(&work_hash);
    BOOL in_flight=dispatch_group_wait(work_group,DISPATCH_TIME_NOW)!=0;
    set_stop(YES);
    double began=now();
    long busy=dispatch_group_wait(work_group,dispatch_time(DISPATCH_TIME_NOW,(int64_t)(3*NSEC_PER_SEC)));
    char held[96]="";
    if(in_flight)snprintf(held,sizeof(held),", interrupting %016llx in %s",(unsigned long long)hash,stage?stage:"its work");
    host_logf(busy?HOST_LOG_WARN:HOST_LOG_INFO,"Upscale: paused for gameplay (%s) after %.0f ms%s%s",why,(now()-began)*1000,held,busy?", a texture still running":"");
}
/* Re-reads whether the work may run now (activity), and resumes it if so. */
static void resume_if_allowed(void) {
    BOOL may;double wait;
    @synchronized(activity_token){may=texture_upscale_activity_may_run(&activity,now());wait=texture_upscale_activity_wait(&activity,now());}
    if(may){set_stop(NO);schedule_pump();return;}
    if(wait>0)dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(wait*NSEC_PER_SEC)+NSEC_PER_MSEC),dispatch_get_main_queue(),^{resume_if_allowed();});
}
static void resume_if_allowed(void);
int host_texture_upscale_delete_cache(const char *directory) {
    if(!upscale_enabled)return texture_cache_delete_all(directory);
    /* stop the texture in hand; the next pump deletes before anything else */
    atomic_store(&delete_requested,true);set_stop(YES);
    dispatch_async(upscale_worker,^{if(atomic_exchange(&delete_requested,false))delete_cache();
        dispatch_async(dispatch_get_main_queue(),^{resume_if_allowed();});});
    return 0;
}
void host_texture_upscale_scan(void) {if(upscale_enabled){atomic_store(&scan_pending,true);schedule_pump();}}
static BOOL text_true(const char *text) { return !strcmp(text,"true")||!strcmp(text,"1"); }
void host_texture_upscale_resolve_profile(const char *root) {
    if(profile_upscales>=0||!root)return;
    char text[32]="";
    /* (the game's own default where no profile sets it: on) */
    profile_upscales=host_device_profile_peek(root,"display.upscaled_textures",text,sizeof(text))?text_true(text):1;
}
void host_texture_upscale_init(const char *root) {
    static dispatch_once_t once;dispatch_once(&once,^{
        /* Development only: HALO_TEXTURE_UPSCALE_CLEAN removes what the generator and its export left in the data
           root, since a device's app container cannot be emptied from the Mac. */
#ifdef HALO_DEVELOPMENT_HOOKS
        if(root&&getenv("HALO_TEXTURE_UPSCALE_CLEAN")) {
            NSString *base=@(root);NSFileManager *files=NSFileManager.defaultManager;
            if(texture_cache_delete_all(cache_folder_for(base).fileSystemRepresentation))
                host_logf(HOST_LOG_WARN,"Upscale: could not delete the whole texture cache");
            [files removeItemAtPath:[base stringByAppendingPathComponent:@"texture-upscale-export"] error:nil];
            [files removeItemAtPath:[base stringByAppendingPathComponent:@"texture-upscale-generate.json"] error:nil];
            /* and packages copied in for HALO_TEXTURE_UPSCALE_MODEL */
            for(NSString *name in [files contentsOfDirectoryAtPath:base error:nil])
                if([name.pathExtension isEqual:@"mlmodelc"])[files removeItemAtPath:[base stringByAppendingPathComponent:name] error:nil];
            host_logf(HOST_LOG_INFO,"Upscale: removed the texture cache, the export and the generator log");
            return;
        }
#endif
        if(!root)return;
        any_root=@(root);
#if (TARGET_OS_IOS && !TARGET_OS_MACCATALYST) || TARGET_OS_VISION
        /* the cache's old place under the data root (Documents, shown in the Files app and backed up): gone */
        NSString *old=[any_root stringByAppendingPathComponent:@TEXTURE_CACHE_DIRECTORY];
        if([NSFileManager.defaultManager fileExistsAtPath:old])
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
                if(texture_cache_delete_all(old.fileSystemRepresentation)==0)rmdir(old.fileSystemRepresentation);
                host_logf(HOST_LOG_INFO,"Upscale: removed the cache's old folder under the data root");});
#endif
        /* Rule 1, before anything else: the profile, then the player's switch, then a Neural Engine (rule 8). */
        char mode[16]="auto",player[16]="";
        const char *environment=getenv("HALO_TEXTURE_UPSCALE_GENERATE");
        if(environment)snprintf(mode,sizeof(mode),"%s",environment);else host_config_string("debug.texture_upscale_generate","auto",mode,sizeof(mode));
        upscale_mode=texture_upscale_mode_parse(mode);
        /* "auto" stays out of measured runs (a benchmark suite, a benchmark replay, any input replay, a fixed timestep,
           screenshots): their menus
           would otherwise heat the device and draw on the Neural Engine between takes. "force" still runs. */
        if(upscale_mode==TEXTURE_UPSCALE_MODE_AUTO){
            char suite[32]="",replay[256]="",bench[16]="";const char *e=getenv("HALO_BENCHMARK_SUITE");
            if(e)snprintf(suite,sizeof(suite),"%s",e);else host_config_string("debug.benchmark_suite","",suite,sizeof(suite));
            e=getenv("HALO_INPUT_REPLAY");if(e)snprintf(replay,sizeof(replay),"%s",e);else host_config_string("debug.input_replay","",replay,sizeof(replay));
            e=getenv("HALO_BENCHMARK");if(e)snprintf(bench,sizeof(bench),"%s",e);else host_config_string("debug.benchmark","false",bench,sizeof(bench));
            /* (and the runs that must draw the same frames every time: a fixed timestep or screenshots) */
            char fixed[16]="",shots[16]="";
            e=getenv("HALO_FIXED_TIMESTEP");if(e)snprintf(fixed,sizeof(fixed),"%s",e);else host_config_string("debug.fixed_timestep","false",fixed,sizeof(fixed));
            e=getenv("HALO_SCREENSHOT_EVERY");if(e)snprintf(shots,sizeof(shots),"%s",e);else host_config_string("debug.screenshot_every","0",shots,sizeof(shots));
            BOOL deterministic=text_true(fixed)||atoi(shots)>0;
            if(suite[0]||replay[0]||text_true(bench)||deterministic){
                host_logf(HOST_LOG_INFO,"Upscale: off for this measured run (%s)",suite[0]?"benchmark suite":replay[0]?"input replay":text_true(bench)?"benchmark":"fixed timestep or screenshots");
                upscale_mode=TEXTURE_UPSCALE_MODE_OFF;
            }
        }
        host_texture_upscale_resolve_profile(root);
        environment=getenv("HALO_UPSCALED_TEXTURES");
        if(environment)snprintf(player,sizeof(player),"%s",environment);else host_config_string("display.upscaled_textures",profile_upscales?"true":"false",player,sizeof(player));
        player_upscales=text_true(player);
        if(upscale_mode==TEXTURE_UPSCALE_MODE_OFF)return;
        int model=texture_upscale_model_available();
        if(!texture_upscale_gate(upscale_mode,profile_upscales,player_upscales,model)){
            host_logf(HOST_LOG_INFO,"Upscale: off on this device (profile %s, switch %s, Neural Engine %s)",profile_upscales?"on":"off",
                player_upscales?"on":"off",model?"available":"unavailable");
            return;
        }
        host_logf(HOST_LOG_INFO,"Upscale: on (%s; profile %s, switch %s)",upscale_mode==TEXTURE_UPSCALE_MODE_FORCE?"forced":"auto",
            profile_upscales?"on":"off",player_upscales?"on":"off");
        upscale_enabled=YES;export_enabled=bool_setting("debug.texture_upscale_export","HALO_TEXTURE_UPSCALE_EXPORT");upscale_root=@(root);started=now();
        /* Background work at utility; the import phase and a level's wait raise each pump to user-initiated. */
        upscale_worker=dispatch_queue_create("org.visr.texture-upscale",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0));
        finisher=dispatch_queue_create("org.visr.texture-upscale.finish",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0));
        state_token=[NSObject new];activity_token=[NSObject new];work_group=dispatch_group_create();
        map_keys=[NSMutableDictionary new];generator_releases=[NSMutableArray new];
        texture_upscale_model_set_cancel(&stop_flag);texture_refine_set_cancel(&stop_flag);
        texture_log=[NSMutableDictionary new];thermal_log=[NSMutableArray new];
        [NSFileManager.defaultManager createDirectoryAtPath:cache_directory() withIntermediateDirectories:YES attributes:nil error:nil];
        [[NSURL fileURLWithPath:cache_directory()] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        texture_cache_manifest_load(cache_directory().fileSystemRepresentation,&manifest,&manifest_count);
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();record_format=TEXTURE_RECORD_ASTC4X4;
        /* rule 7: ASTC 4x4 where the GPU has no BC */
        if(@available(iOS 16.4,tvOS 16.4,macOS 11.0,*))if(device.supportsBCTextureCompression)record_format=TEXTURE_RECORD_BC7;
        host_logf(HOST_LOG_INFO,"Upscale: records are %s",record_format==TEXTURE_RECORD_BC7?"BC7":"ASTC 4x4");
        [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;if(NSProcessInfo.processInfo.thermalState<=NSProcessInfoThermalStateFair&&!atomic_load(&upscale_playing))schedule_pump();}];
        #if TARGET_OS_OSX
        NSString *active_notification=NSApplicationWillBecomeActiveNotification;
#else
        NSString *active_notification=UIApplicationWillEnterForegroundNotification;
#endif
        [NSNotificationCenter.defaultCenter addObserverForName:active_notification object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;host_texture_upscale_scan();}];
        host_texture_upscale_scan();
    });
}
int host_texture_upscale_enabled(void) { return upscale_enabled; }
static void set_priority(const char *name) {
    NSString *normalized=[@(name) stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
    @synchronized(activity_token){priority_name=map_base(normalized);}
    atomic_fetch_add(&priority_serial,1);
}
void host_texture_upscale_map_will_load(const char *name) {
    if(name){NSString *n=map_base([@(name) stringByReplacingOccurrencesOfString:@"\\" withString:@"/"]);snprintf(loading_name,sizeof(loading_name),"%s",n.UTF8String);}
    if(!upscale_enabled)return;
    if(name)set_priority(name);
    @synchronized(activity_token){texture_upscale_activity_map_will_load(&activity);}
    set_stop(NO);schedule_pump();
}
void host_texture_upscale_game_started(int is_game) {
    if(!upscale_enabled)return;
    @synchronized(activity_token){texture_upscale_activity_game_started(&activity,is_game);}
    /* Development only (HALO_TEXTURE_UPSCALE_STOP_TEST): hold the level's start until a texture is in hand, up to
       10 s, so a device test can show the stop interrupting one mid-texture and how long that takes. */
#ifdef HALO_DEVELOPMENT_HOOKS
    /* (compiled into development builds only, never an App Store package: HALO_DEVELOPMENT_HOOKS) */
    if(is_game&&getenv("HALO_TEXTURE_UPSCALE_STOP_TEST")){
        double until=now()+10;
        while(now()<until&&dispatch_group_wait(work_group,DISPATCH_TIME_NOW)==0)usleep(2000);
        /* (into the texture's predictions or its CPU stage, not at its first instant) */
        usleep(30000);
    }
#endif
    if(is_game)stop_for_gameplay("a level's game started");
    else {set_stop(NO);schedule_pump();}
}
/* rule 6: a pause is not gameplay (game_time_set_paused) */
void host_texture_upscale_paused(int paused) {
    if(!upscale_enabled)return;
    BOOL in_game,was;
    @synchronized(activity_token){in_game=activity.in_game;was=activity.paused;texture_upscale_activity_paused(&activity,paused,now());}
    if(!in_game||was==(paused!=0))return;
    if(paused)resume_if_allowed();
    else stop_for_gameplay("the game resumed");
}

/* ---------- the import phase (rule 2) */
int host_texture_upscale_prepare_import(const char *root,host_texture_upscale_progress progress,void *context) {
    host_texture_upscale_init(root);
    if(!upscale_enabled)return 0;
    atomic_fetch_add(&urgent,1);
    unsigned ended=atomic_load(&pumps_ended);
    host_texture_upscale_scan();boost();
    /* the player is on the import's phase: iOS 26 may carry it on if they switch away */
    host_texture_upscale_continued_phase(1);
    double start=now();BOOL skipped=NO;
    for(;;) {
        struct texture_cache_progress p={0};
        if(catalog_ready)p=progress_of(nil);
        /* done: a pump has ended since the import's scan with nothing queued after it */
        BOOL idle=atomic_load(&pumps_ended)!=ended&&!atomic_load(&pump_queued);
        if(progress&&!progress(context,p.ready_units,p.units)){skipped=YES;break;}
        if(idle)break;
        usleep(100000);
    }
    struct texture_cache_progress p=progress_of(nil);
    const char *end=atomic_load(&last_end);
    BOOL finished=!skipped&&end&&!strcmp(end,"queue empty");
    host_logf(HOST_LOG_INFO,"Upscale import: %s after %.1f s, %zu of %zu textures ready%s%s",skipped?"skipped":finished?"finished":"stopped",
        now()-start,p.ready,p.textures,skipped||finished?"":": ",skipped||finished?"":(end?end:"unknown"));
    host_texture_upscale_continued_phase(0);
    atomic_fetch_sub(&urgent,1);
    return 1;
}

void host_texture_upscale_progress_units(unsigned long long *done,unsigned long long *total) {
    struct texture_cache_progress p={0};
    if(upscale_enabled&&catalog_ready)p=progress_of(nil);
    if(done)*done=p.ready_units;
    if(total)*total=p.units;
}
int host_texture_upscale_busy(void) { return upscale_enabled&&(atomic_load(&pump_queued)||atomic_load(&scan_pending)); }
/* the continued-processing task expired while the app is away: stop at the texture in hand (it is made again later);
   back in the foreground, resume if nothing else holds the work (a level being played) */
void host_texture_upscale_background_stop(int stop) {
    if(!upscale_enabled)return;
    if(stop){set_stop(YES);long busy=dispatch_group_wait(work_group,dispatch_time(DISPATCH_TIME_NOW,(int64_t)(2*NSEC_PER_SEC)));
        host_logf(HOST_LOG_INFO,"Upscale: stopped in the background%s",busy?", a texture still running":"");}
    else resume_if_allowed();
}

/* ---------- a level's wait and readiness (the loading screen, texture_upscale_map_loaded) */
int host_texture_upscale_level_begin(const char *name) {
    if(!upscale_enabled||!name)return 0;
    atomic_fetch_add(&urgent,1);
    atomic_store(&level_skipped,false);
    set_priority(name);
    @synchronized(activity_token){level_name=priority_name;texture_upscale_activity_map_will_load(&activity);}
    atomic_store(&level_serial,atomic_load(&priority_serial));
    atomic_store(&level_pumps,atomic_load(&pumps_ended));
    set_stop(NO);schedule_pump();boost();
    return 1;
}
int host_texture_upscale_level_poll(unsigned long long *done,unsigned long long *total) {
    if(done)*done=0;
    if(total)*total=0;
    if(!upscale_enabled)return TEXTURE_UPSCALE_LEVEL_NONE;
    BOOL stopped=atomic_load(&pumps_ended)!=atomic_load(&level_pumps)&&!atomic_load(&pump_queued);
    NSString *name;@synchronized(activity_token){name=level_name;}
    NSData *keys;@synchronized(map_keys){keys=name?map_keys[name]:nil;}
    if(!keys)return stopped||(catalog_ready&&atomic_load(&level_walked_serial)==atomic_load(&level_serial))?TEXTURE_UPSCALE_LEVEL_DONE:TEXTURE_UPSCALE_LEVEL_CHECKING;
    struct texture_cache_progress p=progress_of(keys);
    if(done)*done=p.ready_units;
    if(total)*total=p.units;
    if(p.ready==p.textures)return TEXTURE_UPSCALE_LEVEL_NONE;
    if(stopped||atomic_load(&level_walked_serial)==atomic_load(&level_serial))return TEXTURE_UPSCALE_LEVEL_DONE;
    return TEXTURE_UPSCALE_LEVEL_WORKING;
}
void host_texture_upscale_level_end(int skipped) {
    if(!upscale_enabled)return;
    if(skipped)atomic_store(&level_skipped,true);
    atomic_fetch_sub(&urgent,1);
}
/* every record the manifest lists for the level's keys is on disk at its size (the manifest alone could outlive
   files, as after a delete): the reader would otherwise draw the missing ones original among upscaled ones */
static BOOL level_files_present(NSData *keys) {
    const struct texture_cache_key *k=keys.bytes;size_t n=keys.length/sizeof(*k);
    @synchronized(state_token){
        for(size_t i=0;i<n;i++)for(size_t j=0;j<manifest_count;j++){
            const struct texture_cache_record *r=&manifest[j];
            if(r->hash!=k[i].hash||r->width!=k[i].width||r->height!=k[i].height)continue;
            if(r->bytes){struct stat st;NSString *path=[cache_directory() stringByAppendingPathComponent:[key(r->hash,r->width,r->height) stringByAppendingString:@".bin"]];
                if(stat(path.fileSystemRepresentation,&st)||(uint64_t)st.st_size!=r->bytes){host_logf(HOST_LOG_WARN,"Upscale: %s is listed but missing",path.lastPathComponent.UTF8String);return NO;}}
            break;
        }
    }
    return YES;
}
/* the no-mix rule: 1 when every texture the level's policy upscales has a current entry, else the level plays
   original (and a skipped wait always does); 1 when the generator is off, which leaves the reader as it was */
static int map_path_collect(void *context,const char *path) { [(__bridge NSMutableArray *)context addObject:@(path)];return 1; }
/* The no-mix rule with the generator off (a player who turned it off after a partial run, a measured run): the
   decisions the generator saved, the manifest on disk and the level's own keys. No cache: 1, the reader draws none.
   A cache without its decisions: 0, the level plays original. */
static int level_ready_without_generator(void) {
    if(!any_root||!loading_name[0])return 1;
    NSString *directory=cache_folder_for(any_root);
    struct texture_cache_record *records=NULL;size_t record_count=0;
    if(texture_cache_manifest_load(directory.fileSystemRepresentation,&records,&record_count)){
        host_logf(HOST_LOG_WARN,"Upscale level %s: original textures (the cache's manifest cannot be read)",loading_name);return 0;}
    if(!record_count){free(records);return 1;}
    struct texture_policy_decision *saved=NULL;size_t saved_count=0;
    if(texture_cache_decisions_load(directory.fileSystemRepresentation,&saved,&saved_count)){
        free(records);host_logf(HOST_LOG_INFO,"Upscale level %s: original textures (no decisions beside the cache)",loading_name);return 0;}
    NSMutableArray *paths=[NSMutableArray new];NSString *path=nil,*name=@(loading_name);
    host_texture_source_scan(any_root.fileSystemRepresentation,map_path_collect,(__bridge void *)paths);
    for(NSString *candidate in paths)if([map_base(candidate) isEqual:name]){path=candidate;break;}
    NSData *keys=path?classify_map_keys(path,name):nil;
    int ready=0;
    if(keys){
        if(!record_format){id<MTLDevice> device=MTLCreateSystemDefaultDevice();record_format=TEXTURE_RECORD_ASTC4X4;
            if(@available(iOS 16.4,tvOS 16.4,macOS 11.0,*))if(device.supportsBCTextureCompression)record_format=TEXTURE_RECORD_BC7;}
        struct texture_recipe_globals globals=recipe_globals();struct texture_cache_progress p={0};
        texture_cache_progress(keys.bytes,keys.length/sizeof(struct texture_cache_key),saved,saved_count,records,record_count,&globals,&p);
        ready=p.ready==p.textures;
        const struct texture_cache_key *k=keys.bytes;
        for(size_t i=0;ready&&i<keys.length/sizeof(*k);i++)for(size_t j=0;j<record_count;j++){
            const struct texture_cache_record *r=&records[j];
            if(r->hash!=k[i].hash||r->width!=k[i].width||r->height!=k[i].height)continue;
            struct stat st;NSString *file=[directory stringByAppendingPathComponent:[key(r->hash,r->width,r->height) stringByAppendingString:@".bin"]];
            if(r->bytes&&(stat(file.fileSystemRepresentation,&st)||(uint64_t)st.st_size!=r->bytes))ready=0;
            break;
        }
        host_logf(HOST_LOG_INFO,"Upscale level %s (generator off): %s textures (%zu of %zu ready)",loading_name,ready?"upscaled":"original",p.ready,p.textures);
    } else host_logf(HOST_LOG_INFO,"Upscale level %s (generator off): original textures (its map is not listed)",loading_name);
    free(records);free(saved);
    return ready;
}
int host_texture_upscale_level_ready(void) {
    if(!upscale_enabled)return level_ready_without_generator();
    NSString *name=priority_snapshot();
    if(atomic_exchange(&level_skipped,false)){host_logf(HOST_LOG_INFO,"Upscale level %s: original textures (the wait was skipped)",name.UTF8String);return 0;}
    NSData *keys=keys_for_map(name);
    if(!keys){host_logf(HOST_LOG_INFO,"Upscale level %s: original textures (its textures are not listed yet)",name.UTF8String);return 0;}
    struct texture_cache_progress p=progress_of(keys);
    BOOL ready=p.ready==p.textures&&level_files_present(keys);
    host_logf(HOST_LOG_INFO,"Upscale level %s: %s textures (%zu of %zu ready)",name.UTF8String,ready?"upscaled":"original",p.ready,p.textures);
    return ready;
}
/* the Settings app's line (host_texture_settings.m): empty when the switch is not this module's to explain */
void host_texture_upscale_status(char *text,size_t size) {
    if(!size)return;
    text[0]=0;
    if(upscale_mode==TEXTURE_UPSCALE_MODE_AUTO&&!profile_upscales){snprintf(text,size,"Not used on this device");return;}
    if(!upscale_enabled||!catalog_ready)return;
    struct texture_cache_progress p=progress_of(nil);
    snprintf(text,size,"%zu of %zu ready",p.ready,p.textures);
}
/* the cache folder for the game's reader (guest_host.h) and the Settings app's pane */
void host_texture_cache_directory(char *text,unsigned long size) {
    if(!size)return;
    text[0]=0;
    if(!any_root)return;
    NSString *folder=cache_folder_for(any_root);
    [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSURL fileURLWithPath:folder] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    snprintf(text,size,"%s",folder.fileSystemRepresentation);
}
void host_texture_cache_path(const char *root,char *text,size_t size) {
    if(!size)return;
    snprintf(text,size,"%s",root?cache_folder_for(@(root)).fileSystemRepresentation:"");
}
