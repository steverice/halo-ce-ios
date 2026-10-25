/* Apache-2.0. Exercise the disk worker itself without launching the game. */
#import "host_astc_store.m"
#include <stdarg.h>
void host_texture_upscale_init(const char *root) {(void)root;}
void host_texture_upscale_scan(void) {}
void host_texture_upscale_map_will_load(const char *name) {(void)name;}
void host_texture_upscale_game_started(int is_game) {(void)is_game;}
#include <assert.h>
int host_astc_texture_key(const void *,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,void *);
int host_astc_texture_read(const void *,void *,unsigned);
void host_config_string(const char *name,const char *fallback,char *out,size_t size) {
    snprintf(out,size,"%s",!strcmp(name,"debug.texture_astc")?"force":fallback);
}
double host_config_real(const char *name,double fallback) {(void)name;return fallback;}
void host_logf(int level,const char *format,...) {
    (void)level;va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);fputc('\n',stderr);
}
/* Synthetic data; catches mip truncation, DXT channel/alpha and Morton errors. */
static int source_calls;
static int source_check(void *context,const struct host_texture_source *source) {
    const char *kind=context;source_calls++;
    if(!strcmp(kind,"p8")) {
        assert(source->format==17&&source->width==4&&source->height==2);
        assert(source->level_count==3&&source->rgba_size==44);
        assert(source->level_offsets[1]==32&&source->level_offsets[2]==40);
        const uint8_t want0[]={0x7a,0x19,0xcc,0xff};
        const uint8_t want2[]={0x85,0x19,0xcc,0xff};
        const uint8_t flat[]={0x80,0x80,0xff,0};
        assert(!memcmp(source->rgba,want0,4));
        assert(!memcmp(source->rgba+8,want2,4));
        assert(!memcmp(source->rgba+28,flat,4));
        assert(!memcmp(source->rgba+40,flat,4));
    } else if(!strcmp(kind,"dxt1")) {
        assert(source->level_count==2&&source->rgba_size==160);
        const uint8_t red[]={255,0,0,255},green[]={0,255,0,255};
        assert(!memcmp(source->rgba,red,4));
        assert(!memcmp(source->rgba+128,green,4));
    } else {
        assert(source->format==11&&source->level_count==2&&source->rgba_size==20);
        const uint8_t rgba[]={3,2,1,4},tail[]={7,6,5,8};
        assert(!memcmp(source->rgba,rgba,4));
        assert(!memcmp(source->rgba+16,tail,4));
    }
    return 1;
}
static int source_stop(void *context,const struct host_texture_source *source) {(void)context;(void)source;return 0;}
static void source_put16(uint8_t *p,unsigned x) {p[0]=x;p[1]=x>>8;}
static void source_put32(uint8_t *p,unsigned x) {p[0]=x;p[1]=x>>8;p[2]=x>>16;p[3]=x>>24;}
static void source_probe(void) {
    uint8_t b[48]={0},bytes[24]={0};FILE *file=tmpfile();assert(file);
    source_put16(b+4,4);source_put16(b+6,2);source_put16(b+12,17);source_put16(b+14,1);source_put16(b+20,8);source_put32(b+28,11);
    for(unsigned i=0;i<7;i++)bytes[i]=i;bytes[7]=255;bytes[8]=10;bytes[9]=11;bytes[10]=255;
    assert(fwrite(bytes,1,11,file)==11);fflush(file);
    struct source_walk walk={source_check,"p8",NO};
    assert(source_bitmap(file,11,b,NO,&walk));assert(source_calls==1);
    assert(!source_bitmap(file,10,b,NO,&walk));assert(source_calls==1);
    walk.callback=source_stop;assert(!source_bitmap(file,11,b,NO,&walk)&&walk.canceled);
    fclose(file);file=tmpfile();assert(file);memset(bytes,0,sizeof(bytes));
    source_put16(b+4,8);source_put16(b+6,4);source_put16(b+12,14);source_put32(b+28,24);
    source_put16(bytes,0xf800);source_put16(bytes+8,0xf800);source_put16(bytes+16,0x07e0);
    assert(fwrite(bytes,1,24,file)==24);fflush(file);walk=(struct source_walk){source_check,"dxt1",NO};
    assert(source_bitmap(file,24,b,NO,&walk));fclose(file);
    file=tmpfile();assert(file);memset(bytes,0,sizeof(bytes));
    source_put16(b+4,2);source_put16(b+6,2);source_put16(b+12,11);source_put32(b+28,20);
    memcpy(bytes,(uint8_t[]){1,2,3,4},4);memcpy(bytes+16,(uint8_t[]){5,6,7,8},4);
    assert(fwrite(bytes,1,20,file)==20);fflush(file);walk=(struct source_walk){source_check,"bgra",NO};
    assert(source_bitmap(file,20,b,NO,&walk));fclose(file);
    assert(source_calls==3);
}

static int source_public_calls;
static int source_public_check(void *context,const struct host_texture_source *source) {
    (void)context;source_public_calls++;
    assert(source->width==4&&source->height==4&&source->format==15);
    assert(source->level_count==1&&source->rgba_size==64);
    assert(source->hash==0x2dacddd119685303ULL);
    const uint8_t opaque[]={255,255,255,255},clear[]={255,255,255,0};
    assert(!memcmp(source->rgba,opaque,4));assert(!memcmp(source->rgba+4,clear,4));
    return 1;
}
static int source_scan_check(void *context,const char *path) {
    unsigned *index=context;const char *names[]={"alpha.map","gamma.map","zeta.map"};
    assert(*index<3);assert(!strcmp(strrchr(path,'/')+1,names[(*index)++]));return 1;
}
static int source_scan_stop(void *context,const char *path) {(void)context;(void)path;return 0;}
static void source_scan_probe(NSString *root) {
    NSString *folder=[root stringByAppendingPathComponent:@"source-scan"];
    for(NSString *subfolder in @[@"maps",@"custom_maps",@"maps/directory.map"])
        assert([NSFileManager.defaultManager createDirectoryAtPath:[folder stringByAppendingPathComponent:subfolder] withIntermediateDirectories:YES attributes:nil error:nil]);
    for(NSString *file in @[@"maps/zeta.map",@"custom_maps/gamma.map",@"maps/alpha.map",@"maps/bitmaps.map",@"maps/ignore.txt"])
        assert([[NSData dataWithBytes:"x" length:1] writeToFile:[folder stringByAppendingPathComponent:file] atomically:YES]);
    unsigned index=0;assert(host_texture_source_scan(folder.fileSystemRepresentation,source_scan_check,&index)==1&&index==3);
    assert(host_texture_source_scan(folder.fileSystemRepresentation,source_scan_stop,NULL)==0);
}
static int cancel_after_one(void *context,uint64_t done,uint64_t total) {
    (void)context;(void)total;return planning||done<1;
}
static NSString *identity(NSString *path) {
    return host_map_identity_resolve(path.fileSystemRepresentation)[@"fingerprint"];
}
static uint64_t cache_bytes(NSString *root) {
    uint64_t bytes=0;NSString *folder=[root stringByAppendingPathComponent:@"astc-cache"];
    for(NSString *name in [NSFileManager.defaultManager enumeratorAtPath:folder]) {
        NSDictionary *a=[NSFileManager.defaultManager attributesOfItemAtPath:[folder stringByAppendingPathComponent:name] error:nil];
        if([a[NSFileType] isEqual:NSFileTypeRegular])bytes+=[a[NSFileSize] unsignedLongLongValue];
    }return bytes;
}
int main(int argc,const char **argv) {@autoreleasepool {
    assert(argc>=2);NSString *root=@(argv[1]);assert(chdir(root.fileSystemRepresentation)==0);
    data_root=root;install_root=@"";line_lock=[NSLock new];line=[NSMutableArray new];observations=[NSMutableDictionary new];
    assert(astc_store_init(root.fileSystemRepresentation));host_map_identity_init();
    source_probe();
    source_scan_probe(root);
    if(argc>2) {
        for(int i=2;i<argc;i++) {NSString *path=@(argv[i]);double start=NSProcessInfo.processInfo.systemUptime;
            uint64_t bytes_before=cache_bytes(root);BOOL ok=prepare(path,NULL,NULL);printf("BENCH map=%s complete=%d elapsed=%.6f read=%.6f encode=%.6f write=%.6f rewritten_blocks=%llu encoded_textures=%llu encoded_texels=%llu excluded=%llu store_bytes=%llu\n",path.lastPathComponent.UTF8String,ok,NSProcessInfo.processInfo.systemUptime-start,metrics.read_seconds,metrics.encode_seconds,metrics.write_seconds,(unsigned long long)metrics.direct_blocks,(unsigned long long)metrics.encoded_textures,(unsigned long long)metrics.encoded_texels,(unsigned long long)metrics.excluded,(unsigned long long)(cache_bytes(root)-bytes_before));
            if(!ok)return 1;
        }return 0;
    }
    NSString *(^fixture)(NSString *)=^NSString *(NSString *name){return [root stringByAppendingPathComponent:name];};
    uint8_t header[2048];uint64_t size=0;FILE *f=map_open(fixture(@"xbox.map"),header,&size);assert(f&&size==4096);fclose(f);
    char policy_error[160];
    f=host_texture_source_open_policy_map(fixture(@"xbox.map").fileSystemRepresentation,&size,policy_error,sizeof(policy_error));
    assert(f&&size==4096&&!policy_error[0]);
    const uint8_t *mapped=mmap(NULL,(size_t)size,PROT_READ,MAP_PRIVATE,fileno(f),0);
    assert(mapped!=MAP_FAILED&&!memcmp(mapped,"daeh",4));
    assert(!memcmp(mapped+2088,"bitm",4));munmap((void *)mapped,(size_t)size);fclose(f);
    assert(!host_texture_source_open_policy_map(fixture(@"pc.map").fileSystemRepresentation,&size,policy_error,sizeof(policy_error)));
    assert(size==0&&strstr(policy_error,"version 609"));
    assert(!host_texture_source_open_policy_map(fixture(@"bad-zlib.map").fileSystemRepresentation,&size,policy_error,sizeof(policy_error)));
    assert(size==0&&policy_error[0]);
    for(NSString *name in @[@"truncated.map",@"bad-size.map",@"bad-zlib.map"])assert(!map_open(fixture(name),header,&size));
    for(NSString *name in @[@"xbox.map",@"pc.map",@"external.map"]) {
        assert(host_texture_source_walk_map(fixture(name).fileSystemRepresentation,source_public_check,NULL)==1);
    }
    assert(source_public_calls==3);
    assert(host_texture_source_walk_map(fixture(@"xbox.map").fileSystemRepresentation,source_stop,NULL)==0);
    assert(host_texture_source_walk_map(fixture(@"bad-zlib.map").fileSystemRepresentation,source_public_check,NULL)==-1);
    for(NSString *name in @[@"xbox.map",@"pc.map",@"external.map",@"empty.map"]) {
        assert(prepare(fixture(name),NULL,NULL));assert(astc_store_map_complete(identity(fixture(name)).UTF8String));
    }
    assert(prepare(fixture(@"material.map"),NULL,NULL));
    assert(metrics.encoded_textures==1&&metrics.excluded==0);
    assert(prepare(fixture(@"material-duplicate.map"),NULL,NULL));
    assert(metrics.encoded_textures==0&&metrics.excluded==1);
    assert(prepare(fixture(@"cube.map"),NULL,NULL));assert(metrics.direct_blocks==30&&metrics.encoded_textures==0);
    for(NSString *name in @[@"pc.map",@"cube-alpha.map",@"pc-cube.map"]) {
        NSString *path=fixture(name);assert(prepare(path,NULL,NULL));
        uint8_t input[768]={0},output[480]={0};BOOL pc_cube=[name isEqual:@"pc-cube.map"];unsigned faces=[name containsString:@"cube"]?6:1,levels=faces==6?2:1,stride=faces==6?(pc_cube?80:128):16,width=faces==6?8:4;
        FILE *source=map_open(path,header,&size);assert(source);assert(read_at(source,faces==6?3000:3500,input,faces==6?(pc_cube?480:720):16));fclose(source);
        if(pc_cube){uint8_t raw[480];memcpy(raw,input,480);unsigned order[6]={0,2,1,3,4,5};for(unsigned face=0;face<6;face++)for(unsigned level=0;level<2;level++){unsigned bytes=level?16:64;memcpy(input+face*80+(level?64:0),raw+(level?384:0)+order[face]*bytes,bytes);}}
        struct astc_store_key key,expected;struct astc_store_span spans[12];size_t n=0;
        for(unsigned face=0;face<faces;face++)for(unsigned level=0;level<levels;level++)spans[n++]=(struct astc_store_span){input+face*stride+(level?(faces==6?64:16):0),faces==6&&!level?64:16};
        astc_store_key_compute(&expected,1,width,width,levels,faces,spans,n);
        assert(host_astc_texture_key(input,1,width,width,levels,faces,stride,&key));assert(!memcmp(&key,&expected,sizeof(key)));
        assert(astc_store_select_map(identity(path).UTF8String));assert(host_astc_texture_read(&key,output,faces==6?480:16));assert(!host_astc_texture_read(&key,output,15));
    }
    for(unsigned format=0;format<3;format++) {
        uint8_t bytes[32]={0};struct astc_store_key key,expected;struct astc_store_span span={bytes,format?16:8};
        astc_store_key_compute(&expected,format,4,4,1,1,&span,1);
        assert(host_astc_texture_key(bytes,format,4,4,1,1,span.size,&key));assert(!memcmp(&key,&expected,sizeof(key)));
    }
    assert(prepare(fixture(@"custom_maps/bitmaps.map"),NULL,NULL));
    assert(!prepare(fixture(@"bad-pointer.map"),NULL,NULL));assert(!prepare(fixture(@"bad-pixels.map"),NULL,NULL));
    NSString *resume=fixture(@"resume.map");assert(!prepare(resume,cancel_after_one,NULL));
    assert(!astc_store_map_complete(identity(resume).UTF8String));
    assert(prepare(resume,NULL,NULL));assert(metrics.encoded_textures==1);
    assert(prepare(resume,NULL,NULL));assert(astc_store_map_complete(identity(resume).UTF8String));
    NSString *before=stamp(resume);FILE *changed=fopen(resume.fileSystemRepresentation,"ab");fputc(0,changed);fclose(changed);assert(![before isEqual:stamp(resume)]);
    converting_path=nil;priority_path=fixture(@"pc.map");assert(!walk_map(fixture(@"xbox.map"),identity(fixture(@"xbox.map")),NULL,NULL,NULL));priority_path=nil;
    worker=dispatch_queue_create("astc.test.suspended",DISPATCH_QUEUE_SERIAL);dispatch_suspend(worker);
    NSString *stable=fixture(@"stable.map");[line addObject:stable];pump();
    assert(observations[stable]);assert(!astc_store_map_complete(identity(stable).UTF8String));
    FILE *mutated=fopen(stable.fileSystemRepresentation,"r+b");fseeko(mutated,33,SEEK_SET);fputc(74,mutated);fclose(mutated);pump();
    assert(!astc_store_map_complete(identity(stable).UTF8String));
    /* Gameplay pauses the worker on every device: no walk, and queued maps stay queued. */
    atomic_store(&playing,true);assert(!walk_map(fixture(@"xbox.map"),identity(fixture(@"xbox.map")),NULL,NULL,NULL));
    assert([line containsObject:stable]);
    observations[stable]=@{@"stamp":stamp(stable),@"time":@(NSProcessInfo.processInfo.systemUptime-11)};pump();
    assert([line containsObject:stable]&&!astc_store_map_complete(identity(stable).UTF8String));
    atomic_store(&playing,false);
    observations[stable]=@{@"stamp":stamp(stable),@"time":@(NSProcessInfo.processInfo.systemUptime-11)};pump();
    assert(astc_store_map_complete(identity(stable).UTF8String));
    printf("PASS production worker: Xbox zlib, CE inline/shared, malformed offsets, resume, stable signatures, preemption, gameplay pause\n");
    astc_store_shutdown();return 0;
}}
