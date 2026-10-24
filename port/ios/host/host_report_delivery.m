/* Apache-2.0. One typed CloudKit transport and in-flight lease for both families. */
#import "host_report_delivery.h"
#import "host_perf_validation.h"
#import "host_shader_upload.h"
#import <CommonCrypto/CommonDigest.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <stdatomic.h>
#include <zlib.h>
#if HALO_SHADER_UPLOAD || HALO_PERF_REPORTS
#import <CloudKit/CloudKit.h>
#endif
static atomic_bool busy;
BOOL host_report_delivery_acquire(void) {bool expected=false;return atomic_compare_exchange_strong(&busy,&expected,true);}
void host_report_delivery_release(void) {atomic_store(&busy,false);}
#if HALO_SHADER_UPLOAD || HALO_PERF_REPORTS
static dispatch_queue_t scheduler(void) {
    static dispatch_queue_t queue;static dispatch_once_t once;dispatch_once(&once,^{queue=dispatch_queue_create("org.visr.report-delivery",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0));});return queue;
}
/* Only the scheduler touches these objects; collectors never share CKOperation state. */
static CKOperation *current_operation;
static NSString *current_family;
static NSArray *fields(BOOL performance) {
    return performance?@[@"schema",@"durationMS",@"frameCount",@"model",@"platform",@"profileID",@"mapFingerprint",@"renderer",@"mode",@"appVersion",@"appBuild",@"guestSHA256"]:
        @[@"schema",@"mapFingerprint",@"mapKey",@"mapFormat",@"model",@"platform",@"renderer",@"appVersion",@"appBuild",@"guestSHA256",@"descriptorABI",@"shaderListID"];
}
static BOOL shader_asset_equal(NSString *path,NSDictionary *metadata,NSString *hash) {
#if HALO_SHADER_UPLOAD
    NSDictionary *attributes=path?[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil]:nil;
    if(![attributes[NSFileType] isEqual:NSFileTypeRegular] || [attributes[NSFileSize] unsignedLongLongValue]>128*1024)return NO;
    gzFile file=gzopen(path.fileSystemRepresentation,"rb");if(!file)return NO;NSMutableData *decoded=[NSMutableData data];uint8_t bytes[16384];int n;BOOL valid=YES;
    while((n=gzread(file,bytes,sizeof(bytes)))>0){if(decoded.length+(NSUInteger)n>256*1024){valid=NO;break;}[decoded appendBytes:bytes length:(NSUInteger)n];}
    if(n<0 || gzclose(file)!=Z_OK)valid=NO;
    if(!valid)return NO;uint8_t digest[32];CC_SHA256(decoded.bytes,(CC_LONG)decoded.length,digest);NSMutableString *actual=[NSMutableString string];for(int i=0;i<32;i++)[actual appendFormat:@"%02x",digest[i]];
    NSDictionary *payload=[NSJSONSerialization JSONObjectWithData:decoded options:0 error:nil];if(![actual isEqual:hash] || !host_shader_upload_validate(payload))return NO;
    for(NSString *key in fields(NO))if(![payload[key] isEqual:metadata[key]])return NO;
    return [payload[@"descriptors"] count]==[metadata[@"descriptors"] count];
#else
    (void)path;(void)metadata;(void)hash;return NO;
#endif
}
static BOOL asset_equal(NSString *path,NSDictionary *metadata,NSString *hash,BOOL performance) {
    if(performance) {
#if HALO_PERF_REPORTS
        return host_perf_validate_asset(path,metadata,hash);
#else
        return NO;
#endif
    }
    return shader_asset_equal(path,metadata,hash);
}
static void submit(NSString *path,NSString *asset,NSDictionary *metadata,NSString *hash,BOOL performance,NSString *container_id,dispatch_queue_t callback_queue,void (^completion)(BOOL,NSError *)) {
    dispatch_async(scheduler(),^{@autoreleasepool{
        CKContainer *container=[CKContainer containerWithIdentifier:container_id];NSString *family=performance?@"perf":@"shader";
        CKRecordID *identifier=[[CKRecordID alloc] initWithRecordName:path.lastPathComponent.stringByDeletingPathExtension];
        CKRecord *record=[[CKRecord alloc] initWithRecordType:performance?@"GamePerformanceReport":@"ShaderObservation" recordID:identifier];
        for(NSString *key in fields(performance))record[key]=metadata[key];
        if(!performance)record[@"descriptorCount"]=@([metadata[@"descriptors"] count]);
        record[@"payloadSHA256"]=hash;record[@"payload"]=[[CKAsset alloc] initWithFileURL:[NSURL fileURLWithPath:asset]];
        __block BOOL finished=NO;NSTimeInterval deadline=NSDate.date.timeIntervalSince1970+12;
        void (^finish)(BOOL,NSError *)=^(BOOL success,NSError *error){dispatch_async(scheduler(),^{if(finished)return;finished=YES;current_operation=nil;current_family=nil;host_report_delivery_release();dispatch_async(callback_queue,^{completion(success,error);});});};
        CKModifyRecordsOperation *operation=[[CKModifyRecordsOperation alloc] initWithRecordsToSave:@[record] recordIDsToDelete:nil];
        operation.atomic=YES;operation.savePolicy=CKRecordSaveIfServerRecordUnchanged;
        operation.configuration.timeoutIntervalForRequest=12;operation.configuration.timeoutIntervalForResource=12;
        current_operation=operation;current_family=family;
        operation.modifyRecordsCompletionBlock=^(NSArray *saved,NSArray *deleted,NSError *error){
            (void)saved;(void)deleted;NSError *specific=error.code==CKErrorPartialFailure?error.userInfo[CKPartialErrorsByItemIDKey][identifier]:error;
            if(specific.code!=CKErrorServerRecordChanged){finish(error==nil,specific?:error);return;}
            CKFetchRecordsOperation *fetch=[[CKFetchRecordsOperation alloc] initWithRecordIDs:@[identifier]];
            fetch.configuration.timeoutIntervalForRequest=12;fetch.configuration.timeoutIntervalForResource=12;
            fetch.fetchRecordsCompletionBlock=^(NSDictionary *records,NSError *failure){
                CKRecord *existing=records[identifier];CKAsset *remote=existing[@"payload"];
                BOOL equal=[remote isKindOfClass:CKAsset.class] && asset_equal(remote.fileURL.path,metadata,hash,performance);
                for(NSString *key in fields(performance))if(![existing[key] isEqual:metadata[key]])equal=NO;
                if(!performance && ![existing[@"descriptorCount"] isEqual:@([metadata[@"descriptors"] count])])equal=NO;
                finish(!failure && equal && [existing[@"payloadSHA256"] isEqual:hash],failure?:specific);
            };
            dispatch_async(scheduler(),^{if(!finished && NSDate.date.timeIntervalSince1970<deadline){current_operation=fetch;[container.publicCloudDatabase addOperation:fetch];}else [fetch cancel];});
        };
        [container.publicCloudDatabase addOperation:operation];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,12*NSEC_PER_SEC),scheduler(),^{if(!finished){[current_operation cancel];finish(NO,[NSError errorWithDomain:CKErrorDomain code:CKErrorNetworkFailure userInfo:nil]);}});
    }});
}
#endif
static void cancel_family(NSString *family) {
#if HALO_SHADER_UPLOAD || HALO_PERF_REPORTS
    dispatch_async(scheduler(),^{if([current_family isEqual:family])[current_operation cancel];});
#else
    (void)family;
#endif
}
void host_report_delivery_cancel_performance(void) {cancel_family(@"perf");}
void host_report_delivery_cancel_shader(void) {cancel_family(@"shader");}
void host_report_delivery_submit(NSString *path,NSString *asset,NSDictionary *metadata,NSString *hash,dispatch_queue_t queue,void (^completion)(BOOL,NSError *)) {
#if HALO_PERF_REPORTS
    submit(path,asset,metadata,hash,YES,@HALO_PERF_CLOUD,queue,completion);
#else
    (void)path;(void)asset;(void)metadata;(void)hash;(void)queue;host_report_delivery_release();completion(NO,nil);
#endif
}
void host_report_delivery_submit_shader(NSString *path,NSString *asset,NSDictionary *metadata,NSString *hash,dispatch_queue_t queue,void (^completion)(BOOL,NSError *)) {
#if HALO_SHADER_UPLOAD
    submit(path,asset,metadata,hash,NO,@HALO_SHADER_CLOUD,queue,completion);
#else
    (void)path;(void)asset;(void)metadata;(void)hash;(void)queue;host_report_delivery_release();completion(NO,nil);
#endif
}
