#import <Foundation/Foundation.h>
#import "host_shader_upload.h"
#import "host_benchmark_outbox.h"
#include "../../runtime/shader_observations.h"
#include <assert.h>
#include <unistd.h>
void host_shader_upload_test_barrier(int force);
void host_shader_upload_test_opt_out(void);
void host_shader_upload_test_acknowledge(NSString *path);
void host_shader_upload_test_trim(void);
void host_shader_upload_test_opt_in(void);
void host_shader_upload_test_late_activation(uint32_t token);
void host_shader_upload_test_late_append(uint32_t token,const struct shader_observation *descriptor);
void host_shader_upload_test_archive_timeout(void);
BOOL host_shader_upload_test_old_timeout(void);
void host_shader_upload_test_send(int outcome,int advance);
NSUInteger host_shader_upload_test_attempts(void);
void host_shader_upload_test_complete(void);
BOOL host_shader_upload_test_sent(NSDictionary *payload,NSString *line);
int host_benchmark_running(void){return 0;}
NSData *host_shader_update_list(const char *name){(void)name;return nil;}
static BOOL fallback;
NSDictionary *host_data_updates_metadata(void) {return @{@"shaders":@{@"effective_bundled_fallback":@(fallback),@"bundled":@YES,@"sha256":@"1111111111111111111111111111111111111111111111111111111111111111"}};}
static const char *guest="0000000000000000000000000000000000000000000000000000000000000000";
int main(int argc,const char **argv) { @autoreleasepool {
    assert(argc==3);assert(chdir(argv[1])==0);NSString *mode=@(argv[2]);fallback=[mode isEqual:@"fallback"];
    setenv("HALO_SHADER_UPLOAD_TEST_OVERRIDE","1",1);
    [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"share_shader_improvements"];
    host_shader_upload_start(guest);host_shader_upload_test_barrier(0);
    char key[64];uint32_t token=host_shader_observation_activate("maps/sample.map",0,key,sizeof(key));
    host_shader_upload_test_barrier(0);
    if(!token)token=host_shader_observation_activate("maps/sample.map",0,key,sizeof(key));
    if(fallback){assert(!token && *key);host_shader_upload_test_opt_out();return 0;}
    assert(token && !strncmp(key,"m_",2));
    struct shader_observation event={.kind=SHADER_OBSERVATION_VERTEX,.vertex_hash=1,.metal=1};
    if([mode isEqual:@"activation-race"]) {
        host_shader_upload_test_opt_out();host_shader_upload_test_late_activation(token);
        host_shader_upload_test_opt_in();host_shader_observation_append(token,&event);host_shader_upload_test_barrier(1);
        assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==0);
        host_shader_upload_test_opt_out();
    } else if([mode isEqual:@"producer-race"]) {
        host_shader_upload_test_opt_out();host_shader_upload_test_late_append(token,&event);
        host_shader_upload_test_opt_in();host_shader_upload_test_barrier(1);
        assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==0);
        host_shader_upload_test_opt_out();
    } else if([mode isEqual:@"timeout-race"]) {
        event.vertex_hash=2;host_shader_observation_append(token,&event);host_shader_upload_test_barrier(1);
        host_shader_upload_test_send(3,1);host_shader_upload_test_archive_timeout();host_shader_upload_test_opt_out();
        host_shader_upload_test_opt_in();token=host_shader_observation_activate("maps/sample.map",0,key,sizeof(key));assert(token);host_shader_observation_append(token,&event);host_shader_upload_test_barrier(1);
        host_shader_upload_test_send(3,1);assert(host_shader_upload_test_attempts()==2);
        assert(host_shader_upload_test_old_timeout());host_shader_upload_test_complete();
        assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==0);
        host_shader_upload_test_opt_out();
    } else if([mode isEqual:@"lateimport"]) {
        NSData *late=[@"headlate imported campaign cache bytes" dataUsingEncoding:NSUTF8StringEncoding];
        assert([late writeToFile:@"maps/new.map" atomically:YES]);
        uint32_t next=host_shader_observation_activate("maps/new.map",0,key,sizeof(key));assert(next && *key);
        assert(host_shader_observation_activate("",0,key,sizeof(key))==0 && !*key);
        host_shader_upload_test_opt_out();
    } else if([mode isEqual:@"pixelpersist"]) {
        event.kind=SHADER_OBSERVATION_PIXEL;event.pixel_size=252;
        for(unsigned i=0;i<200;i++){
            event.pixel[0]=(uint8_t)i;event.pixel_hash=UINT64_C(14695981039346656037);
            for(unsigned j=0;j<252;j++){event.pixel_hash^=event.pixel[j];event.pixel_hash*=UINT64_C(1099511628211);}
            host_shader_observation_append(token,&event);
        }
        host_shader_upload_test_barrier(0);
        assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==0);
        assert([[NSFileManager.defaultManager attributesOfItemAtPath:@"shader-upload/working.json" error:nil][NSFileSize] unsignedIntegerValue]<=256*1024);
    } else if([mode isEqual:@"pixelrecover"]) {
        host_shader_upload_test_barrier(1);NSArray *pending=host_typed_outbox_pending(@"shader-upload/outbox",@"shader");
        assert(pending.count==7);NSUInteger count=0;
        for(NSString *path in pending){NSDictionary *p=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] options:0 error:nil];count+=[p[@"descriptors"] count];assert([p[@"dependencies"] count]==0 && host_shader_upload_validate(p));}
        assert(count==200);host_shader_upload_test_opt_out();
    } else if([mode isEqual:@"threshold"]) {
        for(unsigned i=0;i<256;i++){event.vertex_hash=i+1;host_shader_observation_append(token,&event);}
        host_shader_upload_test_barrier(0);
        NSArray *pending=host_typed_outbox_pending(@"shader-upload/outbox",@"shader");assert(pending.count==8);
        NSMutableSet *nonces=[NSMutableSet set];
        NSUInteger count=0;for(NSString *path in pending){NSDictionary *p=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] options:0 error:nil];count+=[p[@"descriptors"] count];[nonces addObject:p[@"nonce"]];assert(host_shader_upload_validate(p));}
        assert(count==256 && nonces.count==8);host_shader_upload_test_opt_out();
    } else if([mode isEqual:@"persist"]) {
        host_shader_observation_append(token,&event);host_shader_upload_test_barrier(0);
        assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==0);
        assert([NSFileManager.defaultManager fileExistsAtPath:@"shader-upload/working.json"]);
    } else if([mode isEqual:@"recover"]) {
        host_shader_upload_test_barrier(1);
        NSArray *pending=host_typed_outbox_pending(@"shader-upload/outbox",@"shader");assert(pending.count==1);
        NSString *path=pending.firstObject;NSData *raw=[NSData dataWithContentsOfFile:path];
        NSDictionary *payload=[NSJSONSerialization JSONObjectWithData:raw options:0 error:nil];assert(host_shader_upload_validate(payload));
        assert([payload[@"descriptors"] isEqual:@[@"vs 0000000000000001 0 0"]]);
        assert(host_shader_upload_gzip(raw).length<128*1024);
        host_shader_upload_test_acknowledge(path);assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==0);
        assert(host_shader_upload_test_sent(payload,@"vs 0000000000000001 0 0"));
        NSMutableDictionary *hostile=[payload mutableCopy];hostile[@"descriptors"]=@[@1];assert(!host_shader_upload_validate(hostile));
        hostile=[payload mutableCopy];hostile[@"schema"]=@YES;assert(!host_shader_upload_validate(hostile));
    } else if([mode isEqual:@"offline"] || [mode isEqual:@"quota"] || [mode isEqual:@"race"] || [mode isEqual:@"schema"]) {
        event.vertex_hash=2;host_shader_observation_append(token,&event);host_shader_upload_test_barrier(1);
        assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==1);
        if([mode isEqual:@"race"]) {
            host_shader_upload_test_send(3,1);assert(host_shader_upload_test_attempts()==1);
            host_shader_upload_test_opt_out();host_shader_upload_test_complete();
            assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==0);
        } else if([mode isEqual:@"schema"]) {
            host_shader_upload_test_send(2,1);host_shader_upload_test_send(0,1);
            assert(host_shader_upload_test_attempts()==1);
        } else {
            host_shader_upload_test_send(1,0);assert(host_shader_upload_test_attempts()==1);
            host_shader_upload_test_send(1,0);assert(host_shader_upload_test_attempts()==1);
            assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==1);
            if([mode isEqual:@"quota"]){for(int i=0;i<6;i++)host_shader_upload_test_send(1,1);assert(host_shader_upload_test_attempts()==4);}
        }
    } else if([mode isEqual:@"limits"]) {
        NSMutableData *large=[NSMutableData dataWithLength:256*1024];
        for(NSUInteger i=0;i<40;i++){((unsigned char *)large.mutableBytes)[0]=(unsigned char)i;[large writeToFile:[NSString stringWithFormat:@"shader-upload/outbox/shader-%03lu.json",(unsigned long)i] atomically:YES];}
        host_shader_upload_test_trim();NSUInteger bytes=0;
        for(NSString *path in host_typed_outbox_pending(@"shader-upload/outbox",@"shader"))bytes+=[[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil][NSFileSize] unsignedIntegerValue];
        assert(bytes<=8*1024*1024);
        assert(![NSFileManager.defaultManager fileExistsAtPath:@"shader-upload/outbox/shader-000.json"]);
        assert(![NSFileManager.defaultManager fileExistsAtPath:@"shader-upload/outbox/shader-000.json.sent"]);
        host_shader_upload_test_opt_out();
    } else if([mode isEqual:@"sent"]) {
        host_shader_observation_append(token,&event);host_shader_upload_test_barrier(1);
        assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==0);
        host_shader_upload_test_opt_out();
        assert(![NSFileManager.defaultManager fileExistsAtPath:@"shader-upload/working.json"]);
        assert(host_shader_observation_activate("maps/sample.map",0,key,sizeof(key))==0 && *key);
        host_shader_observation_append(token,&event);host_shader_upload_test_barrier(1);
        assert(host_typed_outbox_pending(@"shader-upload/outbox",@"shader").count==0);
    }
    assert(host_shader_upload_retry(0,0,0)==60);
    assert(host_shader_upload_retry(1,0,0)==300);
    assert(host_shader_upload_retry(2,0,0)==1800);
    assert(host_shader_upload_retry(3,0,0)==7200);
    assert(host_shader_upload_retry(100,100000,1)==103680);
    assert(host_shader_upload_retry(0,500,0)==500);
} return 0; }
