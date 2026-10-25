/* Apache-2.0. Exercise the disk worker itself without launching the game. */
#import "host_astc_store.m"
#include <stdarg.h>
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
    if(argc>2) {
        for(int i=2;i<argc;i++) {NSString *path=@(argv[i]);double start=NSProcessInfo.processInfo.systemUptime;
            uint64_t bytes_before=cache_bytes(root);BOOL ok=prepare(path,NULL,NULL);printf("BENCH map=%s complete=%d elapsed=%.6f read=%.6f encode=%.6f write=%.6f rewritten_blocks=%llu encoded_textures=%llu encoded_texels=%llu excluded=%llu store_bytes=%llu\n",path.lastPathComponent.UTF8String,ok,NSProcessInfo.processInfo.systemUptime-start,metrics.read_seconds,metrics.encode_seconds,metrics.write_seconds,(unsigned long long)metrics.direct_blocks,(unsigned long long)metrics.encoded_textures,(unsigned long long)metrics.encoded_texels,(unsigned long long)metrics.excluded,(unsigned long long)(cache_bytes(root)-bytes_before));
            if(!ok)return 1;
        }return 0;
    }
    NSString *(^fixture)(NSString *)=^NSString *(NSString *name){return [root stringByAppendingPathComponent:name];};
    uint8_t header[2048];uint64_t size=0;FILE *f=map_open(fixture(@"xbox.map"),header,&size);assert(f&&size==4096);fclose(f);
    for(NSString *name in @[@"truncated.map",@"bad-size.map",@"bad-zlib.map"])assert(!map_open(fixture(name),header,&size));
    for(NSString *name in @[@"xbox.map",@"pc.map",@"external.map",@"empty.map"]) {
        assert(prepare(fixture(name),NULL,NULL));assert(astc_store_map_complete(identity(fixture(name)).UTF8String));
    }
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
    converting_path=nil;priority_path=fixture(@"pc.map");assert(!walk_map(fixture(@"xbox.map"),identity(fixture(@"xbox.map")),NULL,NULL));priority_path=nil;
    worker=dispatch_queue_create("astc.test.suspended",DISPATCH_QUEUE_SERIAL);dispatch_suspend(worker);
    NSString *stable=fixture(@"stable.map");[line addObject:stable];pump();
    assert(observations[stable]);assert(!astc_store_map_complete(identity(stable).UTF8String));
    FILE *mutated=fopen(stable.fileSystemRepresentation,"r+b");fseeko(mutated,33,SEEK_SET);fputc(74,mutated);fclose(mutated);pump();
    assert(!astc_store_map_complete(identity(stable).UTF8String));
    observations[stable]=@{@"stamp":stamp(stable),@"time":@(NSProcessInfo.processInfo.systemUptime-11)};pump();
    assert(astc_store_map_complete(identity(stable).UTF8String));
    printf("PASS production worker: Xbox zlib, CE inline/shared, malformed offsets, resume, stable signatures, preemption\n");
    astc_store_shutdown();return 0;
}}
