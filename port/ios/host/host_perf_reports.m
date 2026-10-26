/* Apache-2.0. All disk, JSON and compression work stays on the utility worker. */
#import "host_perf_reports.h"
#import "host_shader_upload.h"
#import "host_benchmark_outbox.h"
#import "host_report_delivery.h"
#import "host_perf_validation.h"
#import "host_benchmark.h"
#import "host_profile.h"
#import "host_perf_hooks.h"
#import "host_map_identity.h"
#import "host_perf_metal.h"
#include "../../runtime/game_perf_graphics.h"
#include "../../linux/include/halo_network_version.h"
#import <CommonCrypto/CommonDigest.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#import <TargetConditionals.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>
#include <math.h>
#if HALO_NATIVE_MAC
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#define ROOT @"perf-reports"
#define OUTBOX @"perf-reports/outbox"
#define JOURNAL_LIMIT (32u*1024u*1024u)
#define ASSET_LIMIT (4u*1024u*1024u)
#define MAX_TRACE_ROWS (JOURNAL_LIMIT/(sizeof(game_perf_frame)+20u))
#if HALO_PERF_REPORTS
static dispatch_queue_t worker;
static char worker_key;
static dispatch_source_t timer;
static game_perf_recorder *recorder;
static atomic_bool enabled, collecting, menu, suspended, runtime_suppressed;
static atomic_uint appenders;
static BOOL resume_play, resume_ready;
static uint64_t start_us;
static NSString *guest,*active_map;
static NSMutableDictionary *metadata,*delivery;
static NSDictionary *graphics,*initial_context;
static NSString *graphics_hash;
static atomic_bool abnormal,load_begin_logged;
static struct {struct game_perf_graphics_snapshot value;uint64_t frame_id;} graphics_queue[2];
static uint64_t graphics_frame_override;
static atomic_uint graphics_head,graphics_tail,graphics_dropped;
static void drain_graphics(void);
static void thermal_event(void);
static FILE *journal;
static uint32_t sequence;
static uint64_t journal_bytes, rows, duration_us, lost_rows, first_frame_id, omitted_begin_us, omitted_end_us, lost_events, journal_events;
static NSTimeInterval last_sync;
static BOOL truncated;
static uint64_t trace_limit=MAX_TRACE_ROWS, frozen_frames, frozen_skipped;
static uint64_t retained_frame_ids[MAX_TRACE_ROWS];
static BOOL omit_detail;
static uint64_t clock_us(void) {struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000000+(uint64_t)now.tv_nsec/1000;}
static BOOL persist(NSDictionary *value,NSString *path) {
    NSData *data=[NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingSortedKeys error:nil];
    if(![data writeToFile:path options:NSDataWritingAtomic error:nil])return NO;
    FILE *f=fopen(path.fileSystemRepresentation,"rb");BOOL ok=f && fsync(fileno(f))==0;if(f)fclose(f);int folder=open(path.stringByDeletingLastPathComponent.fileSystemRepresentation,O_RDONLY);if(folder<0)return NO;ok=ok && fsync(folder)==0;close(folder);return ok;
}
static game_perf_summary recovered_summary;
static game_perf_status recovered_status;
static BOOL recovered_summary_valid, recovered_windows_active;
static uint64_t recovered_frame_id;
static BOOL rejected_tail;
static id bounded_json(NSString *path,NSUInteger limit) {
    NSNumber *size=[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil][NSFileSize];
    if(!size || size.unsignedLongLongValue>limit)return nil;
    NSData *data=[NSData dataWithContentsOfFile:path];
    return data ? [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil] : nil;
}
static BOOL working_metadata_valid(NSDictionary *value) {
    if(![value isKindOfClass:NSDictionary.class] || ![value[@"schema"] isEqual:@1] ||
       ![value[@"journalABI"] isEqual:@1] || ![value[@"journalFrameSize"] isEqual:@(sizeof(game_perf_frame))])return NO;
    NSArray *allowed=@[@"schema",@"nonce",@"model",@"platform",@"profileID",@"mapFingerprint",@"mapFormat",@"renderer",@"mode",@"role",@"playerCount",@"localPlayerCount",@"width",@"height",@"refreshMilliHz",@"output",@"lowPowerMode",@"frameRateTarget",@"appVersion",@"appBuild",@"guestSHA256",@"profileHash",@"graphics",@"os",@"networkVersion",@"journalABI",@"journalFrameSize",@"timingSemantics",@"resumed",@"_checkpoint",@"_initialContext"];
    for(id key in value)if(![allowed containsObject:key])return NO;
    for(NSString *key in @[@"nonce",@"model",@"platform",@"profileID",@"mapFingerprint",@"renderer",@"mode",@"appVersion",@"appBuild",@"guestSHA256"])
        if(![value[key] isKindOfClass:NSString.class] || ![value[key] length] || [value[key] length]>256)return NO;
    NSCharacterSet *nonhex=[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet];
    if([value[@"nonce"] length]!=32 || [value[@"nonce"] rangeOfCharacterFromSet:nonhex].location!=NSNotFound || [value[@"guestSHA256"] length]!=64 || [value[@"guestSHA256"] rangeOfCharacterFromSet:nonhex].location!=NSNotFound)return NO;
    if([value[@"_initialContext"] isKindOfClass:NSDictionary.class] && !working_metadata_valid(value[@"_initialContext"]))return NO;
    return YES;
}
static void discard_orphaned_recording(void) {
    if(![NSFileManager.defaultManager fileExistsAtPath:[ROOT stringByAppendingPathComponent:@"working.bin"]])return;
    delivery[@"lostReports"]=@([delivery[@"lostReports"] unsignedLongLongValue]+1);
    if(!persist(delivery,[ROOT stringByAppendingPathComponent:@"delivery.json"]))return;
    [NSFileManager.defaultManager removeItemAtPath:[ROOT stringByAppendingPathComponent:@"working.bin"] error:nil];
    [NSFileManager.defaultManager removeItemAtPath:[ROOT stringByAppendingPathComponent:@"working.json"] error:nil];
}
/* Internal checkpoints accompany the fsynced CRC journal; they never enter an asset. */
static BOOL checkpoint_state(void) {
    if(!metadata)return NO;
    game_perf_summary summary={0};game_perf_status status={0};
    uint64_t watermark=0;
    if(recorder && !game_perf_checkpoint_snapshot(recorder,&summary,&status,&watermark))return NO;
    NSData *summary_bytes=[NSData dataWithBytes:&summary length:sizeof(summary)];
    NSData *status_bytes=[NSData dataWithBytes:&status length:sizeof(status)];
    NSDictionary *checkpoint=@{@"schema":@1,@"sequence":@(sequence),@"frameID":@(watermark),@"durationUS":@(duration_us),@"rows":@(rows),
        @"lostRows":@(lost_rows),@"lostEvents":@(lost_events),@"omittedBeginUS":@(omitted_begin_us),@"omittedEndUS":@(omitted_end_us),
        @"truncated":@(truncated),@"abnormal":@(atomic_load(&abnormal)),
        @"summary":[summary_bytes base64EncodedStringWithOptions:0],@"summaryCRC":@(game_perf_crc32(summary_bytes.bytes,summary_bytes.length)),
        @"status":[status_bytes base64EncodedStringWithOptions:0],@"statusCRC":@(game_perf_crc32(status_bytes.bytes,status_bytes.length))};
    NSMutableDictionary *saved=[metadata mutableCopy];saved[@"_checkpoint"]=checkpoint;saved[@"_initialContext"]=initial_context?:metadata;
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:saved options:NSJSONWritingSortedKeys error:nil];
    return bytes.length<=65536 && persist(saved,[ROOT stringByAppendingPathComponent:@"working.json"]);
}
static BOOL restore_checkpoint(NSMutableDictionary *saved) {
    id checkpoint=saved[@"_checkpoint"],initial=saved[@"_initialContext"];
    [saved removeObjectForKey:@"_checkpoint"];[saved removeObjectForKey:@"_initialContext"];
    initial_context=[initial isKindOfClass:NSDictionary.class]?[initial copy]:[saved copy];
    recovered_summary_valid=NO;
    if(![checkpoint isKindOfClass:NSDictionary.class] || ![checkpoint[@"schema"] isEqual:@1] ||
       ![checkpoint[@"summary"] isKindOfClass:NSString.class] || ![checkpoint[@"status"] isKindOfClass:NSString.class])return NO;
    for(NSString *key in @[@"sequence",@"frameID",@"durationUS",@"rows",@"lostRows",@"lostEvents",@"omittedBeginUS",@"omittedEndUS",@"summaryCRC",@"statusCRC",@"truncated",@"abnormal"])
        if(![checkpoint[key] isKindOfClass:NSNumber.class])return NO;
    if([checkpoint[@"sequence"] unsignedLongLongValue]>UINT32_MAX)return NO;
    NSData *summary=[[NSData alloc] initWithBase64EncodedString:checkpoint[@"summary"] options:0];
    NSData *status=[[NSData alloc] initWithBase64EncodedString:checkpoint[@"status"] options:0];
    if(summary.length!=sizeof(recovered_summary) || status.length!=sizeof(recovered_status) ||
       game_perf_crc32(summary.bytes,summary.length)!=[checkpoint[@"summaryCRC"] unsignedIntValue] ||
       game_perf_crc32(status.bytes,status.length)!=[checkpoint[@"statusCRC"] unsignedIntValue])return NO;
    memcpy(&recovered_summary,summary.bytes,sizeof(recovered_summary));memcpy(&recovered_status,status.bytes,sizeof(recovered_status));
    recovered_frame_id=[checkpoint[@"frameID"] unsignedLongLongValue];duration_us=[checkpoint[@"durationUS"] unsignedLongLongValue];
    rows=[checkpoint[@"rows"] unsignedLongLongValue];lost_rows=[checkpoint[@"lostRows"] unsignedLongLongValue];lost_events=[checkpoint[@"lostEvents"] unsignedLongLongValue];
    omitted_begin_us=[checkpoint[@"omittedBeginUS"] unsignedLongLongValue];omitted_end_us=[checkpoint[@"omittedEndUS"] unsignedLongLongValue];
    truncated=[checkpoint[@"truncated"] boolValue];atomic_store(&abnormal,[checkpoint[@"abnormal"] boolValue]);
    recovered_summary_valid=YES;return YES;
}
static uint64_t checkpoint_add(uint64_t a,uint64_t b) {return a>UINT64_MAX-b?UINT64_MAX:a+b;}
static void merge_summary(game_perf_summary *to,const game_perf_summary *tail) {
    for(unsigned g=0;g<2;g++) {
        to->frames[g]=checkpoint_add(to->frames[g],tail->frames[g]);to->over_budget[g]=checkpoint_add(to->over_budget[g],tail->over_budget[g]);to->budget_available[g]=checkpoint_add(to->budget_available[g],tail->budget_available[g]);
        for(unsigned m=0;m<GAME_PERF_METRIC_COUNT;m++){game_perf_metric_summary *a=&to->metrics[g][m];const game_perf_metric_summary *b=&tail->metrics[g][m];
            a->available=checkpoint_add(a->available,b->available);a->sum_us=checkpoint_add(a->sum_us,b->sum_us);a->max_us=MAX(a->max_us,b->max_us);
            for(unsigned bin=0;bin<GAME_PERF_HISTOGRAM_BINS;bin++)a->histogram[bin]=checkpoint_add(a->histogram[bin],b->histogram[bin]);
        }
    }
}

static void append_chunk(uint32_t type,const void *value,size_t size) {
    uint8_t payload[16384], chunk[sizeof(payload)+16];
    if(type!=1 && journal_events>=4096){lost_events++;return;}
    if(size>sizeof(payload)-4){truncated=YES;return;}
    memcpy(payload,&type,4);memcpy(payload+4,value,size);
    size_t n=game_perf_journal_chunk(chunk,sizeof(chunk),sequence,payload,size+4);
    if(!journal || journal_bytes+n>JOURNAL_LIMIT || fwrite(chunk,1,n,journal)!=n){truncated=YES;if(type==1){const game_perf_frame *frame=value;lost_rows++;if(!omitted_begin_us)omitted_begin_us=frame->elapsed_us;omitted_end_us=frame->elapsed_us;}else lost_events++;return;}
    sequence++;journal_bytes+=n;if(type!=1)journal_events++;
}
static void enrich_frame(void *unused,game_perf_frame *frame) {(void)unused;host_perf_hooks_gpu_join(frame);host_perf_metal_join(frame);}
static void frame_row(void *unused,const game_perf_frame *frame) {(void)unused;if(!rows)first_frame_id=frame->frame_id;if(rows<trace_limit)append_chunk(1,frame,sizeof(*frame));else {truncated=YES;lost_rows++;if(!omitted_begin_us)omitted_begin_us=frame->elapsed_us;omitted_end_us=frame->elapsed_us;}rows++;duration_us=MAX(duration_us,frame->elapsed_us);}
static void apply_context_event_to(NSMutableDictionary *context,const game_perf_event *event) {
    if(event->type==GAME_PERF_EVENT_CONTEXT){
        NSArray *keys=@[@"",@"renderer",@"mode",@"role",@"playerCount",@"localPlayerCount",@"width",@"height",@"refreshMilliHz",@"output",@"lowPowerMode",@"frameRateTarget"];
        if(event->flags<keys.count && event->flags){id value=@(event->value);
            NSArray *labels=event->flags==1?@[@"unknown",@"metal",@"gles"]:event->flags==2?@[@"unknown",@"campaign",@"multiplayer"]:event->flags==3?@[@"unknown",@"local",@"host",@"client"]:event->flags==9?@[@"unknown",@"flat",@"immersive",@"external"]:nil;
            if(labels)value=event->value<labels.count?labels[event->value]:@"unknown";if(event->flags==10)value=@(event->value!=0);context[keys[event->flags]]=value;
        }
    }
}
static void event_row(void *unused,const game_perf_event *event) {
    (void)unused;append_chunk(2,event,sizeof(*event));apply_context_event_to(metadata,event);
}
static void resume_segment(void) {
    if(!resume_play || !resume_ready || atomic_load(&suspended) || atomic_load(&appenders) || [NSFileManager.defaultManager fileExistsAtPath:[ROOT stringByAppendingPathComponent:@"working.bin"]])return;
    resume_play=NO;resume_ready=NO;atomic_store(&abnormal,false);metadata=[metadata mutableCopy];metadata[@"nonce"]=[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""].lowercaseString;metadata[@"resumed"]=@YES;initial_context=[metadata copy];
    journal_bytes=0;sequence=0;rows=0;first_frame_id=0;duration_us=0;lost_rows=0;lost_events=0;journal_events=0;omitted_begin_us=0;omitted_end_us=0;truncated=NO;trace_limit=MAX_TRACE_ROWS;omit_detail=NO;recovered_summary_valid=NO;recovered_windows_active=NO;start_us=clock_us();
    persist(metadata,[ROOT stringByAppendingPathComponent:@"working.json"]);journal=fopen([[ROOT stringByAppendingPathComponent:@"working.bin"] fileSystemRepresentation],"wb");
    if(journal && game_perf_start(recorder,start_us)){atomic_store(&load_begin_logged,false);thermal_event();atomic_store(&collecting,true);}
}
static void refresh_map_identity(void) {
    if(!journal || !active_map)return;NSDictionary *identity=host_shader_upload_map_identity(active_map.UTF8String);
    if(!identity || [metadata[@"mapFingerprint"] isEqual:identity[@"fingerprint"]])return;
    metadata[@"mapFingerprint"]=identity[@"fingerprint"];metadata[@"mapFormat"]=identity[@"format"];
    NSData *epoch=[NSJSONSerialization dataWithJSONObject:@{@"frame":@(rows),@"context":@{@"mapFingerprint":identity[@"fingerprint"],@"mapFormat":identity[@"format"]}} options:NSJSONWritingSortedKeys error:nil];append_chunk(3,epoch.bytes,epoch.length);
}
static void thermal_event(void) {
    if(!journal)return;game_perf_event event={.elapsed_us=clock_us()>=start_us?clock_us()-start_us:0,.type=GAME_PERF_EVENT_THERMAL,.value=(uint64_t)NSProcessInfo.processInfo.thermalState};append_chunk(2,&event,sizeof(event));
}
static void drain(void) {
    drain_graphics();game_perf_drain(recorder,frame_row,event_row,NULL);refresh_map_identity();
    if(journal && NSDate.date.timeIntervalSince1970-last_sync>=5){if(!fflush(journal) && !fsync(fileno(journal)) && checkpoint_state())last_sync=NSDate.date.timeIntervalSince1970;}
}
/* Strictly bounded scanning salvages only complete CRC-checked chunks. */
static BOOL read_chunk(FILE *input,uint32_t expected,uint32_t *type,uint8_t *payload,size_t capacity,size_t *size) {
    uint8_t chunk[16400];
    size_t header_bytes=fread(chunk,1,16,input);
    if(header_bytes!=16){if(header_bytes || ferror(input))rejected_tail=YES;return NO;}
    uint32_t length=(uint32_t)chunk[4]|(uint32_t)chunk[5]<<8|(uint32_t)chunk[6]<<16|(uint32_t)chunk[7]<<24;
    if(length<4 || length>16384 || length-4>capacity || fread(chunk+16,1,length,input)!=length){rejected_tail=YES;return NO;}
    uint32_t count=0;if(game_perf_journal_recover(chunk,length+16,&count)!=length+16 || count!=expected){rejected_tail=YES;return NO;}
    uint32_t actual=(uint32_t)chunk[8]|(uint32_t)chunk[9]<<8|(uint32_t)chunk[10]<<16|(uint32_t)chunk[11]<<24;
    if(actual!=expected){rejected_tail=YES;return NO;}memcpy(type,chunk+16,4);*size=length-4;memcpy(payload,chunk+20,*size);return YES;
}
static BOOL write_bytes(FILE *out,const void *bytes,size_t count) {return fwrite(bytes,1,count,out)==count;}
static BOOL json_value(FILE *out,id value) {
    NSData *data=[NSJSONSerialization dataWithJSONObject:@[value] options:NSJSONWritingSortedKeys error:nil];
    return data.length>=2 && write_bytes(out,(const uint8_t *)data.bytes+1,data.length-2);
}
/* Frame IDs can contain gaps after bounded queues drop rows. */
static uint64_t retained_index(uint64_t absolute) {
    uint64_t low=0,high=frozen_frames;
    while(low<high){uint64_t middle=low+(high-low)/2;if(retained_frame_ids[middle]<absolute)low=middle+1;else high=middle;}
    return low;
}
typedef struct epoch_index {uint64_t frame;long offset;uint32_t sequence;} epoch_index;
static int epoch_compare(const void *a,const void *b) {
    const epoch_index *left=a,*right=b;if(left->frame!=right->frame)return left->frame<right->frame?-1:1;return (left->sequence>right->sequence)-(left->sequence<right->sequence);
}
static BOOL write_epochs(FILE *out,NSString *working) {
    epoch_index *index=calloc(4096,sizeof(*index));if(!index)return NO;size_t count=0;
    FILE *input=fopen(working.fileSystemRepresentation,"rb");uint8_t payload[16384];size_t size;uint32_t type,seq=0;
    for(;;){long offset=input?ftell(input):0;uint32_t current=seq++;if(!input || !read_chunk(input,current,&type,payload,sizeof(payload),&size))break;
        BOOL keep=NO;uint64_t frame=0;
        if(type==2 && size==sizeof(game_perf_event)){game_perf_event e;memcpy(&e,payload,sizeof(e));if(e.type==GAME_PERF_EVENT_CONTEXT){keep=YES;frame=retained_index(e.denominator);}}
        if(type==3){@autoreleasepool{id epoch=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:payload length:size] options:0 error:nil];if([epoch isKindOfClass:NSDictionary.class]){if([epoch[@"frameID"] isKindOfClass:NSNumber.class]){uint64_t absolute=[epoch[@"frameID"] unsignedLongLongValue];keep=YES;frame=retained_index(absolute);}else if([epoch[@"frame"] isKindOfClass:NSNumber.class]){keep=YES;frame=[epoch[@"frame"] unsignedLongLongValue];}}}}
        if(keep && count<4095)index[count++]=(epoch_index){MIN(frame,frozen_frames),offset,current};
    }
    qsort(index,count,sizeof(*index),epoch_compare);
    NSMutableDictionary *context=[(initial_context?:metadata) mutableCopy];
    BOOL ok=write_bytes(out,",\"epochs\":[",sizeof(",\"epochs\":[")-1);BOOL first=YES;
    if(!count || index[0].frame){ok=ok && json_value(out,@{@"frame":@0,@"context":context});first=NO;}
    for(size_t i=0;ok && i<count;){uint64_t frame=index[i].frame;
        do {@autoreleasepool{epoch_index entry=index[i++];if(fseek(input,entry.offset,SEEK_SET) || !read_chunk(input,entry.sequence,&type,payload,sizeof(payload),&size)){ok=NO;break;}
            if(type==2){game_perf_event event;memcpy(&event,payload,sizeof(event));apply_context_event_to(context,&event);}
            else {id epoch=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:payload length:size] options:0 error:nil];if([epoch[@"context"] isKindOfClass:NSDictionary.class])[context addEntriesFromDictionary:epoch[@"context"]];}
        }}while(i<count && index[i].frame==frame);
        if(!first)ok=ok && write_bytes(out,",",1);first=NO;ok=ok && json_value(out,@{@"frame":@(frame),@"context":context});
    }
    if(input)fclose(input);free(index);return ok && write_bytes(out,"]",1);
}
/* Shadow rows share the retained trace's index, including frames outside spike windows. */
static BOOL write_dynamic_resolution(FILE *out,NSString *working) {
    NSDictionary *semantics=@{@"mode":@"shadow_only_no_resolution_changes",
        @"gpuTiming":@"frame_joined_stage_boundary_scene_upscale_hud_microseconds",
        @"cpuTiming":@"trace_cpu_subtracts_pacing_slot_visibility_and_readback_waits",
        @"presentation":@"actual_drawable_timestamp_and_interval_monotonic_microseconds_late_over_1_5_display_shares",
        @"lines":@"actual_rendered_lines_and_virtual_planner_choice",
        @"ignore":@"planner_ignore_reason_bitmask",
        @"action":@{@"0":@"hold",@"1":@"drop",@"2":@"panic",@"3":@"recover"},
        @"thermal":@{@"0":@"nominal",@"1":@"fair",@"2":@"serious",@"3":@"critical"},
        @"available":@{@"1":@"scene_upscale_hud",@"2":@"actual_presentation_interval",@"4":@"shadow_decision",@"8":@"stage_counter_capability"},
        @"gpuClockCheck":@"unvalidated_shadow_only",
        @"gpuClockLimitation":@"Shadow mode does not render at virtual dropped levels, so it cannot test whether lower resolution holds GPU clocks low. This collector does not measure GPU clock frequency."};
    BOOL ok=write_bytes(out,",\"dynamicResolution\":{\"semantics\":",sizeof(",\"dynamicResolution\":{\"semantics\":")-1) && json_value(out,semantics) && write_bytes(out,",\"frames\":[",sizeof(",\"frames\":[")-1);
    FILE *input=fopen(working.fileSystemRepresentation,"rb");if(!input)return NO;
    uint8_t payload[16384];size_t size;uint32_t type,seq=0;uint64_t index=0;
    while(ok && read_chunk(input,seq++,&type,payload,sizeof(payload),&size))if(type==1 && size==sizeof(game_perf_frame)) {@autoreleasepool{
        if(index>=frozen_frames)break;game_perf_frame f;memcpy(&f,payload,sizeof(f));
        const game_perf_dynamic_resolution *d=&f.drs;BOOL gpu=(d->available&1)!=0,presented=(d->available&2)!=0,shadow=(d->available&4)!=0;
        if(index)ok=write_bytes(out,",",1);
        ok=ok && json_value(out,@{@"frame":@(index),@"frameID":@(f.frame_id),
            @"sceneUS":gpu?@(d->scene_us):NSNull.null,@"upscaleUS":gpu?@(d->upscale_us):NSNull.null,@"hudUS":gpu?@(d->hud_us):NSNull.null,
            @"presentedUS":presented?@(d->presented_us):NSNull.null,@"presentationUS":presented?@(d->presentation_us):NSNull.null,@"late":presented?@(d->late):NSNull.null,
            @"actualLines":@(d->actual_lines),@"chosenLines":shadow?@(d->chosen_lines):NSNull.null,
            @"ceilingLines":@(d->ceiling_lines),@"floorLines":@(d->floor_lines),
            @"ignore":@(d->ignore),@"action":shadow?@(d->action):NSNull.null,@"thermal":@(d->thermal),@"available":@(d->available)});
        index++;
    }}
    fclose(input);return ok && index==frozen_frames && write_bytes(out,"]}",2);
}
static BOOL write_auxiliary(FILE *out,NSString *working) {
    BOOL ok=write_bytes(out,",\"events\":[",sizeof(",\"events\":[")-1);BOOL first=YES;
    FILE *input=fopen(working.fileSystemRepresentation,"rb");uint8_t payload[16384];size_t size;uint32_t type,seq=0;
    while(ok && input && read_chunk(input,seq++,&type,payload,sizeof(payload),&size))if(type==2 && size==sizeof(game_perf_event)) {@autoreleasepool{
        game_perf_event e;memcpy(&e,payload,sizeof(e));if(!first)ok=write_bytes(out,",",1);first=NO;
        ok=ok && json_value(out,@{@"elapsedUS":@(e.elapsed_us),@"type":@(e.type),@"value":@(e.value),@"denominator":@(e.denominator),@"flags":@(e.flags)});
    }}
    if(input)fclose(input);
    ok=ok && write_bytes(out,"],\"networkSamples\":[",sizeof("],\"networkSamples\":[")-1);first=YES;input=fopen(working.fileSystemRepresentation,"rb");seq=0;
    while(ok && input && read_chunk(input,seq++,&type,payload,sizeof(payload),&size))if(type==2 && size==sizeof(game_perf_event)) {@autoreleasepool{
        game_perf_event e;memcpy(&e,payload,sizeof(e));if(e.type!=15)continue;uint64_t index=retained_index(e.denominator);if(index>=frozen_frames || retained_frame_ids[index]!=e.denominator)continue;if(!first)ok=write_bytes(out,",",1);first=NO;
        ok=ok && json_value(out,@{@"frame":@(index),@"rttMS":@((double)e.value/1000),@"lost":NSNull.null,@"expected":NSNull.null});
    }}
    if(input)fclose(input);
    ok=ok && write_bytes(out,"],\"overflow\":[",sizeof("],\"overflow\":[")-1);first=YES;input=fopen(working.fileSystemRepresentation,"rb");seq=0;uint64_t index=0;
    NSArray *fields=@[@"cpu",@"simulation",@"network",@"render",@"gpu",@"budget"];
    while(ok && input && read_chunk(input,seq++,&type,payload,sizeof(payload),&size))if(type==1 && size==sizeof(game_perf_frame)) {@autoreleasepool{
        game_perf_frame f;memcpy(&f,payload,sizeof(f));if(index>=frozen_frames)break;uint64_t values[]={f.cpu_us,f.simulation_us,f.network_us,f.render_us,f.gpu_us,f.budget_us};
        if(f.interval_us>=UINT32_MAX && f.interval_us!=GAME_PERF_UNAVAILABLE){if(!first)ok=write_bytes(out,",",1);first=NO;ok=ok && json_value(out,@{@"frame":@(index),@"field":@"intervalUS",@"valueUS":@(f.interval_us)});}
        for(int i=0;i<6;i++)if(values[i]!=GAME_PERF_UNAVAILABLE && values[i]>=6553500){if(!first)ok=write_bytes(out,",",1);first=NO;ok=ok && json_value(out,@{@"frame":@(index),@"field":fields[i],@"valueUS":@(values[i])});}
        index++;
    }}
    if(input)fclose(input);
    ok=ok && write_bytes(out,"],\"windows\":[",sizeof("],\"windows\":[")-1);first=YES;
    const game_perf_window *windows=NULL;size_t count=recorder?game_perf_get_windows(recorder,&windows):0;
    for(size_t i=0;ok && !omit_detail && i<count;i++) {@autoreleasepool{
        const game_perf_window *w=&windows[i];NSMutableArray *details=[NSMutableArray arrayWithCapacity:w->frame_count];
        for(uint32_t j=0;j<w->frame_count;j++){const game_perf_frame *f=&w->frames[j];[details addObject:@{@"elapsedUS":@(f->elapsed_us),@"intervalUS":@(f->interval_us),@"draws":@(f->detail.draws),@"packetsSent":@(f->detail.packets_sent),@"packetsReceived":@(f->detail.packets_received),@"bytesSent":@(f->detail.bytes_sent),@"bytesReceived":@(f->detail.bytes_received),@"networkedObjects":@(f->detail.networked_objects),@"pacingUS":@(f->detail.pacing_us),@"gpuPassUS":@[@(f->detail.gpu_pass_us[0]),@(f->detail.gpu_pass_us[1]),@(f->detail.gpu_pass_us[2]),@(f->detail.gpu_pass_us[3]),@(f->detail.gpu_pass_us[4]),@(f->detail.gpu_pass_us[5]),@(f->detail.gpu_pass_us[6]),@(f->detail.gpu_pass_us[7])],@"available":@(f->detail.available)}];}
        NSMutableDictionary *window=[@{@"beginUS":@(w->begin_us),@"endUS":@(w->end_us),@"peakUS":@(w->peak_us),@"truncated":@(w->truncated),@"frames":details} mutableCopy];
        while(details.count){NSUInteger encoded;@autoreleasepool{encoded=[NSJSONSerialization dataWithJSONObject:window options:0 error:nil].length;}if(encoded<=128*1024)break;NSUInteger keep=details.count/2;[details removeObjectsInRange:NSMakeRange(keep,details.count-keep)];window[@"truncated"]=@YES;}
        if(!first)ok=write_bytes(out,",",1);first=NO;ok=ok && json_value(out,window);
    }}
    ok=ok && write_bytes(out,"],\"gaps\":[",sizeof("],\"gaps\":[")-1);game_perf_status status={0};if(recorder && !recovered_windows_active)game_perf_get_status(recorder,&status);if(recovered_windows_active && recovered_summary_valid)status=recovered_status;
    if(status.frames_dropped || status.events_dropped || lost_rows || lost_events || frozen_skipped || rejected_tail)ok=ok && json_value(out,@{@"beginUS":@(omitted_begin_us && status.first_gap_us?MIN(omitted_begin_us,status.first_gap_us):MAX(omitted_begin_us,status.first_gap_us)),@"endUS":@(MAX(status.last_gap_us,omitted_end_us)),@"frames":@(status.frames_dropped+lost_rows+frozen_skipped),@"events":@(status.events_dropped+lost_events),@"reason":rejected_tail?@"incomplete_or_invalid_journal_tail":@"bounded_recorder_or_journal",@"unknownTail":@(rejected_tail)});
    return ok && write_bytes(out,"]",1);
}
static NSDictionary *summary_json(const game_perf_summary *summary) {
    NSArray *names=@[@"interval",@"cpu",@"simulation",@"network",@"render",@"gpu",@"budget"];NSMutableDictionary *groups=[NSMutableDictionary dictionary];
    for(int g=0;g<2;g++){NSMutableDictionary *metrics=[NSMutableDictionary dictionary];for(int m=0;m<7;m++){
        const game_perf_metric_summary *v=&summary->metrics[g][m];NSMutableDictionary *percentiles=[NSMutableDictionary dictionary];const unsigned percent[]={50,90,95,99};
        for(int p=0;p<4;p++){uint64_t target=(v->available*percent[p]+99)/100,seen=0;int bucket=0;for(;bucket<63;bucket++){seen+=v->histogram[bucket];if(seen>=target)break;}percentiles[[NSString stringWithFormat:@"p%uUS",percent[p]]]=v->available?@(bucket==63?UINT64_MAX:((UINT64_C(1)<<(bucket+1))-1)):(id)NSNull.null;}
        percentiles[@"available"]=@(v->available);percentiles[@"sumUS"]=@(v->sum_us);percentiles[@"maxUS"]=@(v->max_us);percentiles[@"quantiles"]=@"log2_histogram_upper_bound";metrics[names[m]]=percentiles;
    }groups[g?@"loading":@"play"]=@{@"frames":@(summary->frames[g]),@"overBudget":@(summary->over_budget[g]),@"budgetAvailable":@(summary->budget_available[g]),@"lateClassification":@"interval_over_budget_proxy",@"metrics":metrics};}
    return groups;
}
/* Exact quantiles cover retained journal rows. The all-frame histogram also covers drops. */
static int perf_duration_compare(const void *left,const void *right) {
    uint64_t a=*(const uint64_t *)left,b=*(const uint64_t *)right;return (a>b)-(a<b);
}
static NSDictionary *trace_summary(NSString *working) {
    uint64_t *values=malloc(MAX_TRACE_ROWS*sizeof(*values));if(!values)return @{};
    NSMutableDictionary *groups=[NSMutableDictionary dictionary];
    NSArray *names=@[@"interval",@"cpu",@"simulation",@"network",@"render",@"gpu",@"budget"];
    for(unsigned loading=0;loading<2;loading++) {
        NSMutableDictionary *metrics=[NSMutableDictionary dictionary];uint64_t frames=0,over=0,budgets=0;
        for(unsigned field=0;field<7;field++) {@autoreleasepool{
            FILE *input=fopen(working.fileSystemRepresentation,"rb");uint32_t type,seq=0;size_t size,n=0;uint64_t row=0;
            uint8_t payload[16384];
            while(input && read_chunk(input,seq++,&type,payload,sizeof(payload),&size)) {
                if(type!=1 || size!=sizeof(game_perf_frame))continue;
                if(row++>=frozen_frames)break;
                game_perf_frame f;memcpy(&f,payload,sizeof(f));if(!!(f.flags&GAME_PERF_LOADING)!=loading)continue;
                uint64_t timings[]={f.interval_us,f.cpu_us,f.simulation_us,f.network_us,f.render_us,f.gpu_us,f.budget_us};
                if(!field){frames++;if(f.budget_us!=GAME_PERF_UNAVAILABLE && f.budget_us){budgets++;if(f.interval_us>f.budget_us)over++;}}
                if(timings[field]!=GAME_PERF_UNAVAILABLE && n<MAX_TRACE_ROWS)values[n++]=timings[field];
            }
            if(input)fclose(input);qsort(values,n,sizeof(*values),perf_duration_compare);
            NSMutableDictionary *m=[@{@"available":@(n),@"quantiles":@"nearest_rank_original_microseconds"} mutableCopy];
            unsigned percent[]={50,90,95,99};NSArray *keys=@[@"p50US",@"p90US",@"p95US",@"p99US"];
            for(unsigned p=0;p<4;p++)m[keys[p]]=n?@(values[(n*percent[p]+99)/100-1]):(id)NSNull.null;
            m[@"maxUS"]=n?@(values[n-1]):(id)NSNull.null;metrics[names[field]]=m;
        }}
        groups[loading?@"loading":@"play"]=@{@"frames":@(frames),@"overBudget":@(over),@"budgetAvailable":@(budgets),@"lateClassification":@"interval_over_budget_proxy",@"metrics":metrics};
    }
    free(values);return groups;
}

static NSString *finish_report(BOOL interrupted) {
    frozen_skipped=0;rejected_tail=NO;
    game_perf_status before_replay={0};if(recorder)game_perf_get_status(recorder,&before_replay);
    BOOL recovery_mode=interrupted && recorder && (recovered_windows_active || !before_replay.frames_seen);
    BOOL rebuild_windows=recovery_mode && !recovered_windows_active;

    if(journal){fflush(journal);fsync(fileno(journal));fclose(journal);journal=NULL;}
    NSString *working=[ROOT stringByAppendingPathComponent:@"working.bin"];
    FILE *input=fopen(working.fileSystemRepresentation,"rb");if(!input)return nil;
    if(rebuild_windows){game_perf_set_enricher(recorder,NULL,NULL);rebuild_windows=game_perf_start(recorder,0);if(rebuild_windows)recovered_windows_active=YES;else game_perf_set_enricher(recorder,enrich_frame,NULL);}
    FILE *columns[10]={0};BOOL valid=YES;for(int i=0;i<10;i++){columns[i]=tmpfile();if(!columns[i])valid=NO;}
    uint8_t payload[16384];size_t size;uint32_t type,seq=0;uint64_t count=0,max_elapsed=0;game_perf_summary summary={0},recovered_tail={0};
    while(valid && read_chunk(input,seq++,&type,payload,sizeof(payload),&size)) {
        if(type==2 && size==sizeof(game_perf_event)){game_perf_event event;memcpy(&event,payload,sizeof(event));@autoreleasepool{apply_context_event_to(metadata,&event);}continue;}
        if(type==3){@autoreleasepool{id epoch=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:payload length:size] options:0 error:nil];if([epoch isKindOfClass:NSDictionary.class] && [epoch[@"context"] isKindOfClass:NSDictionary.class])[metadata addEntriesFromDictionary:epoch[@"context"]];}continue;}
        if(type!=1 || size!=sizeof(game_perf_frame))continue;
        if(rebuild_windows){game_perf_frame detail;memcpy(&detail,payload,sizeof(detail));game_perf_append_frame(recorder,&detail);game_perf_drain(recorder,NULL,NULL,NULL);}
        if(recovered_summary_valid){game_perf_frame tail_frame;memcpy(&tail_frame,payload,sizeof(tail_frame));if(tail_frame.frame_id>recovered_frame_id)game_perf_summary_append(&recovered_tail,&tail_frame);}
        if(count>=trace_limit){game_perf_frame omitted;memcpy(&omitted,payload,sizeof(omitted));truncated=YES;frozen_skipped++;if(!omitted_begin_us || omitted.elapsed_us<omitted_begin_us)omitted_begin_us=omitted.elapsed_us;omitted_end_us=MAX(omitted_end_us,omitted.elapsed_us);continue;}
        game_perf_frame frame;memcpy(&frame,payload,sizeof(frame));if(!count)first_frame_id=frame.frame_id;retained_frame_ids[count]=frame.frame_id;game_perf_summary_append(&summary,&frame);uint8_t encoded[24];
        game_perf_encode_columns(&frame,1,encoded,sizeof(encoded));size_t offset=0;
        for(int i=0;i<10;i++){size_t width=i<2?4:2;valid=valid && write_bytes(columns[i],encoded+offset,width);offset+=width;}
        count++;max_elapsed=MAX(max_elapsed,frame.elapsed_us);
    }
    fclose(input);frozen_frames=count;if(rebuild_windows){game_perf_end(recorder,MAX(duration_us,max_elapsed),GAME_PERF_END_INTERRUPTED);game_perf_drain(recorder,NULL,NULL,NULL);game_perf_set_enricher(recorder,enrich_frame,NULL);}
    if(rejected_tail){truncated=YES;if(!omitted_begin_us)omitted_begin_us=max_elapsed;omitted_end_us=MAX(omitted_end_us,MAX(duration_us,max_elapsed));}
    if(!interrupted && !atomic_load(&abnormal) && summary.metrics[0][0].sum_us<10000000){for(int i=0;i<10;i++)if(columns[i])fclose(columns[i]);[NSFileManager.defaultManager removeItemAtPath:working error:nil];[NSFileManager.defaultManager removeItemAtPath:[ROOT stringByAppendingPathComponent:@"working.json"] error:nil];return @"discarded_short_normal_game";}
    NSString *temporary=[ROOT stringByAppendingPathComponent:@"freezing.json"];
    FILE *out=valid ? fopen(temporary.fileSystemRepresentation,"wb") : NULL;valid=out!=NULL;
    NSMutableDictionary *head=[metadata mutableCopy];head[@"frameCount"]=@(count);head[@"durationMS"]=@(max_elapsed/1000);head[@"interrupted"]=@(interrupted);head[@"abnormal"]=@(atomic_load(&abnormal));head[@"truncated"]=@(truncated);head[@"recoveryGap"]=@(rejected_tail);head[@"droppedFrames"]=@(lost_rows+frozen_skipped);head[@"droppedGraphics"]=@(atomic_load(&graphics_dropped));head[@"gpuTimestampIntervalsDropped"]=@(host_perf_metal_dropped());
    game_perf_status status={0};if(recorder && !recovery_mode)game_perf_get_status(recorder,&status);if(status.frames_seen){game_perf_metric_summary gpu[2]={summary.metrics[0][5],summary.metrics[1][5]};game_perf_get_summary(recorder,&summary);summary.metrics[0][5]=gpu[0];summary.metrics[1][5]=gpu[1];head[@"droppedFrames"]=@(status.frames_dropped+lost_rows+frozen_skipped);head[@"droppedEvents"]=@(status.events_dropped);head[@"droppedDetail"]=@(status.detail_dropped);}
    if(!status.frames_seen && recovered_summary_valid){
        game_perf_metric_summary gpu[2]={summary.metrics[0][5],summary.metrics[1][5]};summary=recovered_summary;merge_summary(&summary,&recovered_tail);
        summary.metrics[0][5]=gpu[0];summary.metrics[1][5]=gpu[1];status=recovered_status;
        head[@"droppedFrames"]=@(status.frames_dropped+lost_rows+frozen_skipped);head[@"droppedDetail"]=@(status.detail_dropped);
    }
    head[@"endReason"]=@(atomic_load(&abnormal)?GAME_PERF_END_DISCONNECT:interrupted && recovery_mode && status.active?GAME_PERF_END_INTERRUPTED:status.frames_seen?status.end_reason:interrupted?GAME_PERF_END_INTERRUPTED:GAME_PERF_END_NORMAL);
    head[@"durationMS"]=@(MAX(MAX(max_elapsed,duration_us),status.end_us>=status.start_us?status.end_us-status.start_us:0)/1000);head[@"droppedEvents"]=@(status.events_dropped+lost_events);
    head[@"summary"]=summary_json(&summary);head[@"gpuSummaryCoverage"]=@"joined_retained_trace_frames";
    head[@"traceSummary"]=trace_summary(working);
    NSData *header=[NSJSONSerialization dataWithJSONObject:head options:NSJSONWritingSortedKeys error:nil];
    if(valid)valid=header.length && write_bytes(out,header.bytes,header.length-1) && write_bytes(out,",\"trace\":{\"encoding\":\"game-perf-columns-1\"",sizeof(",\"trace\":{\"encoding\":\"game-perf-columns-1\"")-1);
    NSArray *names=@[@"intervalUS",@"elapsedMS",@"cpu",@"simulation",@"network",@"render",@"gpu",@"budget",@"ticks",@"flags"];
    for(int i=0;i<10;i++) {
        if(valid){valid=write_bytes(out,",",1) && json_value(out,names[i]) && write_bytes(out,":\"",2);rewind(columns[i]);uint8_t bytes[12288];size_t n;
            while(valid && (n=fread(bytes,1,sizeof(bytes),columns[i]))){@autoreleasepool{NSData *data=[NSData dataWithBytesNoCopy:bytes length:n freeWhenDone:NO];NSData *base64=[[data base64EncodedStringWithOptions:0] dataUsingEncoding:NSASCIIStringEncoding];valid=write_bytes(out,base64.bytes,base64.length);}}
            valid=valid && write_bytes(out,"\"",1);
        }
        if(columns[i])fclose(columns[i]);
    }
    if(valid)valid=write_bytes(out,"}",1) && write_epochs(out,working) && (!host_perf_hooks_dynamic_enabled() || write_dynamic_resolution(out,working)) && write_auxiliary(out,working) && write_bytes(out,"}",1);
    if(out){if(fflush(out) || fsync(fileno(out)))valid=NO;fclose(out);}
    NSNumber *bytes=[NSFileManager.defaultManager attributesOfItemAtPath:temporary error:nil][NSFileSize];if(bytes.unsignedLongLongValue>JOURNAL_LIMIT){[NSFileManager.defaultManager removeItemAtPath:temporary error:nil];if(!omit_detail){omit_detail=YES;return finish_report(interrupted);}if(trace_limit>1000){trace_limit/=2;truncated=YES;return finish_report(interrupted);}valid=NO;}
    FILE *raw=valid?fopen(temporary.fileSystemRepresentation,"rb"):NULL;NSString *gzip=[ROOT stringByAppendingPathComponent:@"freezing.gz"];gzFile compressed=raw?gzopen(gzip.fileSystemRepresentation,"wb"):NULL;
    CC_SHA256_CTX context;CC_SHA256_Init(&context);uint8_t buffer[16384];size_t n;
    valid=valid && raw && compressed;
    while(valid && (n=fread(buffer,1,sizeof(buffer),raw))){CC_SHA256_Update(&context,buffer,(CC_LONG)n);if(gzwrite(compressed,buffer,(unsigned)n)!=(int)n)valid=NO;}
    if(raw){if(ferror(raw))valid=NO;fclose(raw);}if(compressed && gzclose(compressed)!=Z_OK)valid=NO;
    if([[NSFileManager.defaultManager attributesOfItemAtPath:gzip error:nil][NSFileSize] unsignedLongLongValue]>ASSET_LIMIT){
        [NSFileManager.defaultManager removeItemAtPath:temporary error:nil];[NSFileManager.defaultManager removeItemAtPath:gzip error:nil];
        if(!omit_detail){omit_detail=YES;return finish_report(interrupted);}if(trace_limit>1000){trace_limit/=2;truncated=YES;return finish_report(interrupted);}valid=NO;
    }
    uint8_t digest[32];CC_SHA256_Final(digest,&context);NSMutableString *hash=[NSMutableString string];for(int i=0;i<32;i++)[hash appendFormat:@"%02x",digest[i]];
    NSString *path=[OUTBOX stringByAppendingPathComponent:[NSString stringWithFormat:@"perf-%@.json",hash]];
    if(valid){head[@"payloadSHA256"]=hash;NSString *stored=host_typed_outbox_store_asset(OUTBOX,gzip,head,@"perf",hash,^BOOL(NSString *asset,NSDictionary *fields,NSString *digest){return host_perf_validate_asset(asset,fields,digest);},nil);valid=stored!=nil;}
    [NSFileManager.defaultManager removeItemAtPath:temporary error:nil];[NSFileManager.defaultManager removeItemAtPath:gzip error:nil];
    if(valid){[NSFileManager.defaultManager removeItemAtPath:working error:nil];[NSFileManager.defaultManager removeItemAtPath:[ROOT stringByAppendingPathComponent:@"working.json"] error:nil];}
    return valid?path:nil;
}
static void trim(void) {
    NSArray *paths=[host_typed_outbox_pending(OUTBOX,@"perf") sortedArrayUsingComparator:^NSComparisonResult(NSString *a,NSString *b){return [[NSFileManager.defaultManager attributesOfItemAtPath:a error:nil][NSFileModificationDate] compare:[NSFileManager.defaultManager attributesOfItemAtPath:b error:nil][NSFileModificationDate]];}];
    uint64_t total=0;for(NSString *path in paths)total+=[[NSFileManager.defaultManager attributesOfItemAtPath:[path stringByAppendingPathExtension:@"gz"] error:nil][NSFileSize] unsignedLongLongValue]+[[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil][NSFileSize] unsignedLongLongValue];
    NSUInteger remaining=paths.count;
    for(NSString *path in paths){NSDictionary *attrs=[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];if(remaining<=128 && total<=64*1024*1024 && [attrs[NSFileModificationDate] timeIntervalSinceNow]>-7*86400)continue;
        NSString *asset=[path stringByAppendingPathExtension:@"gz"];uint64_t size=[[NSFileManager.defaultManager attributesOfItemAtPath:asset error:nil][NSFileSize] unsignedLongLongValue]+[attrs[NSFileSize] unsignedLongLongValue];
        [NSFileManager.defaultManager removeItemAtPath:asset error:nil];[NSFileManager.defaultManager removeItemAtPath:path error:nil];total-=MIN(total,size);remaining--;[delivery[@"retries"] removeObjectForKey:path.lastPathComponent];delivery[@"evicted"]=@([delivery[@"evicted"] unsignedLongLongValue]+1);
    }
    persist(delivery,[ROOT stringByAppendingPathComponent:@"delivery.json"]);
}
static void deliver(void) {
#if HALO_PERF_REPORTS_TESTING
    if(getenv("HALO_PERF_REPORTS_TEST_OVERRIDE") && !strcmp(getenv("HALO_PERF_REPORTS_TEST_OVERRIDE"),"1"))return;
#endif
    if(!atomic_load(&menu) || atomic_load(&suspended) || !atomic_load(&enabled) || atomic_load(&runtime_suppressed) || host_benchmark_running())return;
    NSString *version=[NSString stringWithFormat:@"%@/%@",[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"],[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]];
    if([delivery[@"paused"] isEqual:version])return;
    long day=(long)(NSDate.date.timeIntervalSince1970/86400);if([delivery[@"day"] longValue]!=day){delivery[@"day"]=@(day);delivery[@"attempts"]=@0;delivery[@"bytes"]=@0;}
    if([delivery[@"attempts"] unsignedIntegerValue]>=20)return;
    for(NSString *path in host_typed_outbox_pending(OUTBOX,@"perf")) {@autoreleasepool {
        NSDictionary *attrs=[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];if([attrs[NSFileSize] unsignedLongLongValue]>65536)continue;
        NSDictionary *head=bounded_json(path,65536);
        NSString *asset=[path stringByAppendingPathExtension:@"gz"];NSNumber *size=[NSFileManager.defaultManager attributesOfItemAtPath:asset error:nil][NSFileSize];
        NSDictionary *retry=delivery[@"retries"][path.lastPathComponent];if([retry[@"after"] doubleValue]>NSDate.date.timeIntervalSince1970)continue;
        if(![head isKindOfClass:NSDictionary.class] || ![head[@"schema"] isEqual:@1] || !size || size.unsignedLongLongValue>ASSET_LIMIT || size.unsignedLongLongValue+[delivery[@"bytes"] unsignedLongLongValue]>32*1024*1024)continue;
        if(![path.lastPathComponent isEqual:[NSString stringWithFormat:@"perf-%@.json",head[@"payloadSHA256"]]] || !host_perf_validate_asset(asset,head,head[@"payloadSHA256"]))continue;
        if(!host_report_delivery_acquire())return;
        delivery[@"attempts"]=@([delivery[@"attempts"] unsignedIntegerValue]+1);delivery[@"bytes"]=@([delivery[@"bytes"] unsignedLongLongValue]+size.unsignedLongLongValue);
        if(!persist(delivery,[ROOT stringByAppendingPathComponent:@"delivery.json"])){host_report_delivery_release();return;}
        host_report_delivery_submit(path,asset,head,head[@"payloadSHA256"],worker,^(BOOL success,NSError *error){
            if(success){if(host_benchmark_outbox_acknowledge(path,nil)){[NSFileManager.defaultManager removeItemAtPath:asset error:nil];[NSFileManager.defaultManager removeItemAtPath:path error:nil];[NSFileManager.defaultManager removeItemAtPath:[path stringByAppendingPathExtension:@"sent"] error:nil];[delivery[@"retries"] removeObjectForKey:path.lastPathComponent];}
                else delivery[@"retries"][path.lastPathComponent]=@{@"attempt":@1,@"after":@(NSDate.date.timeIntervalSince1970+60)};}
            else {NSUInteger attempt=[retry[@"attempt"] unsignedIntegerValue];NSTimeInterval delay=host_shader_upload_retry(attempt,[error.userInfo[@"CKRetryAfter"] doubleValue],(double)arc4random_uniform(10000)/10000);
                delivery[@"retries"][path.lastPathComponent]=@{@"attempt":@(attempt+1),@"after":@(NSDate.date.timeIntervalSince1970+delay)};
                /* CloudKit permission/schema errors stay paused until the app changes. */
                if(error.code==10 || error.code==15 || error.code==12)delivery[@"paused"]=version;
            }
            persist(delivery,[ROOT stringByAppendingPathComponent:@"delivery.json"]);
        });return;
    }}
}
static void drain_graphics(void) {
    unsigned tail=atomic_load_explicit(&graphics_tail,memory_order_relaxed),head=atomic_load_explicit(&graphics_head,memory_order_acquire);
    while(tail!=head){struct game_perf_graphics_snapshot snapshot=graphics_queue[tail%2].value;uint64_t frame_id=graphics_queue[tail%2].frame_id;atomic_store_explicit(&graphics_tail,++tail,memory_order_release);
        NSMutableDictionary *values=[NSMutableDictionary dictionary];
        for(uint32_t i=0;i<snapshot.count && i<128;i++){const struct game_perf_graphics_value *entry=&snapshot.entries[i];
            if(!memchr(entry->name,0,sizeof(entry->name)))continue;NSString *name=@(entry->name);id value=nil;
            switch(entry->type){case GAME_PERF_GRAPHICS_BOOLEAN:value=@(entry->value.boolean!=0);break;case GAME_PERF_GRAPHICS_INTEGER:value=@(entry->value.integer);break;case GAME_PERF_GRAPHICS_REAL:if(isfinite(entry->value.real))value=@(entry->value.real);break;case GAME_PERF_GRAPHICS_STRING:if(memchr(entry->value.string,0,sizeof(entry->value.string)))value=@(entry->value.string);break;default:break;}
            if(value)values[name]=value;
        }
        NSData *json=[NSJSONSerialization dataWithJSONObject:@{@"values":values} options:NSJSONWritingSortedKeys error:nil];NSString *text=[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];graphics_frame_override=frame_id;host_perf_reports_graphics(text.UTF8String);graphics_frame_override=0;
    }
}

#endif
void host_perf_reports_start(const char *sha) {
#if HALO_PERF_REPORTS
    static dispatch_once_t once;dispatch_once(&once,^{
        BOOL valid_guest=sha && strlen(sha)==64 && strspn(sha,"0123456789abcdef")==64;BOOL eligible=valid_guest;
#if TARGET_OS_SIMULATOR
        eligible=NO;
#endif
        if(getenv("HALO_RUNNER") || getenv("HALO_BENCHMARK_SUITE") || getenv("HALO_RENDER_REPLAY") || getenv("HALO_PROBE"))eligible=NO;
#if HALO_PERF_REPORTS_TESTING
        if(getenv("HALO_PERF_REPORTS_TEST_OVERRIDE") && !strcmp(getenv("HALO_PERF_REPORTS_TEST_OVERRIDE"),"1"))eligible=valid_guest;
#endif
        if(!eligible)return;
        host_map_identity_init();recorder=game_perf_create();if(!recorder)return;game_perf_set_enricher(recorder,enrich_frame,NULL);
        guest=@(sha);dispatch_queue_attr_t attributes=dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0);
        attributes=dispatch_queue_attr_make_with_autorelease_frequency(attributes,DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM);
        worker=dispatch_queue_create("org.visr.perf-reports",attributes);dispatch_queue_set_specific(worker,&worker_key,&worker_key,NULL);
        [NSFileManager.defaultManager createDirectoryAtPath:OUTBOX withIntermediateDirectories:YES attributes:nil error:nil];
        delivery=bounded_json([ROOT stringByAppendingPathComponent:@"delivery.json"],65536);
        if(![delivery isKindOfClass:NSMutableDictionary.class])delivery=[NSMutableDictionary dictionary];
        if(![delivery[@"retries"] isKindOfClass:NSMutableDictionary.class])delivery[@"retries"]=[NSMutableDictionary dictionary];
        atomic_store(&enabled,true);
        dispatch_async(worker,^{for(NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:OUTBOX error:nil])if([name hasSuffix:@".json.sent"]){NSString *receipt=[OUTBOX stringByAppendingPathComponent:name],*manifest=receipt.stringByDeletingPathExtension;[NSFileManager.defaultManager removeItemAtPath:[manifest stringByAppendingPathExtension:@"gz"] error:nil];[NSFileManager.defaultManager removeItemAtPath:manifest error:nil];[NSFileManager.defaultManager removeItemAtPath:receipt error:nil];}
            id restored=bounded_json([ROOT stringByAppendingPathComponent:@"working.json"],65536);
            if([restored isKindOfClass:NSMutableDictionary.class] && working_metadata_valid(restored)){metadata=restored;restore_checkpoint(metadata);finish_report(YES);}else discard_orphaned_recording();
            [NSFileManager.defaultManager removeItemAtPath:[ROOT stringByAppendingPathComponent:@"freezing.json"] error:nil];[NSFileManager.defaultManager removeItemAtPath:[ROOT stringByAppendingPathComponent:@"freezing.gz"] error:nil];trim();});
#if HALO_NATIVE_MAC
        NSString *leave=NSApplicationWillResignActiveNotification,*resume=NSApplicationDidBecomeActiveNotification;
#else
        NSString *leave=UIApplicationWillResignActiveNotification,*resume=UIApplicationDidBecomeActiveNotification;
#endif
        [NSNotificationCenter.defaultCenter addObserverForName:leave object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;host_perf_reports_suspend();}];
        [NSNotificationCenter.defaultCenter addObserverForName:resume object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;atomic_store(&suspended,false);dispatch_async(worker,^{resume_segment();});}];
        [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;dispatch_async(worker,^{thermal_event();});}];
        timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,worker);dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC/10),NSEC_PER_SEC/10,NSEC_PER_SEC/100);
        dispatch_source_set_event_handler(timer,^{@autoreleasepool{drain();resume_segment();deliver();}});dispatch_resume(timer);
    });
#else
    (void)sha;
#endif
}
void host_perf_reports_activate(const char *name,int suppressed) {
    host_perf_hooks_dynamic_flush();host_perf_hooks_dynamic_ignore(4);
#if HALO_PERF_REPORTS
    if(!worker)return;
#if HALO_PERF_REPORTS_TESTING
    if(getenv("HALO_PERF_REPORTS_TEST_OVERRIDE") && !strcmp(getenv("HALO_PERF_REPORTS_TEST_OVERRIDE"),"1"))suppressed=0;
#endif
    if(host_benchmark_running())suppressed=1;
    atomic_store(&runtime_suppressed,suppressed!=0);
    NSString *map=name?@(name):@"";BOOL ui=[[[map stringByReplacingOccurrencesOfString:@"\\" withString:@"/"] lastPathComponent].stringByDeletingPathExtension isEqual:@"ui"];
    atomic_store(&menu,ui);atomic_store(&collecting,false);
    /* The game thread only publishes the boundary. Ending, finishing (compression can
       take seconds after a long game) and the next segment's setup run on the serial
       worker, ordered after any earlier start. The game thread is the producer and has
       stopped appending; other appenders are waited out as at exit. */
    BOOL named=name!=NULL;uint64_t ended=clock_us();
    dispatch_async(worker,^{@autoreleasepool{
    uint64_t deadline=clock_us()+100000;while(atomic_load(&appenders) && clock_us()<deadline)usleep(1000);
    game_perf_end(recorder,ended,GAME_PERF_END_NORMAL);
    host_report_delivery_cancel_performance();resume_play=NO;resume_ready=NO;drain();if(journal)finish_report(NO);trim();
    if(ui || suppressed || !named || atomic_load(&suspended) || [NSFileManager.defaultManager fileExistsAtPath:[ROOT stringByAppendingPathComponent:@"working.bin"]])return;
    NSDictionary *identity=host_shader_upload_map_identity(map.UTF8String);
    atomic_store(&abnormal,false);start_us=clock_us();journal_bytes=0;sequence=0;rows=0;first_frame_id=0;duration_us=0;lost_rows=0;lost_events=0;journal_events=0;omitted_begin_us=0;omitted_end_us=0;truncated=NO;trace_limit=MAX_TRACE_ROWS;omit_detail=NO;recovered_summary_valid=NO;recovered_windows_active=NO;
    char model[128]="unknown";size_t length=sizeof(model);
#if HALO_NATIVE_MAC || TARGET_OS_MACCATALYST
    sysctlbyname("hw.model",model,&length,NULL,0);
#else
    sysctlbyname("hw.machine",model,&length,NULL,0);
#endif
    NSString *platform=@"ios";
#if HALO_NATIVE_MAC
    platform=@"macos";
#elif TARGET_OS_VISION
    platform=@"visionos";
#elif TARGET_OS_TV
    platform=@"tvos";
#endif
    struct host_profile snapshot;host_device_profile_copy(&snapshot);
    const struct host_profile *profile=&snapshot;
    metadata=[@{@"schema":@1,@"nonce":[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""].lowercaseString,@"model":@(model),@"platform":platform,@"profileID":profile && profile->model_name[0]?@(profile->model_name):profile && profile->class_name[0]?@(profile->class_name):@"default",@"mapFingerprint":identity[@"fingerprint"]?:@"unknown",@"mapFormat":identity[@"format"]?:@"unknown",@"renderer":@"unknown",@"mode":@"unknown",@"appVersion":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]?:@"unknown",@"appBuild":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]?:@"unknown",@"guestSHA256":guest,@"graphics":graphics?:@{},@"profileHash":graphics_hash?:@"unknown",@"os":NSProcessInfo.processInfo.operatingSystemVersionString,@"networkVersion":@(HALO_PORT_NETWORK_VERSION),@"journalABI":@1,@"journalFrameSize":@(sizeof(game_perf_frame)),@"timingSemantics":@{@"interval":@"frame_start_interval",@"gpu":@"completed_main_command_buffers_joined_by_frame_id",@"presentation":@"attempt_evidence_only",@"deadlineMiss":@"interval_over_budget_proxy",@"packetCounts":@"successful_application_datagrams",@"packetLoss":@"unavailable",@"rtt":@"mean_supported_positive_remote_player_ping_observations",@"networkStall":@"measured_network_cpu_span_over_50ms_not_transport_progress",@"gpuPasses":@"frame_joined_color_depth_color_only_depth_only_encoder_durations_draw_boundaries_or_overlapping_stage_envelopes_not_additive_gpu_busy_time"}} mutableCopy];
active_map=map;initial_context=[metadata copy];persist(metadata,[ROOT stringByAppendingPathComponent:@"working.json"]);journal=fopen([[ROOT stringByAppendingPathComponent:@"working.bin"] fileSystemRepresentation],"wb");
        if(journal && game_perf_start(recorder,start_us)){
            /* The guest's load-begin hook usually runs before this start; record it here instead. */
            game_perf_event begin={.type=GAME_PERF_EVENT_MAP_LOAD_BEGIN};game_perf_append_event(recorder,&begin);atomic_store(&load_begin_logged,true);
            thermal_event();atomic_store(&collecting,true);}
    }});
#else
    (void)name;(void)suppressed;
#endif
}
void host_perf_reports_identity(const char *resolved_path) {
#if HALO_PERF_REPORTS
    if(!worker || !resolved_path || !*resolved_path || strnlen(resolved_path,4097)>4096)return;
    NSString *path=[NSString stringWithUTF8String:resolved_path];if(!path)return;
    dispatch_async(worker,^{if(!journal)return;active_map=path;refresh_map_identity();});
#else
    (void)resolved_path;
#endif
}
void host_perf_reports_frame(const game_perf_frame *value) {
#if HALO_PERF_REPORTS
    if(!value)return;atomic_fetch_add(&appenders,1);
    if(atomic_load_explicit(&collecting,memory_order_acquire) && value->elapsed_us>=start_us){game_perf_frame frame=*value;frame.elapsed_us-=start_us;game_perf_append_frame(recorder,&frame);}
    atomic_fetch_sub(&appenders,1);
#else
    (void)value;
#endif
}
void host_perf_reports_event(const game_perf_event *value) {
#if HALO_PERF_REPORTS
    if(value){atomic_fetch_add(&appenders,1);if(atomic_load_explicit(&collecting,memory_order_acquire)){if(value->type==14)atomic_store(&abnormal,true);if(value->type==GAME_PERF_EVENT_MAP_LOAD_BEGIN && atomic_exchange(&load_begin_logged,true)){atomic_fetch_sub(&appenders,1);return;}game_perf_event event=*value;event.elapsed_us=event.elapsed_us>=start_us?event.elapsed_us-start_us:0;game_perf_append_event(recorder,&event);}atomic_fetch_sub(&appenders,1);}
#else
    (void)value;
#endif
}
void host_perf_reports_suspend(void) {
    host_perf_hooks_dynamic_pause();
#if HALO_PERF_REPORTS
    if(!worker)return;atomic_store(&suspended,true);BOOL was_playing=atomic_exchange(&collecting,false);
    dispatch_async(worker,^{host_report_delivery_cancel_performance();resume_play=was_playing;resume_ready=NO;});
    __block dispatch_block_t flush;flush=^{if(atomic_load(&appenders)){dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_MSEC),worker,flush);return;}
        game_perf_end(recorder,clock_us(),GAME_PERF_END_SUSPEND);drain();if(journal)finish_report(YES);resume_ready=YES;resume_segment();flush=nil;
    };dispatch_async(worker,flush);
#endif
}

#if HALO_PERF_REPORTS
static BOOL graphics_string_allowed(NSString *name,NSString *value) {
    NSString *choices=nil;
#define PROFILE_BOOLEAN(key)
#define PROFILE_INTEGER(key, minimum, maximum)
#define PROFILE_REAL(key, minimum, maximum)
#define PROFILE_STRING(key, accepted) if([name isEqual:@key])choices=@accepted;
#define PROFILE_SCENE(key)
#define PROFILE_VARIABLE(key)
#define PROFILE_EXTERNAL(key)
#include "../../linux/src/port_profile_keys.h"
#undef PROFILE_BOOLEAN
#undef PROFILE_INTEGER
#undef PROFILE_REAL
#undef PROFILE_STRING
#undef PROFILE_SCENE
#undef PROFILE_VARIABLE
#undef PROFILE_EXTERNAL
    if(!choices || !value.length || value.length>80)return NO;
    for(NSString *choice in [choices componentsSeparatedByString:@"|"]){
        if(![choice isEqual:@"#"] && [value isEqual:choice])return YES;
        if([choice isEqual:@"#"] && [name isEqual:@"display.frame_rate"] && value.length<=4 &&
           [value rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789"] invertedSet]].location==NSNotFound &&
           value.integerValue>=30 && value.integerValue<=1000)return YES;
    }
    return NO;
}
#endif
void host_perf_reports_graphics(const char *json) {
#if HALO_PERF_REPORTS
    if(!worker || !json)return;uint64_t captured_frame=dispatch_get_specific(&worker_key)?graphics_frame_override:host_perf_hooks_frame_id();size_t length=strnlen(json,16385);if(length>16384)return;
    NSData *bytes=[NSData dataWithBytes:json length:length];id snapshot=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
    if(![snapshot isKindOfClass:NSDictionary.class] || ![snapshot[@"values"] isKindOfClass:NSDictionary.class])return;
    NSMutableDictionary *allowed=[NSMutableDictionary dictionary];NSArray *keys=@[@"display.renderer",@"display.render_height",@"display.output_height",@"display.render_scale",@"display.max_fps",@"display.frame_rate",@"display.frame_pacing",@"display.frames_in_flight",@"display.frame_spacing",@"display.frame_repeat",@"display.anisotropic_filtering",@"display.shadow_map_size",@"display.model_lod",@"display.anti_aliasing",@"display.foveation",@"display.effect_resolution",@"display.render_quality",@"display.resolution",@"display.resolution_scaling",@"display.upscaler",@"display.vsync",@"display.async_shaders",@"display.lod_scale",@"display.clutter_distance",@"display.compressed_textures",@"display.upscaled_textures",@"display.shadow_batching",@"display.decal_batching",@"display.immersive",@"display.stereo",@"display.interpolation",@"display.late_start",@"display.late_look",@"debug.metal_state_cache",@"debug.metal_specialize",@"debug.metal_vertex_bgra",@"debug.metal_base_vertex",@"debug.gpu_flush_draws",@"debug.dynamic_resolution_shadow",@"display.dynamic_resolution",@"display.dynamic_resolution_floor"];
    for(NSString *key in keys){id value=snapshot[@"values"][key];if(([value isKindOfClass:NSNumber.class] && isfinite([value doubleValue])) || ([value isKindOfClass:NSString.class] && graphics_string_allowed(key,value)))allowed[key]=value;}
    dispatch_block_t apply=^{if([graphics isEqual:allowed] && graphics_hash)return;graphics=[allowed copy];
        NSData *canonical=[NSJSONSerialization dataWithJSONObject:graphics options:NSJSONWritingSortedKeys error:nil];uint8_t digest[32];CC_SHA256(canonical.bytes,(CC_LONG)canonical.length,digest);NSMutableString *hash=[NSMutableString string];for(int i=0;i<32;i++)[hash appendFormat:@"%02x",digest[i]];graphics_hash=hash;if(!metadata)return;metadata[@"graphics"]=graphics;metadata[@"profileHash"]=hash;
        if(journal){NSData *epoch=[NSJSONSerialization dataWithJSONObject:@{@"frameID":@(captured_frame),@"context":@{@"graphics":graphics,@"profileHash":hash}} options:NSJSONWritingSortedKeys error:nil];append_chunk(3,epoch.bytes,epoch.length);}
    };if(dispatch_get_specific(&worker_key))apply();else dispatch_async(worker,apply);
#else
    (void)json;
#endif
}

void host_perf_reports_graphics_snapshot(const struct game_perf_graphics_snapshot *snapshot) {
#if HALO_PERF_REPORTS
    if(!snapshot || !snapshot->valid || snapshot->schema!=1 || snapshot->scene>2 || snapshot->count>128 || !atomic_load_explicit(&enabled,memory_order_relaxed))return;
    unsigned head=atomic_load_explicit(&graphics_head,memory_order_relaxed),tail=atomic_load_explicit(&graphics_tail,memory_order_acquire);
    if(head-tail>=2){atomic_fetch_add_explicit(&graphics_dropped,1,memory_order_relaxed);return;}
    graphics_queue[head%2].value=*snapshot;graphics_queue[head%2].frame_id=host_perf_hooks_frame_id();atomic_store_explicit(&graphics_head,head+1,memory_order_release);
#else
    (void)snapshot;
#endif
}
int host_perf_reports_enabled(void) {
#if HALO_PERF_REPORTS
    return atomic_load_explicit(&enabled,memory_order_relaxed);
#else
    return 0;
#endif
}
void host_perf_reports_finish(int interrupted) {
    host_perf_hooks_dynamic_flush();
#if HALO_PERF_REPORTS
    if(!worker)return;
    atomic_store(&suspended,true);atomic_store(&collecting,false);
    uint64_t deadline=clock_us()+100000;
    while(atomic_load(&appenders) && clock_us()<deadline)usleep(1000);
    BOOL quiescent=!atomic_load(&appenders);
    dispatch_sync(worker,^{
        host_report_delivery_cancel_performance();resume_play=NO;resume_ready=NO;
        if(quiescent)game_perf_end(recorder,clock_us(),interrupted?GAME_PERF_END_INTERRUPTED:GAME_PERF_END_NORMAL);
        drain();
        if(journal && quiescent)finish_report(interrupted!=0);
        else if(journal){fflush(journal);fsync(fileno(journal));checkpoint_state();}
    });
#else
    (void)interrupted;
#endif
}
int host_perf_reports_collecting(void) {
#if HALO_PERF_REPORTS
    return atomic_load_explicit(&collecting,memory_order_relaxed);
#else
    return 0;
#endif
}
