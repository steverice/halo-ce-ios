/* tvOS first-run import. Apple TV has no Files app, so the app serves a small
   web page on the local network: the player opens it on a phone or computer,
   sends their own XISO, and it is imported like the iOS Files importer does
   (host_import.m) before the game starts. Uploads need the pairing code shown
   on the TV and are accepted only from private or link-local addresses. */
#import <UIKit/UIKit.h>
#import <CoreImage/CoreImage.h>
#include <Network/Network.h>
#include "ios_host.h"
#include "xiso.h"
#include "host_astc_store.h"
#include "host_texture_upscale.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#if !__has_feature(objc_arc)
#error The importer requires Objective-C ARC.
#endif

#define IMPORT_MARKER @"Halo tvOS import v1"
#define IMPORT_PREFIX @".halo-import-"
#define IMPORT_PORT "8080"
#define HEADER_LIMIT 16384
#define IMAGE_MINIMUM (64ull<<20)
#define IMAGE_MAXIMUM (16ull<<30)
/* The upload is not kept whole: once the image's directories have arrived, only the maps' own
   bytes are written (xiso_plan), and the extraction gives each map's room back as it copies it
   out (xiso_extract_maps_consuming). So an import needs room for the maps once (about 1.9 GB),
   the largest map again, and the start of the image up to its directories (about 0.7 GB on the
   retail disc, freed as soon as they are read): 3 GB free, whatever the image's size. An image
   whose directories come after its maps is stored whole, and needs its own size as well. */
#define IMPORT_FREE (3ull<<30)
/* how much more of the image arrives between looks for its directories */
#define PLAN_INTERVAL (32ull<<20)

enum import_state {import_waiting,import_receiving,import_extracting,import_done,import_failed};

/* Shared by the server queue, the extraction thread and the setup screen. */
static pthread_mutex_t status_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    enum import_state state;
    uint64_t done,total;
    char message[512];
    unsigned serial;
} status;

static NSString *documents,*staging,*upload_path;
static char pairing_code[8];
static nw_listener_t listener;
static dispatch_queue_t server_queue;
static uint16_t server_port;
static atomic_int preparing_textures, skip_textures, menu_phase_textures;

static void status_set(enum import_state state,uint64_t done,uint64_t total,NSString *message) {
    pthread_mutex_lock(&status_lock);
    status.state=state;status.done=done;status.total=total;
    if(message)snprintf(status.message,sizeof(status.message),"%s",message.UTF8String);
    status.serial++;
    pthread_mutex_unlock(&status_lock);
}

/* ---------- import */

static int extract_progress(void *context,const char *file,uint64_t done,uint64_t total) {
    (void)context;
    status_set(import_extracting,done,total,[NSString stringWithFormat:@"Importing %s",file]);
    return 1;
}

static int texture_progress(void *context,uint64_t done,uint64_t total) {
    (void)context;
    atomic_store(&preparing_textures,1);
    status_set(import_extracting,done,total,@"Preparing textures for this device");
    return !atomic_load(&skip_textures);
}

/* the on-device upscaler's import phase (rule 2), skipped with the same button */
static int upscale_progress(void *context,unsigned long long done,unsigned long long total,int menu_phase) {
    (void)context;
    atomic_store(&preparing_textures,1);
    /* the main menu's textures come first: Start playing is not offered until they are made */
    atomic_store(&menu_phase_textures,menu_phase);
    status_set(import_extracting,done,total,menu_phase?@"Upscaling the main menu":@"Upscaling textures. You can start playing now; the rest continues in menus.");
    return !atomic_load(&skip_textures);
}
/* Called with a complete upload; runs on its own thread. */
static void import_image(void) {
    @autoreleasepool {
        char error[1024]={0};
        NSFileManager *files=NSFileManager.defaultManager;
        status_set(import_extracting,0,0,@"Checking the disc image…");
        BOOL imported=xiso_extract_maps_consuming(upload_path.fileSystemRepresentation,staging.fileSystemRepresentation,
            extract_progress,NULL,error,sizeof(error));
        [files removeItemAtPath:upload_path error:nil];
        NSString *failure=imported?nil:[NSString stringWithUTF8String:error];
        if(imported) {
            /* Only reached without complete maps, so anything here is a
               partial or purged copy and safe to replace. */
            NSString *target=[documents stringByAppendingPathComponent:@"maps"];
            [files removeItemAtPath:target error:nil];
            if(![files moveItemAtPath:[staging stringByAppendingPathComponent:@"maps"] toPath:target error:nil]) {
                imported=NO;failure=@"Could not finish the import. Check the Apple TV's free storage and try again.";
            }
        }
        if(imported) {
            /* the intro and credits movies, when the build plays them and the disc has them (xiso.c) */
            NSString *movies=[documents stringByAppendingPathComponent:@"bink"];
            if([files fileExistsAtPath:[staging stringByAppendingPathComponent:@"bink"]]) {
                [files removeItemAtPath:movies error:nil];
                [files moveItemAtPath:[staging stringByAppendingPathComponent:@"bink"] toPath:movies error:nil];
            }
            host_logf(HOST_LOG_INFO,"XISO import complete");
            atomic_store(&skip_textures,0);
            host_astc_store_prepare_import(documents.fileSystemRepresentation,texture_progress,NULL);
            host_texture_upscale_prepare_import(documents.fileSystemRepresentation,upscale_progress,NULL);
            atomic_store(&preparing_textures,0);atomic_store(&menu_phase_textures,0);
            [files removeItemAtPath:staging error:nil];
            status_set(import_done,1,1,@"Imported. Starting Halo…");
        } else {
            host_logf(HOST_LOG_ERROR,"XISO import failed: %s",failure.UTF8String);
            /* A fresh staging folder for the next attempt. */
            [files removeItemAtPath:[staging stringByAppendingPathComponent:@"maps.partial"] error:nil];
            [files removeItemAtPath:[staging stringByAppendingPathComponent:@"maps"] error:nil];
            [files removeItemAtPath:[staging stringByAppendingPathComponent:@"bink.partial"] error:nil];
            [files removeItemAtPath:[staging stringByAppendingPathComponent:@"bink"] error:nil];
            status_set(import_failed,0,0,failure);
        }
    }
}

/* ---------- HTTP */

static NSString *const page=@"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'><title>VISR for Apple TV</title><style>"
":root{color-scheme:light dark;--bg:#f4f5f2;--fg:#15191b;--muted:#5c6468;--card:#fff;--accent:#4f8f3a;--line:#d6dad6}"
"@media (prefers-color-scheme:dark){:root{--bg:#0b1114;--fg:#e8ece9;--muted:#9aa4a8;--card:#141d21;--line:#27343a}}"
"body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,system-ui,sans-serif}"
"main{max-width:560px;margin:0 auto;padding:32px 16px}h1{margin:0 0 4px;font-size:28px}p{color:var(--muted);margin:0 0 20px}"
"label{display:block;font-weight:600;margin:16px 0 6px}input[type=text]{font:inherit;font-size:22px;letter-spacing:.2em;width:9ch;padding:6px 10px;"
"border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--fg)}"
"#drop{display:block;border:2px dashed var(--line);border-radius:12px;padding:32px 16px;text-align:center;background:var(--card);cursor:pointer}"
"#drop.over{border-color:var(--accent)}#file{display:none}progress{width:100%;height:12px;margin-top:20px;accent-color:var(--accent)}"
"#status{margin-top:8px;min-height:3em;overflow-wrap:anywhere}</style></head><body><main>"
"<h1>VISR</h1><p>Send your own Halo: Combat Evolved Xbox disc image (.iso or .xiso) to your Apple TV. "
"Keep the VISR setup screen open on the TV until the import finishes.</p>"
"<label for=code>Code shown on the TV</label><input id=code type=text inputmode=numeric autocomplete=off maxlength=6>"
"<label>Disc image</label><label id=drop for=file>Drop the disc image here, or choose it</label>"
"<input id=file type=file accept='.iso,.xiso'><progress id=bar max=1 value=0 hidden></progress><div id=status></div>"
"<script>"
"const $=id=>document.getElementById(id),status=t=>$('status').textContent=t;"
"$('code').value=new URLSearchParams(location.search).get('code')||'';let busy=false;"
"function poll(){fetch('/status').then(r=>r.json()).then(s=>{status(s.message);"
"if(s.total){$('bar').hidden=false;$('bar').value=s.done/s.total}"
"if(s.state=='extracting'||s.state=='receiving')setTimeout(poll,1000);else busy=false}).catch(()=>{status('Lost contact with the Apple TV.');busy=false})}"
"function send(f){if(busy||!f)return;busy=true;const x=new XMLHttpRequest();x.open('PUT','/upload');"
"x.setRequestHeader('X-Halo-Code',$('code').value.trim());$('bar').hidden=false;$('bar').value=0;"
"x.upload.onprogress=e=>{if(e.lengthComputable){$('bar').value=e.loaded/e.total;"
"status('Sending '+f.name+' · '+Math.floor(100*e.loaded/e.total)+'%')}};"
"x.onload=()=>{if(x.status==200)poll();else{status(x.responseText||'The Apple TV refused the upload.');busy=false}};"
"x.onerror=()=>poll();x.send(f)}"
"$('file').onchange=e=>send(e.target.files[0]);const d=$('drop');"
"d.ondragover=e=>{e.preventDefault();d.classList.add('over')};d.ondragleave=()=>d.classList.remove('over');"
"d.ondrop=e=>{e.preventDefault();d.classList.remove('over');send(e.dataTransfer.files[0])};"
"</script></main></body></html>";

@interface HaloUploadRequest : NSObject
@property(nonatomic,strong) nw_connection_t connection;
@property(nonatomic,strong) NSMutableData *header;
@property(nonatomic) int fd;
@property(nonatomic) uint64_t length,received;
@end
@implementation HaloUploadRequest
@end

static void respond(nw_connection_t connection,int code,NSString *type,NSString *body) {
    const char *reason=code==200?"OK":code==100?"Continue":code==400?"Bad Request":code==403?"Forbidden":
        code==404?"Not Found":code==409?"Conflict":code==413?"Payload Too Large":code==507?"Insufficient Storage":"Error";
    NSData *bytes=[body dataUsingEncoding:NSUTF8StringEncoding];
    NSString *head=[NSString stringWithFormat:@"HTTP/1.1 %d %s\r\nContent-Type: %@\r\nContent-Length: %lu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",code,reason,type,(unsigned long)bytes.length];
    NSMutableData *all=[[head dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];[all appendData:bytes];
    dispatch_data_t data=dispatch_data_create(all.bytes,all.length,server_queue,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    nw_connection_send(connection,data,NW_CONNECTION_FINAL_MESSAGE_CONTEXT,true,^(nw_error_t error) {
        (void)error;nw_connection_cancel(connection);
    });
}
static void respond_text(nw_connection_t connection,int code,NSString *text) {
    respond(connection,code,@"text/plain; charset=utf-8",text);
}

static NSString *status_json(void) {
    static const char *names[]={"waiting","receiving","extracting","done","failed"};
    pthread_mutex_lock(&status_lock);
    NSDictionary *json=@{@"state":@(names[status.state]),@"done":@(status.done),@"total":@(status.total),
        @"message":@(status.message)};
    pthread_mutex_unlock(&status_lock);
    return [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:json options:0 error:nil] encoding:NSUTF8StringEncoding];
}

/* Private IPv4 ranges, link-local, loopback and IPv6 unique-local only. */
static BOOL local_peer(nw_connection_t connection) {
    nw_endpoint_t endpoint=nw_connection_copy_endpoint(connection);
    if(nw_endpoint_get_type(endpoint)!=nw_endpoint_type_address)return NO;
    const struct sockaddr *address=nw_endpoint_get_address(endpoint);
    if(address->sa_family==AF_INET) {
        uint32_t ip=ntohl(((const struct sockaddr_in *)address)->sin_addr.s_addr);
        return (ip>>24)==10 || (ip>>24)==127 || (ip>>20)==0xAC1 || (ip>>16)==0xC0A8 || (ip>>16)==0xA9FE;
    }
    if(address->sa_family==AF_INET6) {
        const struct in6_addr *ip=&((const struct sockaddr_in6 *)address)->sin6_addr;
        if(IN6_IS_ADDR_V4MAPPED(ip)) {
            uint32_t v4=((uint32_t)ip->s6_addr[12]<<24)|((uint32_t)ip->s6_addr[13]<<16)|((uint32_t)ip->s6_addr[14]<<8)|ip->s6_addr[15];
            return (v4>>24)==10 || (v4>>24)==127 || (v4>>20)==0xAC1 || (v4>>16)==0xC0A8 || (v4>>16)==0xA9FE;
        }
        return IN6_IS_ADDR_LOOPBACK(ip) || IN6_IS_ADDR_LINKLOCAL(ip) || (ip->s6_addr[0]&0xFE)==0xFC;
    }
    return NO;
}

/* the parts of the image being received that the import will read (xiso_plan); none until its
   directories have arrived */
static struct xiso_range plan[XISO_PLAN_RANGES];
static int plan_count;
static uint64_t plan_tried;

static BOOL write_at(int fd,const unsigned char *bytes,size_t size,uint64_t offset) {
    while(size) {
        ssize_t count=pwrite(fd,bytes,size,(off_t)offset);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return NO;
        bytes+=count;offset+=(uint64_t)count;size-=(size_t)count;
    }
    return YES;
}

/* Writes the next bytes of the image at their own place in the file: all of them until the plan
   is known, then only what the plan keeps (the file has the image's full length, with holes). */
static BOOL image_write(int fd,const unsigned char *bytes,size_t size,uint64_t offset) {
    if(!plan_count)return write_at(fd,bytes,size,offset);
    for(int i=0;i<plan_count;i++) {
        uint64_t start=plan[i].start>offset?plan[i].start:offset;
        uint64_t end=plan[i].end<offset+size?plan[i].end:offset+size;
        if(start<end && !write_at(fd,bytes+(start-offset),(size_t)(end-start),start))return NO;
    }
    return YES;
}

/* Looks for the image's directories in what has arrived; with them, frees what was written and is
   not needed. */
static void image_plan(int fd,uint64_t received,uint64_t total) {
    if(plan_count || received<plan_tried+PLAN_INTERVAL)return;
    plan_tried=received;
    plan_count=xiso_plan(upload_path.fileSystemRepresentation,received,total,plan,XISO_PLAN_RANGES);
    if(!plan_count)return;
    uint64_t kept=0,from=0;
    for(int i=0;i<plan_count;i++) {
        if(plan[i].start<received)xiso_free_range(fd,from,plan[i].start<received?plan[i].start:received);
        from=plan[i].end;kept+=plan[i].end-plan[i].start;
    }
    if(from<received)xiso_free_range(fd,from,received);
    host_logf(HOST_LOG_INFO,"upload: the image's directories arrived in its first %llu MB; keeping %llu MB of %llu (%d parts)",
        (unsigned long long)(received>>20),(unsigned long long)(kept>>20),(unsigned long long)(total>>20),plan_count);
}

static void upload_failed(HaloUploadRequest *request,NSString *message) {
    if(request.fd>=0){close(request.fd);request.fd=-1;}
    [NSFileManager.defaultManager removeItemAtPath:upload_path error:nil];
    host_logf(HOST_LOG_ERROR,"upload failed: %s",message.UTF8String);
    status_set(import_failed,0,0,message);
}

static BOOL write_all(int fd,const void *bytes,size_t size) {
    const char *p=bytes;
    while(size) {
        ssize_t count=write(fd,p,size);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return NO;
        p+=count;size-=(size_t)count;
    }
    return YES;
}

/* Returns NO once the request has failed. */
static BOOL upload_append(HaloUploadRequest *request,const void *bytes,size_t size) {
    uint64_t remaining=request.length-request.received;
    if(size>remaining)size=(size_t)remaining;
    if(!image_write(request.fd,bytes,size,request.received)) {
        upload_failed(request,@"The Apple TV ran out of storage while receiving the disc image.");
        respond_text(request.connection,507,@"The Apple TV ran out of storage.");
        return NO;
    }
    request.received+=size;
    image_plan(request.fd,request.received,request.length);
    status_set(import_receiving,request.received,request.length,@"Receiving the disc image…");
    return YES;
}

static void upload_finish(HaloUploadRequest *request) {
    close(request.fd);request.fd=-1;
    host_logf(HOST_LOG_INFO,"upload received: %llu bytes",(unsigned long long)request.received);
    status_set(import_extracting,0,0,@"Received. Checking the disc image…");
    respond_text(request.connection,200,@"Received.");
    [NSThread detachNewThreadWithBlock:^{import_image();}];
}

static void receive_body(HaloUploadRequest *request) {
    nw_connection_receive(request.connection,1,1u<<20,^(dispatch_data_t content,nw_content_context_t context,bool complete,nw_error_t error) {
        (void)context;
        __block BOOL ok=YES;
        if(content) dispatch_data_apply(content,^bool(dispatch_data_t region,size_t offset,const void *buffer,size_t size) {
            (void)region;(void)offset;ok=upload_append(request,buffer,size);return ok;
        });
        if(!ok)return;
        if(request.received==request.length){upload_finish(request);return;}
        if(error || complete) {
            upload_failed(request,@"The upload stopped before the whole disc image arrived. Try again.");
            nw_connection_cancel(request.connection);return;
        }
        receive_body(request);
    });
}

static NSString *header_value(NSArray<NSString *> *lines,NSString *name) {
    for(NSString *line in lines) {
        NSRange colon=[line rangeOfString:@":"];
        if(colon.location!=NSNotFound && [[line substringToIndex:colon.location] caseInsensitiveCompare:name]==NSOrderedSame)
            return [[line substringFromIndex:colon.location+1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    }
    return nil;
}

static void start_upload(HaloUploadRequest *request,NSArray<NSString *> *lines,NSData *rest) {
    nw_connection_t connection=request.connection;
    NSString *code=header_value(lines,@"X-Halo-Code");
    NSString *length=header_value(lines,@"Content-Length");
    NSString *expect=header_value(lines,@"Expect");
    unsigned long long size=length.longLongValue>0?(unsigned long long)length.longLongValue:0;
    pthread_mutex_lock(&status_lock);enum import_state state=status.state;pthread_mutex_unlock(&status_lock);
    NSNumber *available=nil;
    [[NSURL fileURLWithPath:documents] getResourceValue:&available forKey:NSURLVolumeAvailableCapacityKey error:nil];
    if(!code || strcmp(code.UTF8String,pairing_code)) {respond_text(connection,403,@"That code doesn't match the one on the TV.");return;}
    if(state!=import_waiting && state!=import_failed) {respond_text(connection,409,@"The Apple TV is already importing a disc image.");return;}
    if(size<IMAGE_MINIMUM || size>IMAGE_MAXIMUM) {respond_text(connection,413,@"That file isn't the size of an Xbox disc image.");return;}
    if(available && available.unsignedLongLongValue<IMPORT_FREE) {
        host_logf(HOST_LOG_ERROR,"upload refused: %llu MB free, %llu needed",available.unsignedLongLongValue>>20,IMPORT_FREE>>20);
        respond_text(connection,507,[NSString stringWithFormat:@"The Apple TV needs %.1f GB free for this import and has %.1f.",
            IMPORT_FREE/1e9,available.unsignedLongLongValue/1e9]);return;
    }
    request.fd=open(upload_path.fileSystemRepresentation,O_RDWR|O_CREAT|O_TRUNC,0600);
    /* (the image's full length from the start: what the plan leaves out stays a hole) */
    if(request.fd<0 || ftruncate(request.fd,(off_t)size)) {
        if(request.fd>=0){close(request.fd);request.fd=-1;}
        respond_text(connection,507,@"Could not store the disc image on the Apple TV.");return;
    }
    plan_count=0;plan_tried=0;
    host_logf(HOST_LOG_INFO,"upload: %llu MB free on the Apple TV",available.unsignedLongLongValue>>20);
    request.length=size;request.received=0;
    host_logf(HOST_LOG_INFO,"upload started: %llu bytes",size);
    status_set(import_receiving,0,size,@"Receiving the disc image…");
    if(expect && [expect caseInsensitiveCompare:@"100-continue"]==NSOrderedSame) {
        dispatch_data_t data=dispatch_data_create("HTTP/1.1 100 Continue\r\n\r\n",25,server_queue,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        nw_connection_send(connection,data,NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT,true,^(nw_error_t error){(void)error;});
    }
    if(rest.length && !upload_append(request,rest.bytes,rest.length))return;
    if(request.received==request.length){upload_finish(request);return;}
    receive_body(request);
}

static void handle_request(HaloUploadRequest *request,NSString *head,NSData *rest) {
    nw_connection_t connection=request.connection;
    NSArray<NSString *> *lines=[head componentsSeparatedByString:@"\r\n"];
    NSArray<NSString *> *words=[lines.firstObject componentsSeparatedByString:@" "];
    NSString *method=words.count>1?words[0]:@"",*path=words.count>1?words[1]:@"";
    path=[path componentsSeparatedByString:@"?"].firstObject;
    if([method isEqualToString:@"GET"] && [path isEqualToString:@"/"])
        respond(connection,200,@"text/html; charset=utf-8",page);
    else if([method isEqualToString:@"GET"] && [path isEqualToString:@"/status"])
        respond(connection,200,@"application/json",status_json());
    else if([method isEqualToString:@"PUT"] && [path isEqualToString:@"/upload"])
        start_upload(request,lines,rest);
    else
        respond_text(connection,404,@"Not found.");
}

static void receive_header(HaloUploadRequest *request) {
    nw_connection_receive(request.connection,1,HEADER_LIMIT,^(dispatch_data_t content,nw_content_context_t context,bool complete,nw_error_t error) {
        (void)context;
        if(content) dispatch_data_apply(content,^bool(dispatch_data_t region,size_t offset,const void *buffer,size_t size) {
            (void)region;(void)offset;[request.header appendBytes:buffer length:size];return true;
        });
        NSRange end=[request.header rangeOfData:[NSData dataWithBytes:"\r\n\r\n" length:4] options:0 range:NSMakeRange(0,request.header.length)];
        if(end.location!=NSNotFound) {
            NSString *head=[[NSString alloc] initWithData:[request.header subdataWithRange:NSMakeRange(0,end.location)] encoding:NSUTF8StringEncoding];
            NSData *rest=[request.header subdataWithRange:NSMakeRange(end.location+4,request.header.length-end.location-4)];
            request.header=nil;
            if(!head){respond_text(request.connection,400,@"Bad request.");return;}
            handle_request(request,head,rest);
            return;
        }
        if(request.header.length>=HEADER_LIMIT || error || complete) {
            nw_connection_cancel(request.connection);request.header=nil;return;
        }
        receive_header(request);
    });
}

static void accept_connection(nw_connection_t connection) {
    nw_connection_set_queue(connection,server_queue);
    nw_connection_start(connection);
    if(!local_peer(connection)) {nw_connection_cancel(connection);return;}
    HaloUploadRequest *request=[HaloUploadRequest new];
    request.connection=connection;request.header=[NSMutableData new];request.fd=-1;
    receive_header(request);
}

static void start_listener(const char *port) {
    nw_parameters_t parameters=nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL,NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_parameters_set_reuse_local_address(parameters,true);
    nw_listener_t created=nw_listener_create_with_port(port,parameters);
    if(!created) {status_set(import_failed,0,0,@"Could not start the import server.");return;}
    listener=created;
    nw_listener_set_queue(created,server_queue);
    nw_listener_set_new_connection_handler(created,^(nw_connection_t connection){accept_connection(connection);});
    nw_listener_set_state_changed_handler(created,^(nw_listener_state_t state,nw_error_t error) {
        (void)error;
        if(state==nw_listener_state_ready) {
            server_port=nw_listener_get_port(created);
            host_logf(HOST_LOG_INFO,"import server listening on port %u",server_port);
            status_set(import_waiting,0,0,@"Waiting for the disc image…");
        } else if(state==nw_listener_state_failed && created==listener) {
            nw_listener_cancel(created);
            /* The usual port is taken: let the system pick one. */
            if(strcmp(port,"0"))start_listener("0");
            else status_set(import_failed,0,0,@"Could not start the import server.");
        }
    });
    nw_listener_start(created);
}

/* ---------- setup screen */

static NSString *local_address(void) {
    struct ifaddrs *list=NULL,*entry;NSString *found=nil,*fallback=nil;
    if(getifaddrs(&list))return nil;
    for(entry=list;entry;entry=entry->ifa_next) {
        if(!entry->ifa_addr || entry->ifa_addr->sa_family!=AF_INET)continue;
        if(!(entry->ifa_flags&IFF_UP) || (entry->ifa_flags&IFF_LOOPBACK))continue;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&((struct sockaddr_in *)entry->ifa_addr)->sin_addr,text,sizeof(text));
        if(!strncmp(entry->ifa_name,"en",2)){if(!found)found=@(text);}
        else if(!fallback)fallback=@(text);
    }
    freeifaddrs(list);
    return found?:fallback;
}

static UIImage *qr_image(NSString *text) {
    CIFilter *filter=[CIFilter filterWithName:@"CIQRCodeGenerator"];
    [filter setValue:[text dataUsingEncoding:NSUTF8StringEncoding] forKey:@"inputMessage"];
    [filter setValue:@"M" forKey:@"inputCorrectionLevel"];
    CIImage *image=[filter.outputImage imageByApplyingTransform:CGAffineTransformMakeScale(12,12)];
    CGImageRef rendered=[[CIContext context] createCGImage:image fromRect:image.extent];
    UIImage *result=[UIImage imageWithCGImage:rendered];CGImageRelease(rendered);
    return result;
}

@interface HaloTVImportController : UIViewController
@property(nonatomic,strong) UILabel *addressLabel,*codeLabel,*statusLabel;
@property(nonatomic,strong) UIImageView *qr;
@property(nonatomic,strong) UIProgressView *progress;
@property(nonatomic,strong) UIButton *skipTexturesButton;
@property(nonatomic,copy) NSString *shownURL;
@property(nonatomic) unsigned shownSerial;
@end

@implementation HaloTVImportController
- (UILabel *)label:(UIFontTextStyle)style color:(UIColor *)color {
    UILabel *label=[UILabel new];label.font=[UIFont preferredFontForTextStyle:style];label.textColor=color;
    label.numberOfLines=0;label.textAlignment=NSTextAlignmentCenter;return label;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor=[UIColor colorWithRed:0.025 green:0.045 blue:0.055 alpha:1];
    self.overrideUserInterfaceStyle=UIUserInterfaceStyleDark;
    UIStackView *stack=[UIStackView new];stack.axis=UILayoutConstraintAxisVertical;stack.spacing=28;
    stack.alignment=UIStackViewAlignmentCenter;stack.translatesAutoresizingMaskIntoConstraints=NO;[self.view addSubview:stack];
    UILabel *title=[self label:UIFontTextStyleTitle1 color:UIColor.labelColor];title.text=@"VISR";[stack addArrangedSubview:title];
    UILabel *body=[self label:UIFontTextStyleBody color:UIColor.secondaryLabelColor];
    body.text=@"To play, send your own Halo: Combat Evolved Xbox disc image (.iso or .xiso)\nfrom a phone or computer on the same network. No game files are included.";
    [stack addArrangedSubview:body];
    self.qr=[UIImageView new];self.qr.layer.magnificationFilter=kCAFilterNearest;
    self.qr.backgroundColor=UIColor.whiteColor;self.qr.contentMode=UIViewContentModeScaleAspectFit;
    [self.qr.widthAnchor constraintEqualToConstant:300].active=YES;[self.qr.heightAnchor constraintEqualToConstant:300].active=YES;
    [stack addArrangedSubview:self.qr];
    self.addressLabel=[self label:UIFontTextStyleHeadline color:UIColor.labelColor];[stack addArrangedSubview:self.addressLabel];
    self.codeLabel=[self label:UIFontTextStyleTitle2 color:[UIColor colorWithRed:0.45 green:0.78 blue:0.38 alpha:1]];
    self.codeLabel.text=[NSString stringWithFormat:@"Code %s",pairing_code];[stack addArrangedSubview:self.codeLabel];
    self.progress=[[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];self.progress.hidden=YES;
    [self.progress.widthAnchor constraintEqualToConstant:900].active=YES;[stack addArrangedSubview:self.progress];
    self.statusLabel=[self label:UIFontTextStyleCallout color:UIColor.secondaryLabelColor];[stack addArrangedSubview:self.statusLabel];
    self.skipTexturesButton=[UIButton buttonWithType:UIButtonTypeSystem];
    [self.skipTexturesButton setTitle:@"Start playing" forState:UIControlStateNormal];
    [self.skipTexturesButton addTarget:self action:@selector(skipTexturePreparation) forControlEvents:UIControlEventPrimaryActionTriggered];
    self.skipTexturesButton.hidden=YES;[stack addArrangedSubview:self.skipTexturesButton];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.85]]];
    self.shownSerial=~0u;
}
- (void)skipTexturePreparation { atomic_store(&skip_textures,1); }
/* Called from the wait loop; returns YES once the import is done. */
- (BOOL)refresh {
    NSString *address=local_address();
    NSString *url=address && server_port?[NSString stringWithFormat:@"http://%@:%u",address,server_port]:nil;
    if(url && ![url isEqualToString:self.shownURL]) {
        self.shownURL=url;
        self.addressLabel.text=[NSString stringWithFormat:@"Scan the code, or open %@",url];
        self.qr.image=qr_image([NSString stringWithFormat:@"%@/?code=%s",url,pairing_code]);
    } else if(!url && !self.shownURL) {
        self.addressLabel.text=@"Connect the Apple TV to your network to continue.";
    }
    pthread_mutex_lock(&status_lock);
    unsigned serial=status.serial;enum import_state state=status.state;
    uint64_t done=status.done,total=status.total;NSString *message=@(status.message);
    pthread_mutex_unlock(&status_lock);
    BOOL wasHidden=self.skipTexturesButton.hidden;
    self.skipTexturesButton.hidden=!atomic_load(&preparing_textures)||atomic_load(&menu_phase_textures);
    if(wasHidden&&!self.skipTexturesButton.hidden)[self setNeedsFocusUpdate];
    if(serial!=self.shownSerial) {
        self.shownSerial=serial;
        self.statusLabel.text=message;
        self.progress.hidden=!total;
        if(total)self.progress.progress=(float)((double)done/total);
    }
    return state==import_done;
}
@end

/* Recover from an import interrupted by a crash, the Home button or a purge:
   only folders carrying our marker are removed. */
static void import_cleanup(NSString *root) {
    NSFileManager *files=NSFileManager.defaultManager;
    for(NSString *name in [files contentsOfDirectoryAtPath:root error:nil]) {
        if(![name hasPrefix:IMPORT_PREFIX] || ![[NSUUID alloc] initWithUUIDString:[name substringFromIndex:IMPORT_PREFIX.length]])continue;
        NSString *folder=[root stringByAppendingPathComponent:name];
        if([[NSString stringWithContentsOfFile:[folder stringByAppendingPathComponent:@"owner.txt"] encoding:NSUTF8StringEncoding error:nil] isEqualToString:IMPORT_MARKER])
            [files removeItemAtPath:folder error:nil];
    }
}

void host_tv_import(const char *root) {
    NSFileManager *files=NSFileManager.defaultManager;
    documents=[NSString stringWithUTF8String:root];
    import_cleanup(documents);
    staging=[documents stringByAppendingPathComponent:[IMPORT_PREFIX stringByAppendingString:NSUUID.UUID.UUIDString]];
    if(![files createDirectoryAtPath:staging withIntermediateDirectories:NO attributes:nil error:nil])
        host_fatal("Could not create the import folder. Check the Apple TV's free storage.");
    [IMPORT_MARKER writeToFile:[staging stringByAppendingPathComponent:@"owner.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    upload_path=[staging stringByAppendingPathComponent:@"upload.iso"];
    snprintf(pairing_code,sizeof(pairing_code),"%06u",arc4random_uniform(1000000));
    status_set(import_waiting,0,0,@"Starting…");
    server_queue=dispatch_queue_create("halo.import",DISPATCH_QUEUE_SERIAL);
    start_listener(IMPORT_PORT);

    UIWindowScene *scene=nil;
    for(UIScene *candidate in UIApplication.sharedApplication.connectedScenes)
        if([candidate isKindOfClass:UIWindowScene.class]) {scene=(UIWindowScene *)candidate;break;}
    if(!scene)host_fatal("Could not open the game import screen.");
    HaloTVImportController *controller=[HaloTVImportController new];
    UIWindow *window=[[UIWindow alloc] initWithWindowScene:scene];window.frame=scene.coordinateSpace.bounds;
    window.windowLevel=UIWindowLevelNormal+2;window.rootViewController=controller;[window makeKeyAndVisible];
    host_logf(HOST_LOG_INFO,"waiting for an XISO upload");
    while(![controller refresh]) {
        @autoreleasepool {CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.1,true);}
    }
    /* Let the browser's last status poll see "done" before the server stops. */
    for(int i=0;i<15;i++) @autoreleasepool {CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.1,true);}
    dispatch_sync(server_queue,^{nw_listener_cancel(listener);listener=nil;});
    window.hidden=YES;window.rootViewController=nil;
}

/* tvOS: the player's XISO is imported into root/maps over the local network
   (host_tv_import, above). */
void host_ios_prepare_assets(const char *root) {
    NSString *imported=[[NSString stringWithUTF8String:root] stringByAppendingPathComponent:@"maps"];
    char reason[1024]={0};
    /* Caches is purgeable, so the import runs again if tvOS evicted the maps. */
    if(!xiso_maps_ready(imported.fileSystemRepresentation,NULL,0))host_tv_import(root);
    if(!xiso_maps_ready(imported.fileSystemRepresentation,reason,sizeof(reason)))
        host_fatal("The imported maps are not usable (%s).",reason);
    host_logf(HOST_LOG_INFO,"maps: %s",imported.fileSystemRepresentation);
}
