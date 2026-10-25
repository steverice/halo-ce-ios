/* Apache-2.0. Private stock-map measurements; never save extracted art in the tree. */
#import "host_astc_store.m"
#include "bridge.h"
#include <assert.h>
#include <stdarg.h>
void host_texture_upscale_init(const char *root) {(void)root;}
void host_texture_upscale_scan(void) {}
void host_texture_upscale_map_will_load(const char *name) {(void)name;}
void host_texture_upscale_game_started(int is_game) {(void)is_game;}
void host_config_string(const char *name,const char *fallback,char *out,size_t size) {
    snprintf(out,size,"%s",!strcmp(name,"debug.texture_astc")?"force":fallback);
}
double host_config_real(const char *name,double fallback) {(void)name;return fallback;}
void host_logf(int level,const char *format,...) {(void)level;(void)format;}
static void emit(NSDictionary *record,FILE *file) {
    NSData *data=[NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingSortedKeys error:nil];
    assert(data);fwrite(data.bytes,1,data.length,file);fputc('\n',file);fflush(file);
}
static NSMutableArray<NSDictionary *> *worst;
static FILE *records;
static BOOL audit_bitmap(FILE *map,uint64_t size,const uint8_t *b,BOOL pc,NSString *map_name,unsigned tag,unsigned bitmap) {
    unsigned width=u16(b+4),height=u16(b+6),type=u16(b+10),source_format=u16(b+12),flags=u16(b+14),levels=1;
    if(type==1||source_format<14||source_format>16)return YES;
    if((flags&1)&&!(flags&(1u<<4))){unsigned side=MAX(width/4,MAX(height/4,(unsigned)u16(b+8))),maximum=0;while(side>1){side>>=1;maximum++;}levels=MIN((unsigned)u16(b+20),maximum)+1;}
    if(!width||!height||width>4096||height>4096||levels>13||(flags&(1u<<8)))return NO;
    enum astc_dxt_format format=(enum astc_dxt_format)(source_format-14);
    unsigned faces=type==2?6:1;size_t source_face=0;
    for(unsigned l=0;l<levels;l++)source_face+=((MAX(1u,width>>l)+3)/4)*((MAX(1u,height>>l)+3)/4)*(format==ASTC_DXT1?8:16);
    size_t stride=faces==6&&!pc?(source_face+127)&~(size_t)127:source_face;
    size_t source_size=stride*(faces-1)+source_face;uint64_t offset=u32(b+24);
    if(offset>size||source_size>size-offset||source_size>u32(b+28))return NO;
    NSMutableData *input=[NSMutableData dataWithLength:source_size];
    if(!read_at(map,offset,input.mutableBytes,source_size))return NO;
    const uint8_t *source=input.bytes;struct astc_store_span spans[90];size_t count=0;BOOL direct=YES;
    for(unsigned face=0;face<faces;face++){size_t at=face*stride,preceding=0;
        for(unsigned l=0;l<levels;l++){unsigned w=MAX(1u,width>>l),h=MAX(1u,height>>l);size_t bytes=((w+3)/4)*((h+3)/4)*(format==ASTC_DXT1?8:16),position=at;
            if(pc&&faces==6){static const unsigned order[6]={0,2,1,3,4,5};position=preceding*faces+order[face]*bytes;}
            spans[count++]=(struct astc_store_span){source+position,bytes};direct&=astc_rewrite_level_supported(format,source+position,w,h);at+=bytes;preceding+=bytes;}}
    struct astc_store_key key;astc_store_key_compute(&key,format,width,height,levels,faces,spans,count);
    NSData *key_data=[NSData dataWithBytes:&key length:sizeof(key)];struct astc_alpha_policy policy={0};
    NSData *effective=alpha_policies[key_data];if(effective)[effective getBytes:&policy length:sizeof(policy)];
    NSMutableString *key_text=[NSMutableString string];for(size_t i=0;i<sizeof(key);i++)[key_text appendFormat:@"%02x",((const uint8_t *)&key)[i]];
    NSMutableArray *boundaries=[NSMutableArray new];if(policy.known)for(unsigned r=1;r<256;r++)if(policy.boundaries[r/8]&(1u<<(r%8)))[boundaries addObject:@(r)];
    BOOL old_pass=YES,new_pass=YES;double min_psnr=999,max_test=0,max_all=0,squared_error=0,channels=0;unsigned max_alpha=0;uint64_t texels=0;
    NSMutableArray *measurements=[NSMutableArray new];NSData *crop=nil;NSDictionary *crop_info=nil;
    if(!direct)for(unsigned face=0,i=0;face<faces;face++)for(unsigned l=0;l<levels;l++,i++) {
        unsigned w=MAX(1u,width>>l),h=MAX(1u,height>>l);NSMutableData *output=[NSMutableData dataWithLength:((w+3)/4)*((h+3)/4)*16];
        enum astc_encode_result result=astc_encode_level_policy(format,spans[i].bytes,w,h,output.mutableBytes,&policy);
        if(result==ASTC_ENCODE_ERROR)return NO;
        struct audit_measurement m;uint8_t crops[64*64*8];if(!audit_measure(format,spans[i].bytes,w,h,output.bytes,&policy,&m,crops))return NO;
        // Reconstructed all-boundaries acceptance uses the same candidate bytes; only the final policy differs.
        assert(m.material_pass==(result==ASTC_ENCODE_OK));old_pass&=m.old_pass;new_pass&=result==ASTC_ENCODE_OK;
        max_alpha=MAX(max_alpha,m.alpha_max);max_test=MAX(max_test,m.coverage_test);max_all=MAX(max_all,m.coverage_all);
        squared_error+=m.squared_error;channels+=m.channels;texels+=(uint64_t)w*h;
        NSDictionary *level=@{@"face":@(face),@"mip":@(l),@"width":@(w),@"height":@(h),@"binary_alpha":@(m.binary),@"rgb_psnr":@(m.psnr),@"max_alpha_error":@(m.alpha_max),@"test_crossing_fraction":@(m.coverage_test),@"all_crossing_fraction":@(m.coverage_all),@"old_pass":@(m.old_pass),@"new_pass":@(m.material_pass)};
        [measurements addObject:level];
        if(m.psnr<=min_psnr){min_psnr=m.psnr;crop=[NSData dataWithBytes:crops length:(size_t)m.crop_width*m.crop_height*8];crop_info=@{@"face":@(face),@"mip":@(l),@"x":@(m.crop_x),@"y":@(m.crop_y),@"width":@(m.crop_width),@"height":@(m.crop_height)};}
    }
    NSMutableDictionary *record=[@{@"kind":@"texture",@"map":map_name,@"tag":@(tag),@"bitmap":@(bitmap),@"key":key_text,@"format":@(source_format),@"width":@(width),@"height":@(height),@"faces":@(faces),@"mips":@(levels),@"direct":@(direct),@"known_policy":@(policy.known),@"boundaries":boundaries,@"old_pass":@(old_pass),@"new_pass":@(new_pass),@"min_rgb_psnr":@(min_psnr),@"squared_error":@(squared_error),@"rgb_channels":@(channels),@"max_alpha_error":@(max_alpha),@"test_crossing_fraction":@(max_test),@"all_crossing_fraction":@(max_all),@"texels_measured":@(texels),@"levels":measurements} mutableCopy];
    emit(record,records);
    if(!old_pass&&new_pass){BOOL duplicate=NO;for(NSDictionary *entry in worst)if([entry[@"key"] isEqual:key_text])duplicate=YES;
        if(!duplicate){record[@"crop"]=crop_info;record[@"raw"]=crop;[worst addObject:record];[worst sortUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b){return [a[@"min_rgb_psnr"] compare:b[@"min_rgb_psnr"]];}];if(worst.count>5)[worst removeLastObject];}}
    return YES;
}
static BOOL audit_map(NSString *path) {
    NSString *name=path.lastPathComponent.stringByDeletingPathExtension;
    planning=YES;alpha_policies=[NSMutableDictionary new];projected_bytes=0;
    if(!walk_map(path,@"0000000000000000000000000000000000000000000000000000000000000000",NULL,NULL,NULL))return NO;
    planning=NO;uint8_t header[2048];uint64_t size=0;FILE *map=map_open(path,header,&size);if(!map)return NO;
    uint32_t version=u32(header+4),tag_offset=u32(header+16);int64_t translation=(int64_t)tag_offset-(version==5?0x803a6000u:0x40440000u);
    uint8_t index[40];BOOL ok=read_at(map,tag_offset,index,40);int64_t instances=(int64_t)u32(index)+translation;uint32_t count=u32(index+12);
    for(uint32_t i=0;ok&&i<count;i++){@autoreleasepool {
        uint8_t instance[32];ok=read_at(map,(uint64_t)instances+i*32,instance,32);if(!ok)break;
        if(memcmp(instance,"bitm",4)&&memcmp(instance,"mtib",4))continue;if(!u32(instance+20))continue;
        if(version!=5&&u32(instance+24)){ok=NO;break;}
        int64_t group=(int64_t)u32(instance+20)+translation;uint8_t block[12];
        ok=group>=0&&(uint64_t)group+108<=size&&read_at(map,(uint64_t)group+96,block,12);if(!ok)break;
        uint32_t n=u32(block);int64_t elements=(int64_t)u32(block+4)+translation;
        ok=n<=65536&&elements>=0&&(uint64_t)elements+(uint64_t)n*48<=size;
        for(uint32_t k=0;ok&&k<n;k++){uint8_t b[48];ok=read_at(map,(uint64_t)elements+k*48,b,48)&&audit_bitmap(map,size,b,version!=5,name,i,k);}
    }}fclose(map);fprintf(stderr,"AUDIT %s %s\n",name.UTF8String,ok?"complete":"FAILED");return ok;
}
int main(int argc,const char **argv) {@autoreleasepool {
    assert(argc>=3);NSString *output=@(argv[1]);assert([NSFileManager.defaultManager createDirectoryAtPath:output withIntermediateDirectories:YES attributes:nil error:nil]);
    data_root=output;install_root=@"";line_lock=[NSLock new];worst=[NSMutableArray new];assert(astc_store_init(output.fileSystemRepresentation));
    records=fopen([[output stringByAppendingPathComponent:@"textures.jsonl"] fileSystemRepresentation],"w");assert(records);
    for(int i=2;i<argc;i++)if(!audit_map(@(argv[i])))return 1;fclose(records);
    FILE *manifest=fopen([[output stringByAppendingPathComponent:@"crops.jsonl"] fileSystemRepresentation],"w");assert(manifest);
    for(NSUInteger i=0;i<worst.count;i++){NSMutableDictionary *entry=[worst[i] mutableCopy];NSString *raw=[NSString stringWithFormat:@"worst-%lu.rgba",(unsigned long)i+1];assert([entry[@"raw"] writeToFile:[output stringByAppendingPathComponent:raw] atomically:YES]);[entry removeObjectForKey:@"raw"];entry[@"raw_file"]=raw;emit(entry,manifest);}fclose(manifest);astc_store_shutdown();return 0;
}}
