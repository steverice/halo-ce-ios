/* Apache-2.0. The render producer only copies into the bounded portable collector.
   All persistence, admission, compression and networking runs on one utility worker. */
#import "host_shader_upload.h"
#import "host_map_identity.h"
#import "host_benchmark_outbox.h"
#import "host_report_delivery.h"
#import <CommonCrypto/CommonDigest.h>
#include "host_shader_pack.h"
#include "host_shader_official_maps.h"
#include "host_data_updates.h"
#include "host_benchmark.h"
#include "../../runtime/shader_observations.h"
#include <TargetConditionals.h>
#include <zlib.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <unistd.h>
#if HALO_NATIVE_MAC
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#if HALO_SHADER_UPLOAD
#import <CloudKit/CloudKit.h>
#endif
#define ROOT @"shader-upload"
#define OUTBOX @"shader-upload/outbox"
#define DECODED_LIMIT (256*1024)
#define COMPRESSED_LIMIT (128*1024)
#if HALO_SHADER_UPLOAD
static NSString *sha(NSData *data) {
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(data.bytes,(CC_LONG)data.length,bytes);
    NSMutableString *result=[NSMutableString string];
    for(size_t i=0;i<sizeof(bytes);i++)[result appendFormat:@"%02x",bytes[i]];
    return result;
}
static NSData *bounded_read(NSString *path,NSUInteger limit) {
    NSNumber *size=[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil][NSFileSize];
    return size && size.unsignedLongLongValue<=limit ? [NSData dataWithContentsOfFile:path] : nil;
}
#endif
static BOOL hex(id text, NSUInteger length) {
    return [text isKindOfClass:NSString.class] && [text length]==length &&
        [text rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location==NSNotFound;
}
BOOL host_shader_upload_validate(NSDictionary *value) {
    NSSet *keys=[NSSet setWithArray:@[@"schema",@"nonce",@"mapFingerprint",@"mapKey",@"mapFormat",@"renderer",@"model",@"platform",@"appVersion",@"appBuild",@"guestSHA256",@"descriptorABI",@"shaderListID",@"descriptors",@"dependencies"]];
    if(![value isKindOfClass:NSDictionary.class] || ![[NSSet setWithArray:value.allKeys] isEqual:keys] ||
       (![value[@"schema"] isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value[@"schema"])==CFBooleanGetTypeID() || ![value[@"schema"] isEqual:@1]) || !hex(value[@"nonce"],32) || !hex(value[@"mapFingerprint"],64) ||
       !hex(value[@"guestSHA256"],64) || ![value[@"renderer"] isEqual:@"metal"] ||
       ![value[@"descriptorABI"] isEqual:@SHADER_OBSERVATION_ABI])return NO;
    NSString *key=[@"m_" stringByAppendingString:[value[@"mapFingerprint"] substringToIndex:60]];
    const char *alias=host_shader_official_alias([value[@"mapFingerprint"] UTF8String]);
    if(![value[@"mapKey"] isEqual:key] && !(alias && [value[@"mapKey"] isEqual:@(alias)]))return NO;
    for(NSString *field in @[@"mapFormat",@"model",@"platform",@"appVersion",@"appBuild",@"shaderListID"])
        if(![value[field] isKindOfClass:NSString.class] || ![value[field] length] || [value[field] length]>160 ||
           ![value[field] canBeConvertedToEncoding:NSASCIIStringEncoding])return NO;
    NSString *format=value[@"mapFormat"], *list=value[@"shaderListID"];
    NSRegularExpression *format_pattern=[NSRegularExpression regularExpressionWithPattern:@"^cache-(0|[1-9][0-9]{0,9})$" options:0 error:nil];
    NSRegularExpression *list_pattern=[NSRegularExpression regularExpressionWithPattern:@"^shader-lists-2:(bundled|0|[1-9][0-9]{0,19}):[0-9a-f]{64}$" options:0 error:nil];
    if(![format_pattern firstMatchInString:format options:0 range:NSMakeRange(0,format.length)] ||
       [[format substringFromIndex:6] longLongValue]>UINT32_MAX ||
       ![list_pattern firstMatchInString:list options:0 range:NSMakeRange(0,list.length)] ||
       ![@[@"ios",@"tvos",@"visionos",@"macos"] containsObject:value[@"platform"]])return NO;
    NSArray *misses=value[@"descriptors"], *dependencies=value[@"dependencies"];
    if(![misses isKindOfClass:NSArray.class] || !misses.count || misses.count>32 ||
       ![dependencies isKindOfClass:NSArray.class] || dependencies.count>64)return NO;
    NSMutableSet *unique=[NSMutableSet set]; NSMutableString *text=[NSMutableString string];
    for(NSArray *lines in @[misses,dependencies]) {
        for(id line in lines)if(![line isKindOfClass:NSString.class])return NO;
        if(![lines isEqual:[lines sortedArrayUsingSelector:@selector(compare:)]])return NO;
        for(id line in lines) {
            if(![line isKindOfClass:NSString.class] || ![line length] || [line length]>=SHADER_OBSERVATION_LINE_MAX ||
               [line rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location!=NSNotFound || [unique containsObject:line])return NO;
            [unique addObject:line]; [text appendFormat:@"%@\n",line];
        }
    }
    NSData *raw=[text dataUsingEncoding:NSUTF8StringEncoding]; char *merged=NULL; size_t size=0;
    int valid=host_shader_list_merge(raw.bytes,raw.length,NULL,0,&merged,&size); free(merged); return valid!=0;
}
NSData *host_shader_upload_gzip(NSData *data) {
    if(!data || data.length>DECODED_LIMIT)return nil;
    NSMutableData *out=[NSMutableData dataWithLength:compressBound((uLong)data.length)+32]; z_stream stream={0};
    if(deflateInit2(&stream,Z_BEST_COMPRESSION,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)!=Z_OK)return nil;
    stream.next_in=(Bytef *)data.bytes;stream.avail_in=(uInt)data.length;
    stream.next_out=out.mutableBytes;stream.avail_out=(uInt)out.length;
    int result=deflate(&stream,Z_FINISH);out.length=stream.total_out;deflateEnd(&stream);
    return result==Z_STREAM_END && out.length<=COMPRESSED_LIMIT ? out : nil;
}
NSTimeInterval host_shader_upload_retry(NSUInteger attempt,NSTimeInterval delay,double jitter) {
    const double steps[]={60,300,1800,7200,86400};
    return MAX(delay,steps[MIN(attempt,(NSUInteger)4)]*(1+MAX(0,MIN(jitter,1))*.2));
}
#if HALO_SHADER_UPLOAD
static dispatch_queue_t worker;
static dispatch_source_t timer;
static struct shader_observation_collector collector;
/* Slots are immutable after publication and never reused while the producer lives. */
static struct { struct shader_observation_generation value; atomic_int ready; } generations[128];
static atomic_uint token_count;
static atomic_int enabled, menu, suspended, runtime_suppressed;
static NSMutableDictionary *groups, *state;
static sqlite3 *membership_db;
static NSString *guest;
static CKOperation *inflight;
#if HALO_SHADER_UPLOAD_TESTING
static NSInteger test_outcome=-1;
static NSUInteger test_attempts;
static void (^test_completion)(BOOL,NSError *);
static dispatch_block_t test_timeout, test_previous_timeout;
#endif
static NSUInteger active_attempt;
static BOOL eligible;
static NSTimeInterval idle_start;
static NSData *journal_written;   /* worker only: the journal bytes last synced to working.json */
static BOOL durable(NSData *data,NSString *path) {
    if(![data writeToFile:path options:NSDataWritingAtomic error:nil])return NO;
    int fd=open(path.fileSystemRepresentation,O_RDONLY);if(fd<0)return NO;
    BOOL ok=fsync(fd)==0;close(fd);
    fd=open(path.stringByDeletingLastPathComponent.fileSystemRepresentation,O_RDONLY);
    if(fd<0)return NO;ok=fsync(fd)==0 && ok;close(fd);return ok;
}
static BOOL save_state(void) { return durable([NSJSONSerialization dataWithJSONObject:state options:NSJSONWritingSortedKeys error:nil],[ROOT stringByAppendingPathComponent:@"state.json"]); }
static NSString *membership(NSDictionary *payload,NSString *line) {
    return [NSString stringWithFormat:@"%@/%@/metal/%@",payload[@"mapFingerprint"],payload[@"descriptorABI"],sha([line dataUsingEncoding:NSUTF8StringEncoding])];
}
static BOOL sent_contains(NSString *member) {
    sqlite3_stmt *query=NULL;BOOL found=NO;
    if(sqlite3_prepare_v2(membership_db,"SELECT 1 FROM sent WHERE key=?",-1,&query,NULL)==SQLITE_OK) {
        sqlite3_bind_text(query,1,member.UTF8String,-1,SQLITE_TRANSIENT);found=sqlite3_step(query)==SQLITE_ROW;
    }
    sqlite3_finalize(query);return found;
}
static BOOL bytes_contains(NSData *data,NSString *line) {
    NSData *wanted=[line dataUsingEncoding:NSUTF8StringEncoding];const char *bytes=data.bytes;size_t begin=0;
    for(size_t i=0;i<=data.length;i++)if(i==data.length || bytes[i]=='\n') {
        if(i-begin==wanted.length && !memcmp(bytes+begin,wanted.bytes,wanted.length))return YES;begin=i+1;
    }
    return NO;
}
static BOOL published_contains(NSString *key,NSString *line) {
    if(bytes_contains(host_shader_update_list(key.UTF8String),line))return YES;
    NSString *path=[[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"shader-lists"] stringByAppendingPathComponent:[key stringByAppendingPathExtension:@"txt"]];
    FILE *file=fopen(path.fileSystemRepresentation,"r");if(!file)return NO;
    char buffer[SHADER_OBSERVATION_LINE_MAX];BOOL found=NO;
    while(fgets(buffer,sizeof(buffer),file)){buffer[strcspn(buffer,"\r\n")]=0;if(!strcmp(buffer,line.UTF8String)){found=YES;break;}}
    fclose(file);return found;
}
static BOOL pending_contains(NSString *member) {
    for(NSString *path in host_typed_outbox_pending(OUTBOX,@"shader")) { @autoreleasepool {
        NSData *raw=bounded_read(path,DECODED_LIMIT);if(!raw || raw.length>DECODED_LIMIT)continue;
        NSDictionary *payload=[NSJSONSerialization JSONObjectWithData:raw options:0 error:nil];
        if(!host_shader_upload_validate(payload))continue;
        for(NSString *line in payload[@"descriptors"])if([membership(payload,line) isEqual:member])return YES;
    }}
    return NO;
}
static NSDictionary *metadata(struct shader_observation_generation *g) {
    char model[128]="Apple";size_t size=sizeof(model);
#if HALO_NATIVE_MAC
    sysctlbyname("hw.model",model,&size,NULL,0);
#else
    sysctlbyname("hw.machine",model,&size,NULL,0);
#endif
#if HALO_NATIVE_MAC
    NSString *platform=@"macos";
#elif TARGET_OS_TV
    NSString *platform=@"tvos";
#elif TARGET_OS_VISION
    NSString *platform=@"visionos";
#else
    NSString *platform=@"ios";
#endif
    return @{@"schema":@1,@"nonce":[[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString],
        @"mapFingerprint":@(g->fingerprint),@"mapKey":@(g->map_key),@"mapFormat":@(g->map_format),@"renderer":@"metal",
        @"model":@(model),@"platform":platform,@"appVersion":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"unknown",
        @"appBuild":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"unknown",@"guestSHA256":guest,
        @"descriptorABI":@SHADER_OBSERVATION_ABI,@"shaderListID":@(g->shader_list_id)};
}
static void trim_pending(void) {
    NSArray *paths=[host_typed_outbox_pending(OUTBOX,@"shader") sortedArrayUsingComparator:^NSComparisonResult(NSString *a,NSString *b) {
        return [[NSFileManager.defaultManager attributesOfItemAtPath:a error:nil][NSFileCreationDate] compare:[NSFileManager.defaultManager attributesOfItemAtPath:b error:nil][NSFileCreationDate]];
    }];
    NSUInteger size=0;for(NSString *path in paths)size+=[[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil][NSFileSize] unsignedIntegerValue];
    NSUInteger count=paths.count;
    for(NSString *path in paths) { if(size<=8*1024*1024 && count<=128)break;count--;size-=[[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil][NSFileSize] unsignedIntegerValue];[NSFileManager.defaultManager removeItemAtPath:path error:nil];[state[@"retries"] removeObjectForKey:path.lastPathComponent]; }
}
static void freeze(NSMutableDictionary *group) {
    NSMutableArray *remaining=[[group[@"misses"] allObjects] mutableCopy];
    for(NSString *line in [remaining copy])if(sent_contains(membership(group[@"metadata"],line)) ||
        pending_contains(membership(group[@"metadata"],line)) || published_contains(group[@"metadata"][@"mapKey"],line))[remaining removeObject:line];
    [remaining sortUsingSelector:@selector(compare:)];
    while(remaining.count && atomic_load(&enabled)) { @autoreleasepool {
        NSUInteger count=MIN(remaining.count,(NSUInteger)32); BOOL stored=NO;
        while(count) {
            NSArray *misses=[remaining subarrayWithRange:NSMakeRange(0,count)];NSMutableSet *deps=[NSMutableSet set];
            for(NSString *line in misses)[deps unionSet:group[@"links"][line] ?: [NSSet set]];
            [deps minusSet:[NSSet setWithArray:misses]];
            NSMutableDictionary *payload=[group[@"metadata"] mutableCopy];
            payload[@"nonce"]=[[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString];
            payload[@"descriptors"]=misses;
            payload[@"dependencies"]=[[deps allObjects] sortedArrayUsingSelector:@selector(compare:)];
            NSData *data=[NSJSONSerialization dataWithJSONObject:payload options:NSJSONWritingSortedKeys error:nil];
            if(host_shader_upload_validate(payload) && host_shader_upload_gzip(data)) {
                stored=host_typed_outbox_store(OUTBOX,data,@"shader",^BOOL(NSDictionary *v){return host_shader_upload_validate(v);},nil)!=nil;
                if(stored){[remaining removeObjectsInRange:NSMakeRange(0,count)];break;}
            }
            count/=2;
        }
        if(!stored)break;
    }}
    group[@"misses"]=[NSMutableSet setWithArray:remaining];
    NSMutableDictionary *links=group[@"links"];for(NSString *line in [links.allKeys copy])if(![remaining containsObject:line])[links removeObjectForKey:line];
    trim_pending();
}
static void checkpoint(BOOL force) {
    if(!atomic_load(&enabled))return;
    NSUInteger total=0;for(NSDictionary *g in groups.allValues)total+=[g[@"misses"] count];
    struct shader_observation_item item;NSUInteger drained=0;
    while(drained++<SHADER_OBSERVATION_CAPACITY && shader_observations_take(&collector,&item)) { @autoreleasepool {
        if(!item.generation.token || item.generation.token>128 || (!atomic_load_explicit(&generations[item.generation.token-1].ready,memory_order_acquire)))continue;
        char line[SHADER_OBSERVATION_LINE_MAX],v[SHADER_OBSERVATION_LINE_MAX],p[SHADER_OBSERVATION_LINE_MAX];
        if(!shader_observations_lines(&item.descriptor,line,v,p) || item.descriptor.warming)continue;
        NSString *key=[NSString stringWithFormat:@"%u",item.generation.token];NSMutableDictionary *group=groups[key];
        if(!group && groups.count>=4)continue;
        if(!group){group=[@{@"metadata":metadata(&item.generation),@"misses":[NSMutableSet set],@"links":[NSMutableDictionary dictionary]} mutableCopy];groups[key]=group;}
        NSString *canonical=@(line), *member=membership(group[@"metadata"],canonical);
        if(published_contains(@(item.generation.map_key),canonical) || sent_contains(member) || pending_contains(member))continue;
        if(total>=256)continue;
        if(![group[@"misses"] containsObject:canonical])total++;
        [group[@"misses"] addObject:canonical];NSMutableSet *deps=[NSMutableSet set];if(*v)[deps addObject:@(v)];if(*p)[deps addObject:@(p)];
        if(item.descriptor.kind==SHADER_OBSERVATION_PIPELINE)group[@"links"][canonical]=deps;
    }}
    @autoreleasepool {
    NSMutableDictionary *journal=[NSMutableDictionary dictionary];
    for(NSString *key in groups) {
        NSDictionary *g=groups[key];NSMutableDictionary *links=[NSMutableDictionary dictionary];
        for(NSString *line in g[@"links"])links[line]=[g[@"links"][line] allObjects];
        journal[key]=@{@"metadata":g[@"metadata"],@"misses":[g[@"misses"] allObjects],@"links":links};
    }
    NSData *journal_bytes=[NSJSONSerialization dataWithJSONObject:journal options:NSJSONWritingSortedKeys error:nil];
    /* The worker wakes every minute during play; it rewrites and syncs the journal only when it changed. */
    if(journal_bytes.length<=256*1024 && ![journal_bytes isEqual:journal_written] &&
       durable(journal_bytes,[ROOT stringByAppendingPathComponent:@"working.json"]))journal_written=journal_bytes;
    }
    BOOL idle=atomic_load(&menu) && !atomic_load(&runtime_suppressed) && !host_benchmark_running();
    if(!idle)idle_start=0;else if(!idle_start)idle_start=NSDate.date.timeIntervalSince1970;
    for(NSString *key in [groups.allKeys copy]) {
        NSMutableDictionary *g=groups[key];
        if(force || total>=256 || (idle_start && NSDate.date.timeIntervalSince1970-idle_start>=300))freeze(g);
        if(![g[@"misses"] count])[groups removeObjectForKey:key];
    }
}
static void acknowledge(NSString *path,NSDictionary *payload) {
    if(sqlite3_exec(membership_db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return;
    sqlite3_exec(membership_db,"DELETE FROM sent WHERE rowid NOT IN (SELECT rowid FROM sent ORDER BY age DESC LIMIT 5900)",NULL,NULL,NULL);
    BOOL committed=YES;
    for(NSString *line in payload[@"descriptors"]) {
        sqlite3_stmt *insert=NULL;
        if(sqlite3_prepare_v2(membership_db,"INSERT OR REPLACE INTO sent(key,age) VALUES(?,?)",-1,&insert,NULL)!=SQLITE_OK){committed=NO;break;}
        sqlite3_bind_text(insert,1,membership(payload,line).UTF8String,-1,SQLITE_TRANSIENT);sqlite3_bind_double(insert,2,NSDate.date.timeIntervalSince1970);
        if(sqlite3_step(insert)!=SQLITE_DONE)committed=NO;sqlite3_finalize(insert);
    }
    /* The database has a fixed page ceiling; retain recent membership after eviction. */
    if(committed)committed=sqlite3_exec(membership_db,"DELETE FROM sent WHERE rowid NOT IN (SELECT rowid FROM sent ORDER BY age DESC LIMIT 6000); COMMIT",NULL,NULL,NULL)==SQLITE_OK;
    if(!committed){sqlite3_exec(membership_db,"ROLLBACK",NULL,NULL,NULL);return;}
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:state options:NSJSONWritingSortedKeys error:nil];
    if(!durable(bytes,[ROOT stringByAppendingPathComponent:@"state.json"]))return;
    if(host_benchmark_outbox_acknowledge(path,nil)) {
        [NSFileManager.defaultManager removeItemAtPath:path error:nil];
        [NSFileManager.defaultManager removeItemAtPath:[path stringByAppendingPathExtension:@"sent"] error:nil];
    }
}
static void deliver(void) {
    if(inflight || !atomic_load(&enabled) || atomic_load(&runtime_suppressed) || atomic_load(&suspended) || !atomic_load(&menu) || host_benchmark_running())return;
    NSString *version=[NSString stringWithFormat:@"%@/%@",[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"], [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]];
    if([state[@"paused"] isEqual:version])return;
    NSTimeInterval now=NSDate.date.timeIntervalSince1970;
    long day=(long)(now/86400);
    if([state[@"day"] longValue]!=day){state[@"day"]=@(day);state[@"count"]=@0;state[@"bytes"]=@0;save_state();}
    if([state[@"count"] unsignedIntegerValue]>=4)return;
    for(NSString *path in host_typed_outbox_pending(OUTBOX,@"shader")) {
        NSData *raw=bounded_read(path,DECODED_LIMIT);
        NSDictionary *payload=raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil] : nil;
        if(!host_shader_upload_validate(payload))continue;
        BOOL redundant=YES;
        for(NSString *line in payload[@"descriptors"])if(!published_contains(payload[@"mapKey"],line) && !sent_contains(membership(payload,line))){redundant=NO;break;}
        if(redundant) { [NSFileManager.defaultManager removeItemAtPath:path error:nil];[state[@"retries"] removeObjectForKey:path.lastPathComponent];save_state();continue; }
        NSMutableDictionary *retry=state[@"retries"][path.lastPathComponent];
        if([retry[@"after"] doubleValue]>now)continue;
        NSData *compressed=host_shader_upload_gzip(raw);
        if(!compressed || compressed.length+[state[@"bytes"] unsignedIntegerValue]>512*1024)continue;
        NSString *record_name=path.lastPathComponent.stringByDeletingPathExtension;
        if(![record_name isEqual:[@"shader-" stringByAppendingString:sha(raw)]])continue;
        NSURL *asset=[NSURL fileURLWithPath:[[ROOT stringByAppendingPathComponent:@"temporary"] stringByAppendingPathComponent:[record_name stringByAppendingPathExtension:@"gz"]]];
        if(![compressed writeToURL:asset atomically:YES])return;
        if(!host_report_delivery_acquire()){[NSFileManager.defaultManager removeItemAtURL:asset error:nil];return;}
        NSUInteger operation_attempt=++active_attempt;
        CKRecordID *identifier=[[CKRecordID alloc] initWithRecordName:record_name];
        /* The collector keeps a local admission token; the shared transport owns CK operations. */
        inflight=[[CKModifyRecordsOperation alloc] initWithRecordsToSave:nil recordIDsToDelete:nil];
        BOOL shared_transport=YES;
#if HALO_SHADER_UPLOAD_TESTING
        shared_transport=test_outcome<0;
#endif
        /* Charge attempts durably, including a timed-out operation that might reach the server. */
        state[@"count"]=@([state[@"count"] unsignedIntegerValue]+1);
        state[@"bytes"]=@([state[@"bytes"] unsignedIntegerValue]+compressed.length);
        if(!save_state()){host_report_delivery_release();inflight=nil;[NSFileManager.defaultManager removeItemAtURL:asset error:nil];return;}
        __block BOOL finished=NO;
        void (^finish)(BOOL,NSError *)=^(BOOL success,NSError *error) {
            dispatch_async(worker,^{
                if(finished)return;finished=YES;if(!shared_transport)host_report_delivery_release();
                [NSFileManager.defaultManager removeItemAtURL:asset error:nil];
                if(!atomic_load(&enabled))return;
                if(operation_attempt==active_attempt)inflight=nil;
                if(success){acknowledge(path,payload);[state[@"retries"] removeObjectForKey:path.lastPathComponent];}
                else {
                    NSError *specific=error;
                    if(error.code==CKErrorPartialFailure)specific=error.userInfo[CKPartialErrorsByItemIDKey][identifier] ?: error;
                    if(specific.code==CKErrorPermissionFailure || specific.code==CKErrorServerRejectedRequest || specific.code==CKErrorInvalidArguments)
                        state[@"paused"]=version;
                    NSUInteger attempt=[retry[@"attempt"] unsignedIntegerValue];
                    double jitter=(double)arc4random_uniform(10000)/10000;
                    state[@"retries"][path.lastPathComponent]=@{@"attempt":@(attempt+1),@"after":@(NSDate.date.timeIntervalSince1970+host_shader_upload_retry(attempt,[specific.userInfo[CKErrorRetryAfterKey] doubleValue],jitter))};
                }
                save_state();
            });
        };
#if HALO_SHADER_UPLOAD_TESTING
        if(test_outcome>=0) {
            test_attempts++;
            if(test_outcome==3)test_completion=[finish copy];
            else finish(test_outcome==0,test_outcome==0?nil:[NSError errorWithDomain:CKErrorDomain code:test_outcome==2?CKErrorServerRejectedRequest:CKErrorNetworkFailure userInfo:nil]);
            dispatch_block_t deadline_block=^{if(!finished){if(operation_attempt==active_attempt){host_report_delivery_cancel_shader();[inflight cancel];inflight=nil;}finish(NO,[NSError errorWithDomain:CKErrorDomain code:CKErrorNetworkFailure userInfo:nil]);}};
            if(test_outcome==3)test_timeout=[deadline_block copy];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,12*NSEC_PER_SEC),worker,deadline_block);
        } else
#endif
        host_report_delivery_submit_shader(path,asset.path,payload,sha(raw),worker,finish);
        return;
    }
}

#endif
void host_shader_upload_start(const char *guest_sha256) {
#if HALO_SHADER_UPLOAD
    static dispatch_once_t once;dispatch_once(&once,^{
        worker=dispatch_queue_create("org.visr.shader-upload",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0));
        host_map_identity_init();
        eligible=YES;
#if TARGET_OS_SIMULATOR
        eligible=NO;
#endif
        if(getenv("HALO_RUNNER") || getenv("HALO_BENCHMARK_SUITE") || getenv("HALO_RENDER_REPLAY") || getenv("HALO_PROBE"))eligible=NO;
#if HALO_SHADER_UPLOAD_TESTING
        if(getenv("HALO_SHADER_UPLOAD_TEST_OVERRIDE"))eligible=YES;
#endif
        guest=guest_sha256 ? @(guest_sha256) : @"";if(!hex(guest,64))eligible=NO;
        shader_observations_init(&collector);groups=[NSMutableDictionary dictionary];
        /* Discard the retired preference; collection follows build and run eligibility. */
        [NSUserDefaults.standardUserDefaults removeObjectForKey:@"share_shader_improvements"];
        atomic_store(&enabled,eligible);
        [NSFileManager.defaultManager createDirectoryAtPath:OUTBOX withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *temporary=[ROOT stringByAppendingPathComponent:@"temporary"];
        [NSFileManager.defaultManager removeItemAtPath:temporary error:nil];
        [NSFileManager.defaultManager createDirectoryAtPath:temporary withIntermediateDirectories:YES attributes:nil error:nil];
        NSData *bytes=bounded_read([ROOT stringByAppendingPathComponent:@"state.json"],65536);
        state=bytes ? [NSJSONSerialization JSONObjectWithData:bytes options:NSJSONReadingMutableContainers error:nil] : nil;
        if(![state isKindOfClass:NSMutableDictionary.class])state=[NSMutableDictionary dictionary];
        [state removeObjectForKey:@"sent"];
        sqlite3_open([[ROOT stringByAppendingPathComponent:@"sent.sqlite"] fileSystemRepresentation],&membership_db);
        sqlite3_exec(membership_db,"PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA cache_size=-32; PRAGMA max_page_count=1024; CREATE TABLE IF NOT EXISTS sent(key TEXT PRIMARY KEY, age REAL)",NULL,NULL,NULL);
        if(![state[@"retries"] isKindOfClass:NSMutableDictionary.class])state[@"retries"]=[NSMutableDictionary dictionary];
#if HALO_NATIVE_MAC
        NSString *leave=NSApplicationWillResignActiveNotification,*resume=NSApplicationDidBecomeActiveNotification;
#else
        NSString *leave=UIApplicationWillResignActiveNotification,*resume=UIApplicationDidBecomeActiveNotification;
#endif
        [NSNotificationCenter.defaultCenter addObserverForName:leave object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;host_shader_upload_suspend();}];
        [NSNotificationCenter.defaultCenter addObserverForName:resume object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;atomic_store(&suspended,0);}];
        dispatch_async(worker,^{
            /* Repair an acknowledgment interrupted after membership commit. */
            for(NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:OUTBOX error:nil])if([name hasSuffix:@".json.sent"]){
                NSString *receipt=[OUTBOX stringByAppendingPathComponent:name];
                [NSFileManager.defaultManager removeItemAtPath:receipt.stringByDeletingPathExtension error:nil];
                [NSFileManager.defaultManager removeItemAtPath:receipt error:nil];
            }
            trim_pending();
            for(NSString *name in [state[@"retries"] allKeys])if(![NSFileManager.defaultManager fileExistsAtPath:[OUTBOX stringByAppendingPathComponent:name]])[state[@"retries"] removeObjectForKey:name];
            save_state();
        });
        dispatch_async(worker,^{
            NSData *journal_bytes=bounded_read([ROOT stringByAppendingPathComponent:@"working.json"],256*1024);
            NSDictionary *journal=journal_bytes && journal_bytes.length<=256*1024 ? [NSJSONSerialization JSONObjectWithData:journal_bytes options:NSJSONReadingMutableContainers error:nil] : nil;
            NSUInteger restored_count=0;
            if(atomic_load(&enabled) && [journal isKindOfClass:NSDictionary.class] && journal.count<=4)for(NSString *key in journal) {
                NSDictionary *j=journal[key];if(![key isKindOfClass:NSString.class] || ![j isKindOfClass:NSDictionary.class])continue;
                NSMutableDictionary *links=[NSMutableDictionary dictionary];
                if(![j[@"metadata"] isKindOfClass:NSDictionary.class] || ![j[@"links"] isKindOfClass:NSDictionary.class] || [j[@"links"] count]>256 || ![j[@"misses"] isKindOfClass:NSArray.class] || [j[@"misses"] count]>256)continue;
                BOOL valid=YES;
                for(id line in j[@"misses"])if(![line isKindOfClass:NSString.class] || [line length]>=SHADER_OBSERVATION_LINE_MAX)valid=NO;
                for(id line in j[@"links"]) {
                    NSArray *dependencies=j[@"links"][line];
                    if(![line isKindOfClass:NSString.class] || ![dependencies isKindOfClass:NSArray.class] || dependencies.count>2){valid=NO;break;}
                    for(id dependency in dependencies)if(![dependency isKindOfClass:NSString.class] || [dependency length]>=SHADER_OBSERVATION_LINE_MAX)valid=NO;
                    if(valid)links[line]=[NSMutableSet setWithArray:dependencies];
                }
                if(!valid)continue;
                NSArray *all=j[@"misses"];
                for(NSUInteger offset=0;offset<all.count && valid;offset+=32) { @autoreleasepool {
                    NSArray *chunk=[all subarrayWithRange:NSMakeRange(offset,MIN((NSUInteger)32,all.count-offset))];
                    NSMutableDictionary *candidate=[j[@"metadata"] mutableCopy];candidate[@"descriptors"]=[chunk sortedArrayUsingSelector:@selector(compare:)];
                    NSMutableSet *dependencies=[NSMutableSet set];for(NSString *line in chunk)[dependencies unionSet:links[line] ?: [NSSet set]];
                    [dependencies minusSet:[NSSet setWithArray:chunk]];
                    candidate[@"dependencies"]=[[dependencies allObjects] sortedArrayUsingSelector:@selector(compare:)];
                    valid=host_shader_upload_validate(candidate);
                }}
                if(!valid || restored_count+[all count]>256)continue;restored_count+=[all count];
                groups[[@"recovered-" stringByAppendingString:key]]=[@{@"metadata":j[@"metadata"],@"misses":[NSMutableSet setWithArray:j[@"misses"]],@"links":links} mutableCopy];
            }
        });
        timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,worker);
        dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,60*NSEC_PER_SEC),60*NSEC_PER_SEC,NSEC_PER_SEC);
        dispatch_source_set_event_handler(timer,^{@autoreleasepool{checkpoint(NO);deliver();
        }});dispatch_resume(timer);
    });
#else
    (void)guest_sha256;
#endif
}
void host_shader_upload_suspend(void) {
#if HALO_SHADER_UPLOAD
    if(!worker)return;atomic_store(&suspended,1);dispatch_async(worker,^{checkpoint(YES);host_report_delivery_cancel_shader();[inflight cancel];});
#endif
}
uint32_t host_shader_observation_activate(const char *name,int suppressed,char *key,uint32_t key_size) {
    if(key && key_size)*key=0;
#if HALO_SHADER_UPLOAD
    if(!worker)return 0;
    if(!name || !*name){atomic_store(&menu,0);atomic_store(&runtime_suppressed,1);dispatch_async(worker,^{host_report_delivery_cancel_shader();[inflight cancel];});return 0;}
    atomic_store(&runtime_suppressed,suppressed!=0);
    NSString *path=@(name);
    if(![path.pathExtension isEqual:@"map"]){atomic_store(&menu,0);atomic_store(&runtime_suppressed,1);dispatch_async(worker,^{host_report_delivery_cancel_shader();[inflight cancel];});return 0;}
    atomic_store(&menu,[path.lastPathComponent.stringByDeletingPathExtension isEqual:@"ui"]);
    dispatch_async(worker,^{checkpoint(YES);if(!atomic_load(&menu) || atomic_load(&runtime_suppressed)){host_report_delivery_cancel_shader();[inflight cancel];}});
    const char *stock_name=host_shader_warming_alias(NULL,path.lastPathComponent.stringByDeletingPathExtension.UTF8String);
    if(stock_name && key && key_size)snprintf(key,key_size,"%s",stock_name);
    /* Activation is a loading boundary; complete cold identities before warmup. */
    NSDictionary *identity=host_map_identity_resolve(name);if(!identity)return 0;
    const char *trusted_alias=host_shader_warming_alias([identity[@"fingerprint"] UTF8String],stock_name);
    NSString *trusted_key=trusted_alias ? @(trusted_alias) : [@"m_" stringByAppendingString:[identity[@"fingerprint"] substringToIndex:60]];
    if(key && key_size)snprintf(key,key_size,"%s",trusted_key.UTF8String);
    if(suppressed || !atomic_load(&enabled))return 0;
    unsigned index=atomic_fetch_add(&token_count,1);if(index>=128)return 0;
    struct shader_observation_generation *g=&generations[index].value;g->token=index+1;
    snprintf(g->fingerprint,sizeof(g->fingerprint),"%s",[identity[@"fingerprint"] UTF8String]);
    const char *alias=host_shader_official_alias(g->fingerprint);
    if(alias)snprintf(g->map_key,sizeof(g->map_key),"%s",alias);else snprintf(g->map_key,sizeof(g->map_key),"m_%.60s",g->fingerprint);
    snprintf(g->map_format,sizeof(g->map_format),"%s",[identity[@"format"] UTF8String]);
    snprintf(g->guest_sha256,sizeof(g->guest_sha256),"%s",guest.UTF8String);
    NSDictionary *feed=host_data_updates_metadata()[@"shaders"];
    if([feed[@"effective_bundled_fallback"] boolValue])return 0;
    NSString *hash=feed[@"sha256"];if(!hex(hash,64))return 0;
    NSString *list=[NSString stringWithFormat:@"shader-lists-2:%@:%@",[feed[@"bundled"] boolValue] ? @"bundled" : [feed[@"sequence"] description],hash];
    snprintf(g->shader_list_id,sizeof(g->shader_list_id),"%s",list.UTF8String);
    atomic_store_explicit(&generations[index].ready,1,memory_order_release);
    return index+1;
#else
    (void)name;(void)suppressed;return 0;
#endif
}
void host_shader_observation_append(uint32_t token,const struct shader_observation *descriptor) {
#if HALO_SHADER_UPLOAD
    if(!atomic_load(&enabled) || !token || token>128 || (!atomic_load_explicit(&generations[token-1].ready,memory_order_acquire)))return;
    shader_observations_append(&collector,&generations[token-1].value,descriptor);
#else
    (void)token;(void)descriptor;
#endif
}
int host_shader_observation_recording(void) {
#if HALO_SHADER_UPLOAD
    return eligible;
#else
    return 0;
#endif
}
#if HALO_SHADER_UPLOAD && HALO_SHADER_UPLOAD_TESTING
void host_shader_upload_test_barrier(int force) {
    host_map_identity_wait();dispatch_sync(worker,^{checkpoint(force!=0);});
}

void host_shader_upload_test_acknowledge(NSString *path) {
    dispatch_sync(worker,^{NSDictionary *payload=[NSJSONSerialization JSONObjectWithData:bounded_read(path,DECODED_LIMIT) options:0 error:nil];acknowledge(path,payload);});
}
BOOL host_shader_upload_test_sent(NSDictionary *payload,NSString *line) {
    __block BOOL result;dispatch_sync(worker,^{result=sent_contains(membership(payload,line));});return result;
}
void host_shader_upload_test_send(int outcome,int advance) {
    dispatch_sync(worker,^{
        atomic_store(&menu,1);atomic_store(&suspended,0);test_outcome=outcome;
        if(advance)for(NSString *key in [state[@"retries"] allKeys])state[@"retries"][key]=@{@"attempt":state[@"retries"][key][@"attempt"],@"after":@0};
        deliver();
    });dispatch_sync(worker,^{});
}
NSUInteger host_shader_upload_test_attempts(void) {__block NSUInteger result;dispatch_sync(worker,^{result=test_attempts;});return result;}
void host_shader_upload_test_complete(void) {dispatch_sync(worker,^{if(test_completion){test_completion(YES,nil);test_completion=nil;}});dispatch_sync(worker,^{});}
void host_shader_upload_test_timeout(void) {dispatch_sync(worker,^{test_timeout();});dispatch_sync(worker,^{});}
void host_shader_upload_test_archive_timeout(void) {dispatch_sync(worker,^{test_previous_timeout=[test_timeout copy];});}
BOOL host_shader_upload_test_old_timeout(void) {__block BOOL result;dispatch_sync(worker,^{test_previous_timeout();result=inflight!=nil;test_previous_timeout=nil;});dispatch_sync(worker,^{});return result;}
void host_shader_upload_test_trim(void) {dispatch_sync(worker,^{trim_pending();});}
#endif

void host_shader_upload_prepare(void) {
#if HALO_SHADER_UPLOAD
    if(!worker || !eligible)return;
    /* Loading is outside shader hooks. Complete UI identity before the guest activates it. */
    host_map_identity_prepare_ui();
#endif
}
NSDictionary *host_shader_upload_map_identity(const char *name) {
    return host_map_identity_lookup(name);
}
