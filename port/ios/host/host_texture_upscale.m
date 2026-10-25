/* Apache-2.0. One debug texture at a time; the manifest is the durable progress record. */
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
#include <stdatomic.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>

static dispatch_queue_t upscale_worker;
static NSString *upscale_root;
static BOOL upscale_enabled, export_enabled;
static atomic_bool upscale_playing, scan_pending, pump_queued, thermal_blocked, pump_requested;
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
static BOOL catalog_ready;
static NSString *priority_name,*sorting_priority;
static atomic_uint priority_serial;

static NSString *key(uint64_t hash, uint32_t width, uint32_t height) {
    return [NSString stringWithFormat:@"%016llx-%ux%u",(unsigned long long)hash,width,height];
}
static double now(void) { return NSProcessInfo.processInfo.systemUptime; }
static uint64_t footprint(void) {
    task_vm_info_data_t info={0};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&count)!=KERN_SUCCESS)return 0;
    peak_footprint=MAX(peak_footprint,(uint64_t)info.ledger_phys_footprint_peak);
    return MAX((uint64_t)info.phys_footprint,(uint64_t)info.ledger_phys_footprint_peak);
}
static NSString *cache_directory(void) { return [upscale_root stringByAppendingPathComponent:@TEXTURE_CACHE_DIRECTORY]; }
static const char *treatment_name(enum texture_policy_treatment value) {
    static const char *names[]={"skip","color","first-person","emissive","multipurpose","bump"};
    return value<=TEXTURE_POLICY_BUMP_MAP?names[value]:"skip";
}
static int bool_setting(const char *setting,const char *environment) {
    const char *override=getenv(environment);char value[32];host_config_string(setting,"false",value,sizeof(value));
    return !strcmp(override?override:value,"true")||!strcmp(override?override:value,"1");
}
static void save_log(void) {
    peak_footprint=MAX(peak_footprint,footprint());
    [thermal_log addObject:@{@"seconds":@(now()-started),@"state":@(NSProcessInfo.processInfo.thermalState)}];
    NSMutableDictionary *stage_totals=[NSMutableDictionary new];uint64_t ready=0,excluded=0;
    for(NSDictionary *row in texture_log.allValues){
        if([row[@"result_status"] isEqual:@"complete"])ready++;
        if([row[@"result_status"] isEqual:@"compression-excluded"])excluded++;
        for(NSString *stage in row[@"stage_seconds"])stage_totals[stage]=@([stage_totals[stage] doubleValue]+[row[@"stage_seconds"][stage] doubleValue]);
    }
    NSDictionary *contents=@{@"textures":texture_log.allValues,@"thermal":thermal_log,
        @"peak_footprint":@(peak_footprint),@"totals":@{@"textures":@(texture_log.count),@"catalog":@(decision_count),@"seconds":@(now()-started),@"ready":@(ready),@"excluded":@(excluded),@"stage_seconds":stage_totals}};
    NSData *data=[NSJSONSerialization dataWithJSONObject:contents options:NSJSONWritingPrettyPrinted error:nil];
    [data writeToFile:[upscale_root stringByAppendingPathComponent:@"texture-upscale-generate.json"] options:NSDataWritingAtomic error:nil];
}
static void export_table(void) {
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
static int collect_map(void *context,const char *path) { (void)context;[map_paths addObject:@(path)];return 1; }
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
        if(!map){int reason=errno;host_logf(HOST_LOG_WARN,"Upscale catalog: %s",error);if(reason==ENOTSUP)continue;ok=NO;break;}
        const unsigned char *mapping=mmap(NULL,(size_t)map_size,PROT_READ,MAP_PRIVATE,fileno(map),0);
        int classified=mapping!=MAP_FAILED?texture_policy_add_map(catalog,mapping,(size_t)map_size,error,sizeof(error)):-1;
        if(mapping!=MAP_FAILED)munmap((void *)mapping,(size_t)map_size);fclose(map);
        if(classified){host_logf(HOST_LOG_WARN,"Upscale catalog: %s",error);ok=NO;break;}
        [classified_maps addObject:path];
    }
    if(ok){map_paths=classified_maps;free(decisions);decisions=NULL;decision_count=texture_policy_classify(catalog,&texture_policy_embedded,&decisions,NULL,NULL);
        ok=decisions!=NULL;
        for(size_t i=0;ok&&i<decision_count;i++)decision_slots[key(decisions[i].hash,decisions[i].width,decisions[i].height)]=@(i);
    }
    texture_policy_catalog_free(catalog);return ok;
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
static void log_texture_error(const struct texture_policy_decision *d,const char *stage,const char *message,double elapsed) {
    texture_log[key(d->hash,d->width,d->height)]=@{@"hash":[NSString stringWithFormat:@"%016llx",(unsigned long long)d->hash],
        @"width":@(d->width),@"height":@(d->height),@"kind":@(texture_policy_kind_name(d->kind)),
        @"treatment":@(treatment_name(d->treatment)),@"result":@(texture_policy_result_name(d->result)),@"scale":@(d->scale),
        @"result_status":@"error",@"failed_stage":@(stage),@"error":@(message),@"stage_seconds":@{@"total":@(elapsed)},
        @"damage_share":@(-1),@"structure_loss":@(-1)};
    save_log();export_table();
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
    struct texture_recipe_globals globals={.model_id="S4g: PBRify SPAN V4 per channel",
        .model_sha256="d1f1fa1af7a5ab23fcf143e75aac4951e6c52d56bf77d9ccd4d004a4831d0461",.pipeline_version={1,1,1,1,1,1},
        .low=policy->low,.high=policy->high,.sigma=policy->sigma,.iterations=(uint32_t)policy->iterations,.post_version=1,
        .encoder=texture_upscale_encoder_name(record_format),.damage_share=policy->damage_share,.damage_structure_loss=policy->damage_structure_loss,
        .damage_bpf_structure_loss=policy->damage_bpf_structure_loss,.damage_measure_version=(uint32_t)policy->damage_measure_version};
    struct texture_recipe recipe={d->hash,d->width,d->height,d->result,d->treatment,d->wrap,dilate,alpha,d->scale};
    struct texture_cache_record desired={d->hash,d->width,d->height,d->result,d->treatment,
        texture_recipe_key(&globals,&recipe),texture_recipe_global_key(&globals,d->treatment,d->result),0};
    struct texture_cache_record *previous=find_record(d);
    /* An export redoes a cached texture once per launch, not on every pass a thermal or foreground change starts. */
    BOOL exported=export_enabled&&texture_log[key(d->hash,d->width,d->height)]!=nil;
    if(previous&&previous->recipe_key==desired.recipe_key&&previous->global_key==desired.global_key&&valid_record(previous)&&(!export_enabled||exported))return 0;
    uint64_t top_width=(uint64_t)d->width*d->scale,top_height=(uint64_t)d->height*d->scale;
    unsigned channels=alpha!=TEXTURE_UPSCALE_ALPHA_NONE||d->treatment==TEXTURE_POLICY_MULTIPURPOSE?4:3;
    if((uint64_t)d->width*d->height>(1024ull*1024)||top_width>TEXTURE_RECORD_MAXIMUM_DIMENSION||top_height>TEXTURE_RECORD_MAXIMUM_DIMENSION||
        top_width*top_height*channels*sizeof(float)>(256ull<<20)) {
        desired.result=TEXTURE_POLICY_ORIGINAL;context->failed=!publish_record(&desired);
        texture_log[key(d->hash,d->width,d->height)]=@{@"hash":[NSString stringWithFormat:@"%016llx",(unsigned long long)d->hash],
            @"width":@(d->width),@"height":@(d->height),@"kind":@(texture_policy_kind_name(d->kind)),@"result":@"original",@"scale":@1,
            @"treatment":@(treatment_name(d->treatment)),@"result_status":@"working-set-excluded",@"damage_share":@(-1),@"structure_loss":@(-1)};
        host_logf(HOST_LOG_WARN,"Upscale %016llx: output exceeds the bounded working set",(unsigned long long)d->hash);
        save_log();export_table();return 0;
    }
    uint64_t projected=(uint64_t)d->width*d->height*d->scale*d->scale*2+4096;
    struct statvfs disk;
    if(statvfs(upscale_root.fileSystemRepresentation,&disk)||!texture_cache_background_may_run((uint64_t)disk.f_bavail*disk.f_frsize,projected)){
        host_logf(HOST_LOG_WARN,"Upscale: insufficient storage for %016llx",(unsigned long long)d->hash);
        log_texture_error(d,"storage","insufficient free storage",0);context->failed=YES;return 0;
    }
    struct texture_upscale_rgba_level levels[13];uint32_t width=d->width,height=d->height;
    for(unsigned i=0;i<source->level_count;i++){levels[i]=(struct texture_upscale_rgba_level){source->rgba+source->level_offsets[i],width,height};width=MAX(1u,width/2);height=MAX(1u,height/2);}
    struct texture_upscale_generate_input input={*d,policy,levels,source->level_count,alpha,d->wrap,dilate,d->pinned};
    struct texture_upscale_generate_output output={0};char error[512]={0};double start=now();
    if(!generator)generator=texture_upscale_generate_create(error,sizeof(error));
    if(!generator||texture_upscale_generate(generator,&input,&output,error,sizeof(error))){
        peak_footprint=MAX(peak_footprint,footprint());log_texture_error(d,"generate",error,now()-start);
        host_logf(HOST_LOG_WARN,"Upscale %016llx: %s",(unsigned long long)d->hash,error);context->failed=YES;return 0;
    }
    struct texture_upscale_encode_metrics metrics={0};void *bytes=NULL;size_t size=0;double encode_start=now();
    enum texture_upscale_encode_result encoded=TEXTURE_UPSCALE_ENCODE_OK;
    if(output.result!=TEXTURE_POLICY_ORIGINAL)encoded=texture_upscale_encode_record(output.levels,output.level_count,d->width,d->height,d->hash,
        desired.recipe_key,record_format,(unsigned)alpha,NSProcessInfo.processInfo.thermalState>=NSProcessInfoThermalStateSerious?2:4,&bytes,&size,&metrics);
    double encode_seconds=now()-encode_start;BOOL excluded=encoded==TEXTURE_UPSCALE_ENCODE_EXCLUDED;
    if(encoded==TEXTURE_UPSCALE_ENCODE_ERROR){peak_footprint=MAX(peak_footprint,footprint());log_texture_error(d,"encode","encoder failed",now()-start);context->failed=YES;texture_upscale_generate_free_output(&output);return 0;}
    desired.result=excluded?TEXTURE_POLICY_ORIGINAL:output.result;desired.bytes=size;
    double write_start=now();
    if(size&&texture_cache_write_entry(cache_directory().fileSystemRepresentation,&desired,bytes,size))context->failed=YES;
    if(!context->failed&&!publish_record(&desired))context->failed=YES;
    if(export_enabled&&output.level_count){
        NSString *folder=[upscale_root stringByAppendingPathComponent:@"texture-upscale-export"];
        [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
        uint32_t header[3]={output.levels[0].width,output.levels[0].height,1};
        NSString *path=[folder stringByAppendingPathComponent:[NSString stringWithFormat:@"%016llx.rgba",(unsigned long long)d->hash]];
        NSString *temporary=[path stringByAppendingString:@".tmp"];FILE *file=fopen(temporary.fileSystemRepresentation,"wb");
        if(file){BOOL ok=fwrite(header,1,sizeof(header),file)==sizeof(header)&&fwrite(output.levels[0].pixels,4,(size_t)header[0]*header[1],file)==(size_t)header[0]*header[1];
            ok=fclose(file)==0&&ok;if(ok)rename(temporary.fileSystemRepresentation,path.fileSystemRepresentation);else unlink(temporary.fileSystemRepresentation);}

    }
    struct texture_upscale_model_stats stats={0};texture_upscale_generate_stats(generator,&stats);peak_footprint=MAX(peak_footprint,stats.peak_footprint);
    texture_log[key(d->hash,d->width,d->height)]=@{@"hash":[NSString stringWithFormat:@"%016llx",(unsigned long long)d->hash],@"width":@(d->width),@"height":@(d->height),
        @"kind":@(texture_policy_kind_name(d->kind)),@"result":@(texture_policy_result_name(desired.result)),@"treatment":@(treatment_name(d->treatment)),
        @"scale":@(desired.result==TEXTURE_POLICY_ORIGINAL?1:output.scale),@"format":metrics.raw?@"bgra8":record_format==TEXTURE_RECORD_BC7?@"bc7":@"astc4x4",@"alpha_psnr":@(isfinite(metrics.alpha_psnr)?metrics.alpha_psnr:999),
        @"stage_seconds":@{@"predict":@(output.seconds.predict),@"refine":@(output.seconds.refine),@"damage":@(output.seconds.damage),@"bump":@(output.seconds.bump),@"mips":@(output.seconds.mips),@"encode":@(encode_seconds),@"write":@(now()-write_start),@"total":@(now()-start)},
        @"psnr":@(isfinite(metrics.rgb_psnr)?metrics.rgb_psnr:999),@"binary_alpha_max_error":@(metrics.binary_alpha_max_error),@"psnr_mask":@"alpha>0",
        @"compression_gate":@(!excluded),@"encoder_attempts":@(metrics.attempts),@"encoder_preset":@(metrics.preset),@"result_status":context->failed?@"write-error":excluded?@"compression-excluded":metrics.raw?@"complete-raw":@"complete",
        @"damage_share":@(output.damage_share),@"structure_loss":@(output.structure_loss_s4g),
        @"shape":@{@"width":@(stats.input_width),@"height":@(stats.input_height),@"footprint_before":@(stats.footprint_before),@"footprint_after":@(stats.footprint_after)}};
    host_logf(HOST_LOG_INFO,"Upscale %016llx: %s, damage %.4f in %.3fs, compression %.2f dB",(unsigned long long)d->hash,
        context->failed?"write failed":excluded?"kept original":"complete",output.damage_share,output.seconds.damage,metrics.rgb_psnr);
    free(bytes);texture_upscale_generate_free_output(&output);
    if(export_enabled&&input.wrap&&!context->failed) {
        double seam_start=now();NSArray *scores=half_offset_scores(&input,error,sizeof(error));
        NSMutableDictionary *row=[texture_log[key(d->hash,d->width,d->height)] mutableCopy];
        if(scores){row[@"half_offset_psnr"]=scores;row[@"half_offset_stage"]=@"raw4x";}
        else {row[@"seam_error"]=@(error[0]?error:"half-offset prediction failed");host_logf(HOST_LOG_WARN,"Upscale seam %016llx: %s",(unsigned long long)d->hash,error);}
        NSMutableDictionary *times=[row[@"stage_seconds"] mutableCopy];times[@"seam"]=@(now()-seam_start);row[@"stage_seconds"]=times;
        texture_log[key(d->hash,d->width,d->height)]=row;
    }
    save_log();export_table();return 0;
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
static void pump(void) {
    @autoreleasepool {
        NSMutableArray<NSString *> *ordered=nil;
        atomic_store(&pump_requested,false);
        if(atomic_load(&upscale_playing)||!thermal_may_run())goto done;
        if(atomic_exchange(&scan_pending,false))catalog_ready=NO;
        if(!catalog_ready){catalog_ready=build_catalog();if(!catalog_ready)goto done;if(!reconcile_manifest()){catalog_ready=NO;goto done;}}
        unsigned serial=atomic_load(&priority_serial);
        @synchronized(upscale_root){sorting_priority=priority_name;}
        ordered=[map_paths mutableCopy];pass_hashes=[NSMutableSet new];
        if(sorting_priority.length) {
            for(NSString *path in map_paths)if([path.lastPathComponent.stringByDeletingPathExtension.lowercaseString isEqual:sorting_priority]) {
                [ordered removeObject:path];[ordered insertObject:path atIndex:0];break;
            }
        }
        /* Every prediction uses the same 288x288 shape, so walking each map
           once preserves shape grouping without inflating it for each texture. */
        for(NSString *path in ordered){@autoreleasepool {
            if(serial!=atomic_load(&priority_serial)||atomic_load(&scan_pending)||atomic_load(&upscale_playing)||!thermal_may_run())break;
            struct map_generation_context context={serial,NO};
            int result=host_texture_source_walk_map(path.fileSystemRepresentation,generate_map_source,&context);
            if(result<0)host_logf(HOST_LOG_WARN,"Upscale: malformed source map");
            if(result!=1||context.failed)break;
        }}
        pass_hashes=nil;
        export_table();save_log();
    done:
        atomic_store(&pump_queued,false);
        if((atomic_load(&scan_pending)||atomic_load(&pump_requested))&&!atomic_load(&upscale_playing)&&thermal_may_run())schedule_pump();
    }
}
static void schedule_pump(void) {
    if(!upscale_enabled)return;
    atomic_store(&pump_requested,true);
    if(!atomic_exchange(&pump_queued,true))dispatch_async(upscale_worker,^{pump();});
}
void host_texture_upscale_scan(void) {if(upscale_enabled){atomic_store(&scan_pending,true);schedule_pump();}}
void host_texture_upscale_init(const char *root) {
    static dispatch_once_t once;dispatch_once(&once,^{
        /* Development only: HALO_TEXTURE_UPSCALE_CLEAN removes what the generator and its export left in the data
           root, since a device's app container cannot be emptied from the Mac. */
        if(root&&getenv("HALO_TEXTURE_UPSCALE_CLEAN")) {
            NSString *base=@(root);NSFileManager *files=NSFileManager.defaultManager;
            if(texture_cache_delete_all([base stringByAppendingPathComponent:@TEXTURE_CACHE_DIRECTORY].fileSystemRepresentation))
                host_logf(HOST_LOG_WARN,"Upscale: could not delete the whole texture cache");
            [files removeItemAtPath:[base stringByAppendingPathComponent:@"texture-upscale-export"] error:nil];
            [files removeItemAtPath:[base stringByAppendingPathComponent:@"texture-upscale-generate.json"] error:nil];
            host_logf(HOST_LOG_INFO,"Upscale: removed the texture cache, the export and the generator log");
            return;
        }
        if(!root||!bool_setting("debug.texture_upscale_generate","HALO_TEXTURE_UPSCALE_GENERATE")||!texture_upscale_model_available())return;
        upscale_enabled=YES;export_enabled=bool_setting("debug.texture_upscale_export","HALO_TEXTURE_UPSCALE_EXPORT");upscale_root=@(root);started=now();
        upscale_worker=dispatch_queue_create("org.visr.texture-upscale",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0));
        texture_log=[NSMutableDictionary new];thermal_log=[NSMutableArray new];
        [NSFileManager.defaultManager createDirectoryAtPath:cache_directory() withIntermediateDirectories:YES attributes:nil error:nil];
        [[NSURL fileURLWithPath:cache_directory()] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        texture_cache_manifest_load(cache_directory().fileSystemRepresentation,&manifest,&manifest_count);
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();record_format=TEXTURE_RECORD_ASTC4X4;
        if(@available(iOS 16.4,tvOS 16.4,macOS 11.0,*))if(device.supportsBCTextureCompression)record_format=TEXTURE_RECORD_BC7;
        [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;if(NSProcessInfo.processInfo.thermalState<=NSProcessInfoThermalStateFair)schedule_pump();}];
        #if TARGET_OS_OSX
        NSString *active_notification=NSApplicationWillBecomeActiveNotification;
#else
        NSString *active_notification=UIApplicationWillEnterForegroundNotification;
#endif
        [NSNotificationCenter.defaultCenter addObserverForName:active_notification object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;host_texture_upscale_scan();}];
        host_texture_upscale_scan();
    });
}
void host_texture_upscale_map_will_load(const char *name) {
    if(!upscale_enabled)return;
    if(name){NSString *normalized=[@(name) stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
        @synchronized(upscale_root){priority_name=normalized.lastPathComponent.stringByDeletingPathExtension.lowercaseString;}
        atomic_fetch_add(&priority_serial,1);}
    atomic_store(&upscale_playing,false);schedule_pump();
}
void host_texture_upscale_game_started(int is_game) {
    atomic_store(&upscale_playing,is_game!=0);if(!is_game)schedule_pump();
}
