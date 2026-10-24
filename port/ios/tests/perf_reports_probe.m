/* Apache-2.0. Worker persistence probe; no app or CloudKit delivery. */
#import <Foundation/Foundation.h>
#include <assert.h>
#include "../host/host_perf_reports.m"
static uint64_t probe_frame_id=1;
uint64_t host_perf_hooks_frame_id(void) {return probe_frame_id;}
void host_perf_hooks_dynamic_flush(void) {}
int host_perf_hooks_dynamic_enabled(void) {return 1;}
void host_perf_hooks_dynamic_pause(void) {}
void host_perf_hooks_dynamic_ignore(unsigned reason) {(void)reason;}
void host_map_identity_init(void) {}
uint64_t host_perf_metal_dropped(void) {return 0;}
void host_perf_metal_join(game_perf_frame *frame) {(void)frame;}
void host_perf_hooks_gpu_join(game_perf_frame *frame) {frame->gpu_us=1234;frame->flags &= ~GAME_PERF_GPU_UNAVAILABLE;}
NSDictionary *host_shader_upload_map_identity(const char *name) {
    return name && !strcmp(name,"/resolved/custom.map")?@{@"fingerprint":@"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",@"format":@"cache-7"}:nil;
}
NSTimeInterval host_shader_upload_retry(NSUInteger attempt,NSTimeInterval cloud,double jitter) {(void)attempt;(void)jitter;return MAX(60,cloud);}
void host_device_profile_copy(struct host_profile *profile) {*profile=(struct host_profile){0};}
int host_benchmark_running(void) {return 0;}
int main(void) {@autoreleasepool {
    NSString *directory=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil]);
    assert([NSFileManager.defaultManager changeCurrentDirectoryPath:directory]);
    assert(host_report_delivery_acquire());assert(!host_report_delivery_acquire());host_report_delivery_release();assert(host_report_delivery_acquire());host_report_delivery_release();
    worker=dispatch_queue_create("org.visr.perf-privacy-probe",DISPATCH_QUEUE_SERIAL);dispatch_queue_set_specific(worker,&worker_key,&worker_key,NULL);
    host_perf_reports_graphics("{\"values\":{\"display.renderer\":\"/private/perf-secret\",\"display.frame_rate\":\"private-name\",\"display.render_height\":\"private-name\"}}");dispatch_sync(worker,^{});
    assert(!graphics[@"display.renderer"] && !graphics[@"display.frame_rate"] && !graphics[@"display.render_height"]);
    NSString *empty_graphics_hash=[graphics_hash copy];
    host_perf_reports_graphics("{\"values\":{\"display.renderer\":\"metal\",\"display.frame_rate\":\"60\"}}");dispatch_sync(worker,^{});
    assert([graphics[@"display.renderer"] isEqual:@"metal"] && [graphics[@"display.frame_rate"] isEqual:@"60"]);assert(![graphics_hash isEqual:empty_graphics_hash]);
    NSString *safe_graphics_hash=[graphics_hash copy];
    host_perf_reports_graphics("{\"values\":{\"display.renderer\":\"metal\",\"display.frame_rate\":\"60\",\"display.render_height\":\"/private/perf-secret\"}}");dispatch_sync(worker,^{});
    assert([graphics_hash isEqual:safe_graphics_hash]);assert(!graphics_string_allowed(@"display.frame_rate",@"29") && !graphics_string_allowed(@"display.frame_rate",@"1001"));
    [NSFileManager.defaultManager createDirectoryAtPath:OUTBOX withIntermediateDirectories:YES attributes:nil error:nil];
    metadata=[@{@"journalABI":@1,@"journalFrameSize":@(sizeof(game_perf_frame)),@"schema":@1,@"nonce":@"0123456789abcdef0123456789abcdef",@"model":@"probe",@"platform":@"macos",@"profileID":@"unknown",@"mapFingerprint":@"unknown",@"renderer":@"metal",@"mode":@"unknown",@"appVersion":@"1",@"appBuild":@"1",@"guestSHA256":@"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"} mutableCopy];
    initial_context=[metadata copy];assert(working_metadata_valid(metadata));
    NSMutableDictionary *old_abi=[metadata mutableCopy];old_abi[@"journalFrameSize"]=@1;assert(!working_metadata_valid(old_abi));
    journal=fopen([[ROOT stringByAppendingPathComponent:@"working.bin"] fileSystemRepresentation],"wb");assert(journal);
    game_perf_frame frame={.frame_id=1,.elapsed_us=100000,.interval_us=16667,.cpu_us=1000,.simulation_us=500,.network_us=100,.render_us=400,.gpu_us=GAME_PERF_UNAVAILABLE,.budget_us=16667};
    frame.drs=(game_perf_dynamic_resolution){.scene_us=9000,.upscale_us=800,.hud_us=300,.presented_us=100001,.presentation_us=16667,.late=0,.actual_lines=1080,.chosen_lines=880,.ceiling_lines=1080,.floor_lines=540,.ignore=4,.action=1,.thermal=2,.available=15};
    recorder=game_perf_create();assert(recorder);game_perf_set_enricher(recorder,enrich_frame,NULL);assert(game_perf_start(recorder,1));
    game_perf_frame previous_segment=frame;previous_segment.elapsed_us=99;start_us=100;atomic_store(&collecting,true);
    host_perf_reports_frame(&previous_segment);game_perf_status before={0};game_perf_get_status(recorder,&before);assert(before.frames_seen==0);
    atomic_store(&collecting,false);start_us=0;
    assert(game_perf_append_frame(recorder,&frame));frame.elapsed_us=200000;frame.frame_id=2;assert(game_perf_append_frame(recorder,&frame));
    frame.drs.available=0;frame.elapsed_us=300000;frame.frame_id=3;frame.flags=GAME_PERF_LOADING;frame.interval_us=60000;frame.cpu_us=500;assert(game_perf_append_frame(recorder,&frame));game_perf_end(recorder,300001,GAME_PERF_END_INTERRUPTED);drain();
    /* The graphics payload exceeds a frame struct, and epochs arrive out of order. */
    game_perf_event context={.elapsed_us=200000,.type=GAME_PERF_EVENT_CONTEXT,.flags=2,.value=2,.denominator=2};event_row(NULL,&context);
    NSData *epoch=[NSJSONSerialization dataWithJSONObject:@{@"frame":@0,@"context":@{@"graphics":@{@"syntheticPadding":[@"x" stringByPaddingToLength:7000 withString:@"x" startingAtIndex:0]}}} options:0 error:nil];append_chunk(3,epoch.bytes,epoch.length);
    /* An incomplete tail must not discard complete checksummed frames. */
    fwrite("GPJ1",1,4,journal);
    NSString *path=finish_report(YES);assert(path);
    NSDictionary *head=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] options:0 error:nil];assert([head[@"frameCount"] isEqual:@3]);
    assert(host_perf_validate_asset([path stringByAppendingPathExtension:@"gz"],head,head[@"payloadSHA256"]));
    NSMutableDictionary *changed=[head mutableCopy];changed[@"frameCount"]=@4;assert(!host_perf_validate_asset([path stringByAppendingPathExtension:@"gz"],changed,head[@"payloadSHA256"]));
    gzFile input=gzopen([[path stringByAppendingPathExtension:@"gz"] fileSystemRepresentation],"rb");assert(input);NSMutableData *decoded=[NSMutableData data];uint8_t bytes[4096];int n;while((n=gzread(input,bytes,sizeof(bytes)))>0)[decoded appendBytes:bytes length:n];assert(gzclose(input)==Z_OK);
    NSDictionary *value=[NSJSONSerialization JSONObjectWithData:decoded options:0 error:nil];assert(value);NSArray *drs=value[@"dynamicResolution"][@"frames"];assert(drs.count==3);
    assert([drs[0][@"frame"] isEqual:@0] && [drs[0][@"sceneUS"] isEqual:@9000] && [drs[0][@"chosenLines"] isEqual:@880] && [drs[0][@"actualLines"] isEqual:@1080]);
    assert([drs[1][@"frameID"] isEqual:@2] && [drs[1][@"presentedUS"] isEqual:@100001] && [drs[1][@"presentationUS"] isEqual:@16667] && [drs[1][@"late"] isEqual:@0]);
    assert(drs[2][@"sceneUS"]==NSNull.null && drs[2][@"action"]==NSNull.null && drs[2][@"presentedUS"]==NSNull.null);
    assert([value[@"dynamicResolution"][@"semantics"][@"gpuClockCheck"] isEqual:@"unvalidated_shadow_only"]);
    assert([value[@"interrupted"] boolValue]);assert([value[@"traceSummary"][@"play"][@"metrics"][@"cpu"][@"p99US"] isEqual:@1000]);assert([value[@"traceSummary"][@"play"][@"metrics"][@"interval"][@"p99US"] isEqual:@16667]);assert([value[@"traceSummary"][@"loading"][@"metrics"][@"interval"][@"p99US"] isEqual:@60000]);
    assert([[(NSArray *)value[@"epochs"] firstObject][@"frame"] isEqual:@0]);assert([[(NSArray *)value[@"epochs"] lastObject][@"frame"] isEqual:@1]);assert([value[@"summary"][@"play"][@"metrics"][@"gpu"][@"available"] isEqual:@2]);
    NSData *interval=[[NSData alloc] initWithBase64EncodedString:value[@"trace"][@"intervalUS"] options:0];assert(interval.length==12);
    assert(![NSFileManager.defaultManager fileExistsAtPath:[ROOT stringByAppendingPathComponent:@"working.bin"]]);
    const char *destination=getenv("HALO_PERF_PROBE_ASSET");if(destination){[NSFileManager.defaultManager removeItemAtPath:@(destination) error:nil];[NSFileManager.defaultManager removeItemAtPath:[@(destination) stringByAppendingString:@".metadata.json"] error:nil];[NSFileManager.defaultManager copyItemAtPath:[path stringByAppendingPathExtension:@"gz"] toPath:@(destination) error:nil];[NSFileManager.defaultManager copyItemAtPath:path toPath:[@(destination) stringByAppendingString:@".metadata.json"] error:nil];}
    metadata=[initial_context mutableCopy];metadata[@"nonce"]=@"1123456789abcdef0123456789abcdef";initial_context=[metadata copy];
    journal_bytes=0;sequence=0;rows=0;duration_us=0;lost_rows=0;lost_events=0;journal_events=0;omitted_begin_us=0;omitted_end_us=0;truncated=NO;trace_limit=MAX_TRACE_ROWS;
    journal=fopen([[ROOT stringByAppendingPathComponent:@"working.bin"] fileSystemRepresentation],"wb");assert(journal);assert(game_perf_start(recorder,1));
    frame=(game_perf_frame){.frame_id=1,.elapsed_us=100000,.interval_us=16667,.cpu_us=1000,.gpu_us=GAME_PERF_UNAVAILABLE,.budget_us=16667};
    assert(game_perf_append_frame(recorder,&frame));frame.frame_id=2;frame.elapsed_us=200000;assert(game_perf_append_frame(recorder,&frame));drain();
    duration_us=600000000;lost_rows=7;omitted_begin_us=300000000;omitted_end_us=600000000;truncated=YES;
    assert(!fflush(journal));assert(!fsync(fileno(journal)));assert(checkpoint_state());
    NSMutableDictionary *saved=bounded_json([ROOT stringByAppendingPathComponent:@"working.json"],65536);assert(saved);
    NSMutableDictionary *corrupt=[saved mutableCopy],*checkpoint=[saved[@"_checkpoint"] mutableCopy];checkpoint[@"summaryCRC"]=@([checkpoint[@"summaryCRC"] unsignedIntValue]^1u);corrupt[@"_checkpoint"]=checkpoint;assert(!restore_checkpoint(corrupt));
    corrupt=[saved mutableCopy];checkpoint=[saved[@"_checkpoint"] mutableCopy];checkpoint[@"durationUS"]=(id)NSNull.null;corrupt[@"_checkpoint"]=checkpoint;assert(!restore_checkpoint(corrupt));
    frame.frame_id=4;frame.elapsed_us=600000000;frame.cpu_us=2000;frame.interval_us=60000;assert(game_perf_append_frame(recorder,&frame));drain();
    assert(!fflush(journal));assert(!fsync(fileno(journal)));fclose(journal);journal=NULL;
    game_perf_destroy(recorder);recorder=game_perf_create();assert(recorder);duration_us=0;lost_rows=0;omitted_begin_us=0;omitted_end_us=0;truncated=NO;
    metadata=[saved mutableCopy];assert(restore_checkpoint(metadata));NSString *recovered=finish_report(YES);assert(recovered);
    NSDictionary *recovery=bounded_json(recovered,65536);assert([recovery[@"durationMS"] isEqual:@600000]);assert([recovery[@"droppedFrames"] isEqual:@7]);assert([recovery[@"truncated"] boolValue]);assert([recovery[@"summary"][@"play"][@"frames"] isEqual:@3]);assert(!recovery[@"_checkpoint"]);
    input=gzopen([[recovered stringByAppendingPathExtension:@"gz"] fileSystemRepresentation],"rb");assert(input);[decoded setLength:0];while((n=gzread(input,bytes,sizeof(bytes)))>0)[decoded appendBytes:bytes length:n];assert(gzclose(input)==Z_OK);NSDictionary *restored_value=[NSJSONSerialization JSONObjectWithData:decoded options:0 error:nil];assert([restored_value[@"windows"] count]>0);assert([restored_value[@"gaps"] count]>0);assert([restored_value[@"gaps"][0][@"beginUS"] isEqual:@300000000]);assert([restored_value[@"gaps"][0][@"endUS"] isEqual:@600000000]);
    /* Graphics snapshots published before the first drain retain absolute frame IDs. */
    metadata=[initial_context mutableCopy];metadata[@"nonce"]=@"2123456789abcdef0123456789abcdef";initial_context=[metadata copy];
    journal_bytes=0;sequence=0;rows=0;duration_us=0;lost_rows=0;lost_events=0;journal_events=0;omitted_begin_us=0;omitted_end_us=0;truncated=NO;trace_limit=MAX_TRACE_ROWS;recovered_summary_valid=NO;recovered_windows_active=NO;first_frame_id=17;
    journal=fopen([[ROOT stringByAppendingPathComponent:@"working.bin"] fileSystemRepresentation],"wb");assert(journal);assert(game_perf_start(recorder,1));
    probe_frame_id=500;host_perf_reports_graphics("{\"values\":{\"display.renderer\":\"metal\",\"display.frame_rate\":\"90\"}}");dispatch_sync(worker,^{});
    frame=(game_perf_frame){.frame_id=500,.elapsed_us=100000,.interval_us=16667,.cpu_us=1000,.gpu_us=GAME_PERF_UNAVAILABLE,.budget_us=16667};assert(game_perf_append_frame(recorder,&frame));
    probe_frame_id=501;host_perf_reports_graphics("{\"values\":{\"display.renderer\":\"metal\",\"display.frame_rate\":\"120\"}}");dispatch_sync(worker,^{});
    frame.frame_id=501;frame.elapsed_us=200000;assert(game_perf_append_frame(recorder,&frame));game_perf_end(recorder,200001,GAME_PERF_END_INTERRUPTED);drain();
    NSString *absolute_report=finish_report(YES);assert(absolute_report);input=gzopen([[absolute_report stringByAppendingPathExtension:@"gz"] fileSystemRepresentation],"rb");assert(input);[decoded setLength:0];while((n=gzread(input,bytes,sizeof(bytes)))>0)[decoded appendBytes:bytes length:n];assert(gzclose(input)==Z_OK);
    NSDictionary *absolute_value=[NSJSONSerialization JSONObjectWithData:decoded options:0 error:nil];NSArray *absolute_epochs=absolute_value[@"epochs"];assert(absolute_epochs.count==2);assert([absolute_epochs[0][@"frame"] isEqual:@0]);assert([absolute_epochs[1][@"frame"] isEqual:@1]);assert([absolute_epochs[0][@"context"][@"graphics"][@"display.frame_rate"] isEqual:@"90"]);assert([absolute_epochs[1][@"context"][@"graphics"][@"display.frame_rate"] isEqual:@"120"]);
    /* Resolve a loaded custom map after logical-name collection has already begun. */
    metadata=[initial_context mutableCopy];metadata[@"nonce"]=@"3123456789abcdef0123456789abcdef";metadata[@"mapFingerprint"]=@"unknown";initial_context=[metadata copy];
    journal_bytes=0;sequence=0;rows=0;first_frame_id=0;duration_us=0;lost_rows=0;lost_events=0;journal_events=0;omitted_begin_us=0;omitted_end_us=0;truncated=NO;trace_limit=MAX_TRACE_ROWS;recovered_summary_valid=NO;recovered_windows_active=NO;active_map=@"logical-custom";
    journal=fopen([[ROOT stringByAppendingPathComponent:@"working.bin"] fileSystemRepresentation],"wb");assert(journal);assert(game_perf_start(recorder,1));
    frame=(game_perf_frame){.frame_id=600,.elapsed_us=100000,.interval_us=16667,.cpu_us=1000,.gpu_us=GAME_PERF_UNAVAILABLE,.budget_us=16667};assert(game_perf_append_frame(recorder,&frame));drain();assert([metadata[@"mapFingerprint"] isEqual:@"unknown"]);
    NSString *identity_nonce=[metadata[@"nonce"] copy];uint64_t identity_rows=rows;
    host_perf_reports_identity("/resolved/custom.map");dispatch_sync(worker,^{});
    assert([metadata[@"nonce"] isEqual:identity_nonce] && rows==identity_rows);assert([metadata[@"mapFingerprint"] isEqual:@"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"]);assert([initial_context[@"mapFingerprint"] isEqual:@"unknown"]);
    frame.frame_id=601;frame.elapsed_us=200000;assert(game_perf_append_frame(recorder,&frame));game_perf_end(recorder,200001,GAME_PERF_END_INTERRUPTED);drain();
    NSString *identity_report=finish_report(YES);assert(identity_report);NSDictionary *identity_head=bounded_json(identity_report,65536);assert([identity_head[@"nonce"] isEqual:identity_nonce] && [identity_head[@"frameCount"] isEqual:@2]);
    input=gzopen([[identity_report stringByAppendingPathExtension:@"gz"] fileSystemRepresentation],"rb");assert(input);[decoded setLength:0];while((n=gzread(input,bytes,sizeof(bytes)))>0)[decoded appendBytes:bytes length:n];assert(gzclose(input)==Z_OK);
    NSDictionary *identity_value=[NSJSONSerialization JSONObjectWithData:decoded options:0 error:nil];NSArray *identity_epochs=identity_value[@"epochs"];assert(identity_epochs.count==2);assert([identity_epochs[0][@"context"][@"mapFingerprint"] isEqual:@"unknown"]);assert([identity_epochs[1][@"frame"] isEqual:@1]);assert([identity_epochs[1][@"context"][@"mapFingerprint"] isEqual:identity_head[@"mapFingerprint"]]);assert([[[NSString alloc] initWithData:decoded encoding:NSUTF8StringEncoding] rangeOfString:@"/resolved/custom.map"].location==NSNotFound);
    /* Context changes persist across dropped IDs; RTT correlation requires an exact row. */
    metadata=[initial_context mutableCopy];metadata[@"nonce"]=@"4123456789abcdef0123456789abcdef";initial_context=[metadata copy];
    journal_bytes=0;sequence=0;rows=0;first_frame_id=0;duration_us=0;lost_rows=0;lost_events=0;journal_events=0;omitted_begin_us=0;omitted_end_us=0;truncated=NO;trace_limit=MAX_TRACE_ROWS;recovered_summary_valid=NO;recovered_windows_active=NO;active_map=nil;
    journal=fopen([[ROOT stringByAppendingPathComponent:@"working.bin"] fileSystemRepresentation],"wb");assert(journal);assert(game_perf_start(recorder,1));
    frame=(game_perf_frame){.frame_id=500,.elapsed_us=100000,.interval_us=16667,.cpu_us=1000,.gpu_us=GAME_PERF_UNAVAILABLE,.budget_us=16667};assert(game_perf_append_frame(recorder,&frame));frame.frame_id=510;frame.elapsed_us=200000;assert(game_perf_append_frame(recorder,&frame));
    probe_frame_id=505;host_perf_reports_graphics("{\"values\":{\"display.renderer\":\"metal\",\"display.frame_rate\":\"240\"}}");dispatch_sync(worker,^{});
    context=(game_perf_event){.elapsed_us=150000,.type=GAME_PERF_EVENT_CONTEXT,.flags=4,.value=2,.denominator=505};assert(game_perf_append_event(recorder,&context));
    game_perf_event rtt={.elapsed_us=150000,.type=15,.value=10000,.denominator=505};assert(game_perf_append_event(recorder,&rtt));rtt.elapsed_us=200000;rtt.value=20000;rtt.denominator=510;assert(game_perf_append_event(recorder,&rtt));game_perf_end(recorder,200001,GAME_PERF_END_INTERRUPTED);drain();
    NSString *gap_report=finish_report(YES);assert(gap_report);input=gzopen([[gap_report stringByAppendingPathExtension:@"gz"] fileSystemRepresentation],"rb");assert(input);[decoded setLength:0];while((n=gzread(input,bytes,sizeof(bytes)))>0)[decoded appendBytes:bytes length:n];assert(gzclose(input)==Z_OK);
    NSDictionary *gap_value=[NSJSONSerialization JSONObjectWithData:decoded options:0 error:nil];NSArray *gap_epochs=gap_value[@"epochs"],*gap_samples=gap_value[@"networkSamples"];assert(gap_epochs.count==2);assert([gap_epochs[1][@"frame"] isEqual:@1]);assert([gap_epochs[1][@"context"][@"playerCount"] isEqual:@2]);assert([gap_epochs[1][@"context"][@"graphics"][@"display.frame_rate"] isEqual:@"240"]);assert(gap_samples.count==1 && [gap_samples[0][@"frame"] isEqual:@1] && [gap_samples[0][@"rttMS"] doubleValue]==20);
    game_perf_destroy(recorder);recorder=NULL;
    /* Config-only automated runs cannot retry old menu reports. Hold admission so
       even a failed guard cannot contact CloudKit during this behavior probe. */
    assert(host_report_delivery_acquire());atomic_store(&enabled,true);atomic_store(&suspended,false);
    delivery=[@{@"day":@0,@"attempts":@7,@"bytes":@99,@"retries":[NSMutableDictionary dictionary]} mutableCopy];
    host_perf_reports_activate("ui",1);dispatch_sync(worker,^{});assert(atomic_load(&menu) && atomic_load(&runtime_suppressed));
    NSDictionary *suppressed_state=[delivery copy];deliver();assert([delivery isEqual:suppressed_state]);
    host_perf_reports_activate("ui",0);dispatch_sync(worker,^{});assert(!atomic_load(&runtime_suppressed));deliver();
    assert([delivery[@"day"] longValue]!=0 && [delivery[@"attempts"] unsignedIntegerValue]==0 && [delivery[@"bytes"] unsignedLongLongValue]==0);
    host_report_delivery_release();atomic_store(&enabled,false);atomic_store(&menu,false);
    delivery=[NSMutableDictionary dictionary];assert(!working_metadata_valid(@{}));
    assert([[@"orphan" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:[ROOT stringByAppendingPathComponent:@"working.bin"] atomically:YES]);
    assert([[NSMutableData dataWithLength:65537] writeToFile:[ROOT stringByAppendingPathComponent:@"working.json"] atomically:YES]);assert(!bounded_json([ROOT stringByAppendingPathComponent:@"working.json"],65536));
    discard_orphaned_recording();assert([delivery[@"lostReports"] isEqual:@1]);assert(![NSFileManager.defaultManager fileExistsAtPath:[ROOT stringByAppendingPathComponent:@"working.bin"]]);
    [NSFileManager.defaultManager changeCurrentDirectoryPath:NSTemporaryDirectory()];[NSFileManager.defaultManager removeItemAtPath:directory error:nil];puts("performance reports probe passed");
}}
