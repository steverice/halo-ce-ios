/* Apache-2.0. The disk walker never borrows guest memory or renderer entries. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#import "host_astc_store.h"
#import "host_map_identity.h"
#include "host_config.h"
#include "ios_host.h"
#include "astc_store.h"
#include "astc_encode.h"
#include "astc_policy.h"
#include "astc_material.h"
#include "texture_override.h"
#include "host_texture_upscale.h"
#include <zlib.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#define FLIGHT_LIMIT (16u*1024u*1024u)
static dispatch_queue_t worker;
static NSLock *line_lock;
static NSMutableArray<NSString *> *line;
static NSMutableDictionary<NSString *,NSDictionary *> *observations;
static NSString *data_root,*install_root,*priority_path,*converting_path,*converting_stamp;
static BOOL enabled,two_core,synchronous;
static atomic_bool scheduled;
static atomic_uint load_serial;
static BOOL planning;static uint64_t projected_bytes;
static NSMutableDictionary<NSData *, NSData *> *alpha_policies;
static host_astc_progress cancellation;static void *cancellation_context;static uint64_t progress_done,progress_total;
static struct {double read_seconds,encode_seconds,write_seconds;uint64_t direct_blocks,encoded_texels,encoded_textures,excluded;} metrics;
/* playing: a level (not the menus) is loaded and its game has started. The
 * worker does no encoding then, on any device; it resumes in the menus and
 * while a level loads. game_map: the map loading or loaded is a level. */
static atomic_bool playing,game_map;
/* A pass stopped by play, a priority map, cancellation or a changing file is
 * resumed; a malformed map or a full disk waits for the next scan instead of
 * looping in the menus. */
static BOOL interrupted;
static uint16_t u16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static uint32_t u32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static NSString *stamp(NSString *path) {
    struct stat s;if(stat(path.fileSystemRepresentation,&s)||!S_ISREG(s.st_mode))return nil;
    return [NSString stringWithFormat:@"%llu:%llu:%llu:%lld:%ld:%lld:%ld",(unsigned long long)s.st_dev,(unsigned long long)s.st_ino,(unsigned long long)s.st_size,(long long)s.st_mtimespec.tv_sec,s.st_mtimespec.tv_nsec,(long long)s.st_ctimespec.tv_sec,s.st_ctimespec.tv_nsec];
}
static NSString *shared_path(void) {
    NSString *local=[data_root stringByAppendingPathComponent:@"custom_maps/bitmaps.map"];
    if(stamp(local))return local;
    return install_root.length?[install_root stringByAppendingPathComponent:@"maps/bitmaps.map"]:local;
}
static NSString *normalize(const char *path) {
    if(!path||!*path)return nil;
    NSString *name=@(path);
    if(name.length>2 && [name characterAtIndex:1]==':') {
        NSString *base=[[name substringToIndex:1].lowercaseString isEqual:@"h"]?install_root:data_root;
        name=[base stringByAppendingPathComponent:[name substringFromIndex:3]];
    }
    name=[name stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
    return [(name.isAbsolutePath?name:[data_root stringByAppendingPathComponent:name]) stringByStandardizingPath];
}
static BOOL read_at(FILE *f,uint64_t offset,void *bytes,size_t size) {
    return !fseeko(f,(off_t)offset,SEEK_SET)&&fread(bytes,1,size,f)==size;
}
/* Snapshot files before mapping: Files may replace or truncate the original
 * after the stability check, and a mapping of that inode could SIGBUS. */
static FILE *snapshot(FILE *input) {
    FILE *output=tmpfile();if(!output)return NULL;
    uint8_t bytes[65536];size_t n;rewind(input);
    while((n=fread(bytes,1,sizeof(bytes),input)))if(fwrite(bytes,1,n,output)!=n){fclose(output);return NULL;}
    if(ferror(input)){fclose(output);return NULL;}fflush(output);rewind(output);return output;
}
static FILE *resource_open(NSString *path) {
    if(!path.length)return NULL;
    FILE *input=fopen(path.fileSystemRepresentation,"rb");if(!input)return NULL;
    FILE *output=snapshot(input);fclose(input);return output;
}
/* Xbox disc maps have a zlib body. Inflate to an anonymous file rather than
 * retaining a second whole map in memory on the 2 GB Apple TV. */
static FILE *map_open(NSString *path,uint8_t header[2048],uint64_t *size) {
    FILE *f=fopen(path.fileSystemRepresentation,"rb");if(!f)return NULL;
    if(!read_at(f,0,header,2048)||memcmp(header,"daeh",4)) {fclose(f);return NULL;}
    uint32_t declared=u32(header+8),version=u32(header+4);
    struct stat st;if(fstat(fileno(f),&st)||declared<2048||declared>0x40000000u){fclose(f);return NULL;}
    if(version!=5) {if(declared!=(uint64_t)st.st_size){fclose(f);return NULL;}*size=declared;FILE *copy=snapshot(f);fclose(f);return copy;}
    /* Uncompressed precached maps are also valid inputs. */
    if((uint64_t)st.st_size==declared){*size=declared;FILE *copy=snapshot(f);fclose(f);return copy;}
    FILE *out=tmpfile();if(!out){fclose(f);return NULL;}
    fwrite(header,1,2048,out);fseeko(f,2048,SEEK_SET);
    z_stream z={0};BOOL ok=inflateInit(&z)==Z_OK;int status=Z_OK;
    uint8_t input[65536],output[65536];uint64_t written=2048;
    while(ok&&status!=Z_STREAM_END) {
        size_t n=fread(input,1,sizeof(input),f);if(!n){ok=NO;break;}
        z.next_in=input;z.avail_in=(uInt)n;
        do {z.next_out=output;z.avail_out=sizeof(output);status=inflate(&z,Z_NO_FLUSH);
            size_t produced=sizeof(output)-z.avail_out;written+=produced;
            if(written>declared||fwrite(output,1,produced,out)!=produced||(status!=Z_OK&&status!=Z_STREAM_END)){ok=NO;break;}
        } while(z.avail_in&&status!=Z_STREAM_END);
    }
    if(ok)ok=written==declared&&status==Z_STREAM_END;
    inflateEnd(&z);fclose(f);if(!ok){fclose(out);return NULL;}fflush(out);*size=written;return out;
}
/* The guest's global_vector_palette (source/bitmaps/bitmaps.c), kept on the
 * host so disk decoding never reads the running game's mutable arena. */
static const uint32_t source_vector_palette[256] =
{
	0xFF7A19CC, 0xFF7E19CC, 0xFF8019CC, 0xFF8119CC, 0xFF8519CC, 0xFF742FE2, 0xFF7A2FE2, 0xFF7E2FE2,
	0xFF802FE2, 0xFF812FE2, 0xFF852FE2, 0xFF8B2FE2, 0xFF6B42ED, 0xFF7442EE, 0xFF7A42EF, 0xFF7E42EF,
	0xFF8042EF, 0xFF8142EF, 0xFF8542EF, 0xFF8B42EE, 0xFF9442ED, 0xFF6052F2, 0xFF6B52F5, 0xFF7452F6,
	0xFF7A52F7, 0xFF7E52F7, 0xFF8052F7, 0xFF8152F7, 0xFF8552F7, 0xFF8B52F6, 0xFF9452F5, 0xFF9F52F2,
	0xFF5260F2, 0xFF6060F7, 0xFF6B60F9, 0xFF7460FB, 0xFF7A60FB, 0xFF7E60FB, 0xFF8060FB, 0xFF8160FB,
	0xFF8560FB, 0xFF8B60FB, 0xFF9460F9, 0xFF9F60F7, 0xFFAD60F2, 0xFF426BED, 0xFF526BF5, 0xFF606BF9,
	0xFF6B6BFC, 0xFF746BFD, 0xFF7A6BFD, 0xFF7E6BFD, 0xFF806BFD, 0xFF816BFD, 0xFF856BFD, 0xFF8B6BFD,
	0xFF946BFC, 0xFF9F6BF9, 0xFFAD6BF5, 0xFFBD6BED, 0xFF2F74E2, 0xFF4274EE, 0xFF5274F6, 0xFF6074FB,
	0xFF6B74FD, 0xFF7474FE, 0xFF7A74FE, 0xFF7E74FE, 0xFF8074FE, 0xFF8174FE, 0xFF8574FE, 0xFF8B74FE,
	0xFF9474FD, 0xFF9F74FB, 0xFFAD74F6, 0xFFBD74EE, 0xFFD074E2, 0xFF197ACC, 0xFF2F7AE2, 0xFF427AEF,
	0xFF527AF7, 0xFF607AFB, 0xFF6B7AFD, 0xFF747AFE, 0xFF7A7AFF, 0xFF7E7AFF, 0xFF807AFF, 0xFF817AFF,
	0xFF857AFF, 0xFF8B7AFE, 0xFF947AFD, 0xFF9F7AFB, 0xFFAD7AF7, 0xFFBD7AEF, 0xFFD07AE2, 0xFFE57ACC,
	0xFF197ECC, 0xFF2F7EE2, 0xFF427EEF, 0xFF527EF7, 0xFF607EFB, 0xFF6B7EFD, 0xFF747EFE, 0xFF7A7EFF,
	0xFF7E7EFF, 0xFF807EFF, 0xFF817EFF, 0xFF857EFF, 0xFF8B7EFE, 0xFF947EFD, 0xFF9F7EFB, 0xFFAD7EF7,
	0xFFBD7EEF, 0xFFD07EE2, 0xFFE57ECC, 0xFF1980CC, 0xFF2F80E2, 0xFF4280EF, 0xFF5280F7, 0xFF6080FB,
	0xFF6B80FD, 0xFF7480FE, 0xFF7A80FF, 0xFF7E80FF, 0xFF8080FF, 0xFF8180FF, 0xFF8580FF, 0xFF8B80FE,
	0xFF9480FD, 0xFF9F80FB, 0xFFAD80F7, 0xFFBD80EF, 0xFFD080E2, 0xFFE580CC, 0xFF1981CC, 0xFF2F81E2,
	0xFF4281EF, 0xFF5281F7, 0xFF6081FB, 0xFF6B81FD, 0xFF7481FE, 0xFF7A81FF, 0xFF7E81FF, 0xFF8081FF,
	0xFF8181FF, 0xFF8581FF, 0xFF8B81FE, 0xFF9481FD, 0xFF9F81FB, 0xFFAD81F7, 0xFFBD81EF, 0xFFD081E2,
	0xFFE581CC, 0xFF1985CC, 0xFF2F85E2, 0xFF4285EF, 0xFF5285F7, 0xFF6085FB, 0xFF6B85FD, 0xFF7485FE,
	0xFF7A85FF, 0xFF7E85FF, 0xFF8085FF, 0xFF8185FF, 0xFF8585FF, 0xFF8B85FE, 0xFF9485FD, 0xFF9F85FB,
	0xFFAD85F7, 0xFFBD85EF, 0xFFD085E2, 0xFFE585CC, 0xFF2F8BE2, 0xFF428BEE, 0xFF528BF6, 0xFF608BFB,
	0xFF6B8BFD, 0xFF748BFE, 0xFF7A8BFE, 0xFF7E8BFE, 0xFF808BFE, 0xFF818BFE, 0xFF858BFE, 0xFF8B8BFE,
	0xFF948BFD, 0xFF9F8BFB, 0xFFAD8BF6, 0xFFBD8BEE, 0xFFD08BE2, 0xFF4294ED, 0xFF5294F5, 0xFF6094F9,
	0xFF6B94FC, 0xFF7494FD, 0xFF7A94FD, 0xFF7E94FD, 0xFF8094FD, 0xFF8194FD, 0xFF8594FD, 0xFF8B94FD,
	0xFF9494FC, 0xFF9F94F9, 0xFFAD94F5, 0xFFBD94ED, 0xFF529FF2, 0xFF609FF7, 0xFF6B9FF9, 0xFF749FFB,
	0xFF7A9FFB, 0xFF7E9FFB, 0xFF809FFB, 0xFF819FFB, 0xFF859FFB, 0xFF8B9FFB, 0xFF949FF9, 0xFF9F9FF7,
	0xFFAD9FF2, 0xFF60ADF2, 0xFF6BADF5, 0xFF74ADF6, 0xFF7AADF7, 0xFF7EADF7, 0xFF80ADF7, 0xFF81ADF7,
	0xFF85ADF7, 0xFF8BADF6, 0xFF94ADF5, 0xFF9FADF2, 0xFF6BBDED, 0xFF74BDEE, 0xFF7ABDEF, 0xFF7EBDEF,
	0xFF80BDEF, 0xFF81BDEF, 0xFF85BDEF, 0xFF8BBDEE, 0xFF94BDED, 0xFF74D0E2, 0xFF7AD0E2, 0xFF7ED0E2,
	0xFF80D0E2, 0xFF81D0E2, 0xFF85D0E2, 0xFF8BD0E2, 0xFF7AE5CC, 0xFF7EE5CC, 0xFF80E5CC, 0xFF81E5CC,
	0xFF85E5CC, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x008080FF,
};
struct source_walk {
    host_texture_source_callback callback;
    void *context;
    BOOL canceled;
};
static unsigned source_texel_bytes(unsigned format) {
    switch(format) {
    case 0:case 1:case 2:case 17:return 1;
    case 3:case 6:case 8:case 9:return 2;
    case 10:case 11:return 4;
    default:return 0;
    }
}
static size_t source_level_bytes(unsigned format,unsigned width,unsigned height) {
    return format>=14&&format<=16?(size_t)((width+3)/4)*((height+3)/4)*(format==14?8:16):(size_t)width*height*source_texel_bytes(format);
}
static void source_rgba(uint32_t argb,uint8_t *out) {
    out[0]=argb>>16;out[1]=argb>>8;out[2]=argb;out[3]=argb>>24;
}
static unsigned source_expand5(unsigned x) {return (x<<3)|(x>>2);}
static unsigned source_expand6(unsigned x) {return (x<<2)|(x>>4);}
static uint32_t source_565(unsigned x) {
    return 0xff000000u|source_expand5(x>>11)<<16|source_expand6((x>>5)&63)<<8|source_expand5(x&31);
}
static void source_decode_dxt(const uint8_t *input,unsigned format,unsigned width,unsigned height,uint8_t *out) {
    for(unsigned by=0;by<(height+3)/4;by++)for(unsigned bx=0;bx<(width+3)/4;bx++) {
        const uint8_t *block=input+((size_t)by*((width+3)/4)+bx)*(format==14?8:16);
        const uint8_t *color=block+(format==14?0:8);unsigned c0=u16(color),c1=u16(color+2);
        uint8_t palette[4][4];source_rgba(source_565(c0),palette[0]);source_rgba(source_565(c1),palette[1]);
        for(unsigned channel=0;channel<3;channel++) {
            palette[2][channel]=c0>c1||format!=14?(2*palette[0][channel]+palette[1][channel])/3:(palette[0][channel]+palette[1][channel])/2;
            palette[3][channel]=c0>c1||format!=14?(palette[0][channel]+2*palette[1][channel])/3:0;
        }
        palette[2][3]=255;palette[3][3]=c0>c1||format!=14?255:0;
        unsigned alpha[8]={block[0],block[1]};uint64_t alpha_bits=0;
        if(format==16) {
            for(unsigned i=0;i<6;i++)alpha_bits|=(uint64_t)block[i+2]<<(8*i);
            for(unsigned i=2;i<8;i++)alpha[i]=alpha[0]>alpha[1]?((8-i)*alpha[0]+(i-1)*alpha[1])/7:i<6?((6-i)*alpha[0]+(i-1)*alpha[1])/5:i==6?0:255;
        }
        uint32_t indices=u32(color+4);
        for(unsigned y=0;y<4&&by*4+y<height;y++)for(unsigned x=0;x<4&&bx*4+x<width;x++) {
            unsigned texel=y*4+x;uint8_t *pixel=out+((size_t)(by*4+y)*width+bx*4+x)*4;
            memcpy(pixel,palette[(indices>>(texel*2))&3],4);
            if(format==15)pixel[3]=((block[texel/2]>>((texel&1)*4))&15)*17;
            if(format==16)pixel[3]=alpha[(alpha_bits>>(texel*3))&7];
        }
    }
}
static size_t source_swizzled(unsigned x,unsigned y,unsigned width,unsigned height) {
    size_t offset=0,mask=1;
    for(unsigned bit=1;bit<width||bit<height;bit<<=1) {
        if(bit<width){if(x&bit)offset|=mask;mask<<=1;}
        if(bit<height){if(y&bit)offset|=mask;mask<<=1;}
    }
    return offset;
}
static void source_decode_texels(const uint8_t *input,unsigned format,unsigned width,unsigned height,BOOL swizzled,uint8_t *out) {
    unsigned bytes=source_texel_bytes(format);
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++) {
        const uint8_t *pixel=input+(swizzled?source_swizzled(x,y,width,height):(size_t)y*width+x)*bytes;
        uint32_t value=0;unsigned word=bytes>=2?u16(pixel):pixel[0];
        switch(format) {
        case 0:value=(uint32_t)word<<24|0xffffffu;break;
        case 1:value=0xff000000u|word*0x010101u;break;
        case 2:value=word*0x01010101u;break;
        case 3:value=(uint32_t)pixel[1]<<24|pixel[0]*0x010101u;break;
        case 6:value=source_565(word);break;
        case 8:value=(word&0x8000?0xff000000u:0)|source_expand5((word>>10)&31)<<16|source_expand5((word>>5)&31)<<8|source_expand5(word&31);break;
        case 9:value=((word>>12)*17)<<24|(((word>>8)&15)*17)<<16|(((word>>4)&15)*17)<<8|(word&15)*17;break;
        case 10:value=u32(pixel)|0xff000000u;break;
        case 11:value=u32(pixel);break;
        case 17:value=source_vector_palette[word];break;
        }
        source_rgba(value,out+((size_t)y*width+x)*4);
    }
}
static BOOL source_bitmap(FILE *pixels,uint64_t size,const uint8_t *b,BOOL pc,struct source_walk *walk) {
    struct host_texture_source source={0};
    source.width=u16(b+4);source.height=u16(b+6);source.format=u16(b+12);source.flags=u16(b+14);
    /* Match texture_policy_add_map: only non-linear 2D textures have keys. */
    if(u16(b+10)!=0||(source.flags&16))return YES;
    if(!source_level_bytes(source.format,1,1))return YES;
    if(!source.width||!source.height||source.width>4096||source.height>4096)return NO;
    if(!pc&&(source.flags&1)&&source_texel_bytes(source.format)&&
        ((source.width&(source.width-1))||(source.height&(source.height-1))))return NO;
    source.level_count=1;
    if(source.flags&1) {
        unsigned side=MAX(source.width,source.height),limit=0;
        if(source.format>=14&&source.format<=16)side=MAX(source.width/4,source.height/4);
        side=MAX(side,(unsigned)u16(b+8));while(side>1){side>>=1;limit++;}
        source.level_count=MIN((unsigned)u16(b+20),limit)+1;
    }
    if(source.level_count>13)return NO;
    size_t encoded_size=0;
    for(unsigned level=0;level<source.level_count;level++) {
        unsigned w=MAX(1u,source.width>>level),h=MAX(1u,source.height>>level);
        source.level_offsets[level]=source.rgba_size;source.rgba_size+=(size_t)w*h*4;
        encoded_size+=source_level_bytes(source.format,w,h);
    }
    uint64_t offset=u32(b+24);
    if(offset>size||encoded_size>size-offset||encoded_size>u32(b+28))return NO;
    uint8_t *encoded=malloc(encoded_size),*rgba=malloc(source.rgba_size);BOOL ok=encoded&&rgba&&read_at(pixels,offset,encoded,encoded_size);
    if(ok) {
        source.hash=texture_override_hash(encoded,source_level_bytes(source.format,source.width,source.height));
        size_t at=0;
        for(unsigned level=0;level<source.level_count;level++) {
            unsigned w=MAX(1u,source.width>>level),h=MAX(1u,source.height>>level);
            if(source.format>=14&&source.format<=16)source_decode_dxt(encoded+at,source.format,w,h,rgba+source.level_offsets[level]);
            else source_decode_texels(encoded+at,source.format,w,h,!pc&&(source.flags&1),rgba+source.level_offsets[level]);
            at+=source_level_bytes(source.format,w,h);
        }
        source.rgba=rgba;
        if(!walk->callback(walk->context,&source)){walk->canceled=YES;ok=NO;}
    }
    free(encoded);free(rgba);return ok;
}
static BOOL convert_bitmap(FILE *pixels,uint64_t size,const uint8_t *b,BOOL pc,NSString *fingerprint,uint64_t *stored,const struct astc_alpha_policy *policy) {
    unsigned width=u16(b+4),height=u16(b+6),type=u16(b+10),source_format=u16(b+12),flags=u16(b+14),levels=1;
    /* Match rasterizer_xbox_bitmap_get_max_mipmap_count: compressed guest
     * textures stop at a 4x4 block even when tag metadata lists smaller mips. */
    if((flags&1)&&!(flags&(1u<<4))){unsigned side=MAX(width/4,MAX(height/4,(unsigned)u16(b+8))),maximum=0;while(side>1){side>>=1;maximum++;}levels=MIN((unsigned)u16(b+20),maximum)+1;}
    if(type==1||source_format<14||source_format>16)return YES;
    if(!width||!height||width>4096||height>4096||levels>13)return NO;
    enum astc_dxt_format format=(enum astc_dxt_format)(source_format-14);
    unsigned faces=type==2?6:1;size_t source_face=0,output_face=0;
    for(unsigned l=0;l<levels;l++){size_t blocks=((MAX(1u,width>>l)+3)/4)*((MAX(1u,height>>l)+3)/4);source_face+=blocks*(format==ASTC_DXT1?8:16);output_face+=blocks*16;}
    size_t stride=faces==6&&!pc?(source_face+127)&~(size_t)127:source_face;
    size_t source_size=stride*(faces-1)+source_face,output_size=output_face*faces;
    uint64_t offset=u32(b+24);
    if(offset>size||source_size>size-offset||source_size>u32(b+28))return NO;
    /* File-backed views keep large community textures out of the copied-byte
     * budget. Encoder scratch is tile-bounded; temporary output is unpublished. */
    if(ASTC_ENCODER_TILE_SCRATCH+ASTC_ENCODER_CONTEXT_RESERVE>FLIGHT_LIMIT)return NO;
    size_t page=(size_t)getpagesize(),leading=(size_t)(offset%page),mapping_size=leading+source_size;
    double read_start=NSProcessInfo.processInfo.systemUptime;
    void *mapping=mmap(NULL,mapping_size,PROT_READ,MAP_PRIVATE,fileno(pixels),(off_t)(offset-leading));
    if(mapping==MAP_FAILED)return NO;
    uint8_t *source=(uint8_t *)mapping+leading,*output=NULL;FILE *output_file=NULL;
    BOOL ok=YES,direct=YES;metrics.read_seconds+=NSProcessInfo.processInfo.systemUptime-read_start;
    struct astc_store_span spans[90];size_t count=0;
    for(unsigned face=0;ok&&face<faces;face++){size_t at=face*stride;
        for(unsigned l=0;l<levels;l++){unsigned w=MAX(1u,width>>l),h=MAX(1u,height>>l);size_t bytes=((w+3)/4)*((h+3)/4)*(format==ASTC_DXT1?8:16);
            size_t position=at;if(pc&&faces==6){size_t preceding=0;for(unsigned earlier=0;earlier<l;earlier++)preceding+=((MAX(1u,width>>earlier)+3)/4)*((MAX(1u,height>>earlier)+3)/4)*(format==ASTC_DXT1?8:16);static const unsigned hardware_face[6]={0,2,1,3,4,5};position=preceding*faces+hardware_face[face]*bytes;}
            spans[count++]=(struct astc_store_span){source+position,bytes};direct&=astc_rewrite_level_supported(format,source+position,w,h);at+=bytes;}}
    struct astc_store_key key;
    struct astc_alpha_policy merged={0};
    if(!direct) {
        /* Merge identical content before publishing any record. Bound policy
         * bookkeeping; an unusually large map uses the conservative gate. */
        astc_store_key_compute(&key,format,width,height,levels,faces,spans,count);
        NSData *policy_key=[NSData dataWithBytes:&key length:sizeof(key)];
        if(planning) {
            if(policy)merged=*policy;
            NSData *prior=alpha_policies[policy_key];
            if(prior){struct astc_alpha_policy previous;[prior getBytes:&previous length:sizeof(previous)];merged.known&=previous.known;for(unsigned r=0;r<32;r++)merged.boundaries[r]|=previous.boundaries[r];}
            if(alpha_policies.count>=8192&&!prior)alpha_policies=nil;
            alpha_policies[policy_key]=[NSData dataWithBytes:&merged length:sizeof(merged)];
        } else {
            NSData *combined=alpha_policies[policy_key];
            if(combined)[combined getBytes:&merged length:sizeof(merged)];
            policy=&merged;
        }
    }
    if(ok&&direct&&!planning)metrics.direct_blocks+=output_size/16;
    if(ok&&!direct&&planning){struct astc_store_record existing={0};
        if(!astc_store_is_excluded(fingerprint.UTF8String,&key)){if(astc_store_lookup_map(fingerprint.UTF8String,&key,&existing))astc_store_release(&existing);else projected_bytes+=output_size;}}

    if(ok&&!direct&&!planning){
        struct astc_store_record record={0};
        if(astc_store_is_excluded(fingerprint.UTF8String,&key)){}
        else if(astc_store_lookup_map(fingerprint.UTF8String,&key,&record))astc_store_release(&record);
        else {output_file=tmpfile();ok=output_file&&!ftruncate(fileno(output_file),(off_t)output_size);
            if(ok){output=mmap(NULL,output_size,PROT_READ|PROT_WRITE,MAP_SHARED,fileno(output_file),0);if(output==MAP_FAILED){output=NULL;ok=NO;}}
            madvise(mapping,mapping_size,MADV_DONTNEED);
            size_t at=0,i=0;enum astc_encode_result result=ASTC_ENCODE_ERROR;
            for(unsigned face=0;ok&&face<faces;face++)for(unsigned l=0;ok&&l<levels;l++,i++){unsigned w=MAX(1u,width>>l),h=MAX(1u,height>>l);double encode_start=NSProcessInfo.processInfo.systemUptime;result=astc_encode_level_policy(format,spans[i].bytes,w,h,output+at,policy);ok=result==ASTC_ENCODE_OK;metrics.encode_seconds+=NSProcessInfo.processInfo.systemUptime-encode_start;metrics.encoded_texels+=(uint64_t)w*h;at+=((w+3)/4)*((h+3)/4)*16;}
            if(ok){double write_start=NSProcessInfo.processInfo.systemUptime;ok=astc_store_write_encoder(fingerprint.UTF8String,&key,output,output_size);metrics.write_seconds+=NSProcessInfo.processInfo.systemUptime-write_start;if(ok){*stored+=output_size;metrics.encoded_textures++;}}
            else if(result==ASTC_ENCODE_EXCLUDED){metrics.excluded++;host_logf(HOST_LOG_WARN,"ASTC: encoder quality exclusion %ux%u format %u",width,height,(unsigned)format);ok=astc_store_write_exclusion(fingerprint.UTF8String,&key); }
        }
    }
    if(output)munmap(output,output_size);if(output_file)fclose(output_file);munmap(mapping,mapping_size);
    return ok;
}
static BOOL walk_group(FILE *map,uint64_t map_size,uint64_t group,int64_t translation,FILE *resource,uint64_t resource_size,NSString *fingerprint,BOOL pc,NSString *shared,uint64_t *stored,const struct astc_alpha_policy *policy,struct source_walk *source) {
    uint8_t block[12];if(group>map_size||map_size-group<108||!read_at(map,group+96,block,12))return NO;
    uint32_t count=u32(block);if(!count)return YES;int64_t elements=(int64_t)u32(block+4)+translation;
    if(count>65536||elements<0||(uint64_t)elements>map_size||(uint64_t)count*48>map_size-(uint64_t)elements)return NO;
    BOOL ok=YES;
    for(uint32_t i=0;i<count;i++){@autoreleasepool {if(!source&&cancellation&&!cancellation(cancellation_context,progress_done,progress_total)){interrupted=YES;return NO;}uint8_t b[48];if(!read_at(map,(uint64_t)elements+i*48,b,48))return NO;
        BOOL external=(u16(b+14)&(1u<<8))!=0;
        if(external&&(!resource||(!source&&!shared))){ok=NO;continue;}
        if(source){if(!source_bitmap(external?resource:map,external?resource_size:map_size,b,pc,source))return NO;}
        else if(!convert_bitmap(external?resource:map,external?resource_size:map_size,b,pc,external?shared:fingerprint,stored,external?NULL:policy))ok=NO;
        if(source)continue;
        [line_lock lock];BOOL preempt=priority_path&&![priority_path isEqual:converting_path];[line_lock unlock];if(preempt||atomic_load(&playing)||(converting_path&&![converting_stamp isEqual:stamp(converting_path)])){interrupted=YES;return NO;}
    }}return ok;
}
static BOOL walk_map(NSString *path,NSString *fingerprint,host_astc_progress progress,void *context,struct source_walk *source) {
    uint8_t header[2048];uint64_t size=0,stored=0;double start=NSProcessInfo.processInfo.systemUptime;
    FILE *map=map_open(path,header,&size);if(!source)metrics.read_seconds+=NSProcessInfo.processInfo.systemUptime-start;if(!map)return NO;
    uint32_t version=u32(header+4),tag_offset=u32(header+16),tag_size=u32(header+20);BOOL ok=YES;
    if((version!=5&&version!=609&&version!=7)||tag_offset>size||tag_size>size-tag_offset||tag_size<40){fclose(map);return NO;}
    uint8_t index[40];if(!read_at(map,tag_offset,index,40)){fclose(map);return NO;}
    uint32_t base=version==5?0x803a6000u:0x40440000u;int64_t translation=(int64_t)tag_offset-base;
    int64_t instances=(int64_t)u32(index)+translation;uint32_t count=u32(index+12);
    if(count>65536||instances<0||(uint64_t)instances>size||(uint64_t)count*32>size-(uint64_t)instances){fclose(map);return NO;}
    struct astc_alpha_policy *policies=!source&&version==5?astc_material_collect(map,size,(uint64_t)instances,count,translation):NULL;
    NSString *shared=nil;
    NSString *resource_path=source?[path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"bitmaps.map"]:shared_path();
    if(source&&!stamp(resource_path))resource_path=shared_path();
    FILE *resource=resource_open(resource_path);uint64_t resource_size=0;uint8_t resource_header[16]={0};
    if(resource){struct stat st;if(fstat(fileno(resource),&st)||!read_at(resource,0,resource_header,16)){fclose(resource);resource=NULL;}else resource_size=st.st_size;}
    if(resource&&!source)shared=host_map_identity_resolve(resource_path.fileSystemRepresentation)[@"fingerprint"];
    for(uint32_t i=0;ok&&i<count;i++){@autoreleasepool {
        if(!source){progress_done=i;progress_total=count;}if(progress&&!progress(context,i,count)){interrupted=YES;ok=NO;break;}
        uint8_t instance[32];if(!read_at(map,(uint64_t)instances+i*32,instance,32)){ok=NO;break;}
        if(memcmp(instance,"bitm",4)&&memcmp(instance,"mtib",4))continue;
        if(version!=5&&u32(instance+24)) {
            uint32_t item=u32(instance+20),items=u32(resource_header+12),offset=u32(resource_header+8);uint8_t entry[12];
            if(!resource||item>=items||(uint64_t)offset+(uint64_t)item*12+12>resource_size||!read_at(resource,(uint64_t)offset+item*12,entry,12)){ok=NO;break;}
            uint32_t group=u32(entry+8);ok=walk_group(resource,resource_size,group,group,resource,resource_size,shared,YES,shared,&stored,NULL,source);
        }else {if(!u32(instance+20))continue;int64_t group=(int64_t)u32(instance+20)+translation;
            if(group<0){ok=NO;break;}ok=walk_group(map,size,(uint64_t)group,translation,resource,resource_size,fingerprint,version!=5,shared,&stored,policies?&policies[i]:NULL,source);}
        if(source)continue;
        [line_lock lock];BOOL preempt=priority_path&&![priority_path isEqual:path];[line_lock unlock];
        if(preempt||atomic_load(&playing)){interrupted=YES;ok=NO;break;}
    }}
    free(policies);if(resource)fclose(resource);fclose(map);
    if(!source)host_logf(HOST_LOG_INFO,"ASTC: map preparation %s, %.3f seconds, %llu bytes stored",ok?"finished":"paused",NSProcessInfo.processInfo.systemUptime-start,(unsigned long long)stored);
    return ok;
}
static BOOL walk_resource(NSString *path,NSString *fingerprint) {
    BOOL ok;
    {
        FILE *f=resource_open(path);struct stat st;uint8_t h[16];uint64_t stored=0;ok=f&&!fstat(fileno(f),&st)&&read_at(f,0,h,16)&&u32(h)==1;
        uint32_t count=ok?u32(h+12):0,offset=ok?u32(h+8):0;
        if(count>1048576||(uint64_t)offset+(uint64_t)count*12>(uint64_t)(ok?st.st_size:0))ok=NO;
        for(uint32_t i=0;ok&&i<count;i++){uint8_t item[12];ok=read_at(f,(uint64_t)offset+i*12,item,12);if(!ok)break;
            /* Resource entries alternate raw pixels and tag metadata. Only a tag has a bounded bitmap block. */
            if(u32(item+4)>=108){uint8_t block[12];uint32_t group=u32(item+8);if(read_at(f,(uint64_t)group+96,block,12)&&u32(block)>0&&u32(block)<65536&&(uint64_t)u32(block+4)+(uint64_t)u32(block)*48<=u32(item+4))
                ok=walk_group(f,st.st_size,group,group,f,st.st_size,fingerprint,YES,fingerprint,&stored,NULL,NULL);}
        }if(f)fclose(f);
    }
    return ok;
}
static BOOL prepare(NSString *path,host_astc_progress progress,void *context) {
    NSString *before=stamp(path);if(!before)return NO;
    NSDictionary *identity=host_map_identity_resolve(path.fileSystemRepresentation);NSString *fingerprint=identity[@"fingerprint"];
    if(!fingerprint)return NO;
    if(astc_store_map_complete(fingerprint.UTF8String))return YES;
    converting_path=path;converting_stamp=before;cancellation=progress;cancellation_context=context;interrupted=NO;progress_done=0;progress_total=1;memset(&metrics,0,sizeof(metrics));double began=NSProcessInfo.processInfo.systemUptime;
    BOOL ok;
    planning=YES;projected_bytes=0;alpha_policies=[NSMutableDictionary new];
    BOOL resource=[path.lastPathComponent.lowercaseString isEqual:@"bitmaps.map"];
    ok=resource?walk_resource(path,fingerprint):walk_map(path,fingerprint,progress,context,NULL);
    planning=NO;
    if(ok&&!astc_store_has_space(projected_bytes)){host_logf(HOST_LOG_WARN,"ASTC: not enough free storage for %llu projected bytes",(unsigned long long)projected_bytes);ok=NO;}
    if(ok)ok=resource?walk_resource(path,fingerprint):walk_map(path,fingerprint,progress,context,NULL);
    alpha_policies=nil;
    if(ok&&![before isEqual:stamp(path)]){interrupted=YES;ok=NO;}
    if(ok)ok=astc_store_mark_complete(fingerprint.UTF8String);
    host_logf(HOST_LOG_INFO,"ASTC map %s: complete=%d seconds=%.6f read=%.6f encode=%.6f write=%.6f rewritten_blocks=%llu encoded_textures=%llu encoded_texels=%llu excluded=%llu thermal=%ld",path.lastPathComponent.UTF8String,ok,NSProcessInfo.processInfo.systemUptime-began,metrics.read_seconds,metrics.encode_seconds,metrics.write_seconds,(unsigned long long)metrics.direct_blocks,(unsigned long long)metrics.encoded_textures,(unsigned long long)metrics.encoded_texels,(unsigned long long)metrics.excluded,(long)NSProcessInfo.processInfo.thermalState);
    return ok;
}
static void schedule(void);
static void pump(void) {
    @autoreleasepool {
        /* Gameplay pauses the worker entirely; host_astc_game_started and the
         * next level load reschedule it. Recheck after clearing the flag so a
         * load that raced with this check is not lost. */
        if(atomic_load(&playing)){atomic_store(&scheduled,false);if(!atomic_load(&playing))schedule();return;}
        [line_lock lock];NSString *path=priority_path?:line.lastObject;BOOL priority=path&&[path isEqual:priority_path];if(path)[line removeObject:path];else atomic_store(&scheduled,false);[line_lock unlock];
        if(!path)return;
        NSString *current=stamp(path);NSDictionary *prior=observations[path];double now=NSProcessInfo.processInfo.systemUptime;
        if(!current){[observations removeObjectForKey:path];[line_lock lock];if([priority_path isEqual:path])priority_path=nil;[line_lock unlock];}
        /* The game is about to read a priority map itself, so it is not mid-copy:
         * convert during loading without the arrival stability wait. prepare
         * still rejects a file whose stamp changes underneath it. */
        else if(priority){[line_lock lock];if([priority_path isEqual:path])priority_path=nil;[line_lock unlock];BOOL finished=prepare(path,NULL,NULL);[observations removeObjectForKey:path];if(!finished&&interrupted){[line_lock lock];if(![line containsObject:path])[line insertObject:path atIndex:0];[line_lock unlock];}}
        else if(![prior[@"stamp"] isEqual:current]){observations[path]=@{@"stamp":current,@"time":@(now)};[line_lock lock];[line insertObject:path atIndex:0];[line_lock unlock];}
        else if(now-[prior[@"time"] doubleValue]>=10){[line_lock lock];if([priority_path isEqual:path])priority_path=nil;[line_lock unlock];BOOL finished=prepare(path,NULL,NULL);[observations removeObjectForKey:path];if(!finished&&interrupted){[line_lock lock];if(![line containsObject:path])[line insertObject:path atIndex:0];[line_lock unlock];}}
        else {[line_lock lock];[line insertObject:path atIndex:0];[line_lock unlock];}
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),worker,^{pump();});
    }
}
static void schedule(void) {if(!atomic_exchange(&scheduled,true))dispatch_async(worker,^{pump();});}
void astc_store_map_arrived(const char *name) {
    if(!enabled)return;NSString *path=normalize(name);if(![path.pathExtension.lowercaseString isEqual:@"map"])return;
    [line_lock lock];if(![line containsObject:path])[line addObject:path];[line_lock unlock];schedule();
}
void host_astc_store_scan(void) {
    host_texture_upscale_scan();
    if(!enabled)return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool {
        NSMutableArray *fingerprints=[NSMutableArray new];
        NSMutableArray *folders=[NSMutableArray arrayWithObjects:[data_root stringByAppendingPathComponent:@"maps"],[data_root stringByAppendingPathComponent:@"custom_maps"],nil];
        if(install_root.length)[folders addObject:[install_root stringByAppendingPathComponent:@"maps"]];
        for(NSString *folder in folders)for(NSString *name in [NSFileManager.defaultManager enumeratorAtPath:folder])if([name.pathExtension.lowercaseString isEqual:@"map"]){NSString *path=[folder stringByAppendingPathComponent:name];astc_store_map_arrived(path.fileSystemRepresentation);NSDictionary *identity=host_map_identity_resolve(path.fileSystemRepresentation);if(identity[@"fingerprint"])[fingerprints addObject:identity[@"fingerprint"]];}
        const char **list=calloc(fingerprints.count,sizeof(*list));if(list){for(NSUInteger i=0;i<fingerprints.count;i++)list[i]=[fingerprints[i] UTF8String];astc_store_cleanup(list,fingerprints.count);free(list);}
    }});
}
void host_astc_store_init(const char *root) {
    static dispatch_once_t once;dispatch_once(&once,^{
        data_root=[@(root) stringByStandardizingPath];char setting[32],install[4096];
        host_config_string("paths.custom_edition","",install,sizeof(install));install_root=@(install);
        host_texture_upscale_init(root);
        host_config_string("debug.texture_astc","auto",setting,sizeof(setting));if(getenv("HALO_TEXTURE_ASTC"))snprintf(setting,sizeof(setting),"%s",getenv("HALO_TEXTURE_ASTC"));
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();BOOL bc=NO;
        if(@available(iOS 16.4,tvOS 16.4,macOS 11.0,*))bc=device.supportsBCTextureCompression;
        char compressed[16];host_config_string("display.compressed_textures","true",compressed,sizeof(compressed));if(getenv("HALO_COMPRESSED_TEXTURES"))snprintf(compressed,sizeof(compressed),"%s",getenv("HALO_COMPRESSED_TEXTURES"));
        /* astc_policy.h decides; the release flag VISR_ASTC_AUTOMATIC gates "auto". */
        enabled=astc_policy_converts(setting,bc)&&(!strcmp(compressed,"true")||!strcmp(compressed,"1"))&&[device supportsFamily:MTLGPUFamilyApple2];
        if(!enabled)return;
        char sync[16];host_config_string("debug.texture_astc_sync","false",sync,sizeof(sync));if(getenv("HALO_TEXTURE_ASTC_SYNC"))snprintf(sync,sizeof(sync),"%s",getenv("HALO_TEXTURE_ASTC_SYNC"));synchronous=!strcmp(sync,"true");
        char model[128]={0};size_t length=sizeof(model);sysctlbyname("hw.machine",model,&length,NULL,0);
        two_core=!strcmp(model,"AppleTV5,3")||!strncmp(model,"iPad6,7",7)||!strncmp(model,"iPad6,8",7)||!strncmp(model,"iPad6,3",7)||!strncmp(model,"iPad6,4",7);
        worker=dispatch_queue_create("org.visr.astc",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,two_core?QOS_CLASS_BACKGROUND:QOS_CLASS_UTILITY,0));
        line_lock=[NSLock new];line=[NSMutableArray new];observations=[NSMutableDictionary new];
        if(!astc_store_init(root)){enabled=NO;return;}
        [[NSURL fileURLWithPath:[data_root stringByAppendingPathComponent:@"astc-cache"]] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];host_map_identity_init();
#if TARGET_OS_OSX
        NSString *notification=NSApplicationWillBecomeActiveNotification;
#else
        NSString *notification=UIApplicationWillEnterForegroundNotification;
#endif
        [NSNotificationCenter.defaultCenter addObserverForName:notification object:nil queue:nil usingBlock:^(NSNotification *note){(void)note;host_astc_store_scan();}];
        host_astc_store_scan();
    });
}
static NSString *resolve_map_name(NSString *name) {
    NSString *path=normalize(name.UTF8String);if(!path.pathExtension.length) {
        NSString *base=path.lastPathComponent;path=[data_root stringByAppendingPathComponent:[NSString stringWithFormat:@"maps/%@.map",base]];
        if(!stamp(path)){path=[data_root stringByAppendingPathComponent:[NSString stringWithFormat:@"custom_maps/%@.map",base]];
            if(!stamp(path)){NSString *folder=[data_root stringByAppendingPathComponent:@"custom_maps"];NSString *prefix=[base stringByAppendingString:@"_"];for(NSString *candidate in [NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:nil])if([candidate.lowercaseString hasPrefix:prefix.lowercaseString]&&[candidate.pathExtension.lowercaseString isEqual:@"map"]){path=[folder stringByAppendingPathComponent:candidate];break;}}
            if(!stamp(path)&&install_root.length)path=[install_root stringByAppendingPathComponent:[NSString stringWithFormat:@"maps/%@.map",base]];}
    }
    return path;
}
void astc_store_map_will_load(const char *name) {
    if(!name||!*name)return;NSString *requested=@(name);
    /* Loading is not play: the worker may convert this map until the game starts. */
    atomic_store(&game_map,![requested.lastPathComponent.stringByDeletingPathExtension.lowercaseString isEqual:@"ui"]);
    atomic_store(&playing,false);
    host_texture_upscale_map_will_load(name);
    if(!enabled)return;
    unsigned serial=atomic_fetch_add(&load_serial,1)+1;
    /* A sentinel interrupts current conversion at its next texture boundary.
     * Resolution, stat and directory enumeration all stay off the game thread. */
    [line_lock lock];priority_path=[NSString stringWithFormat:@"!load-%u",serial];[line_lock unlock];
    dispatch_block_t load=^{
        if(serial!=atomic_load(&load_serial))return;
        NSString *path=resolve_map_name(requested);
        if(serial!=atomic_load(&load_serial))return;
        [line_lock lock];priority_path=path;[line removeObject:path];[line addObject:path];[line_lock unlock];
        NSDictionary *identity=host_map_identity_resolve(path.fileSystemRepresentation);
        if(serial!=atomic_load(&load_serial))return;
        astc_store_select_map([identity[@"fingerprint"] UTF8String]);
        NSString *resource=shared_path();NSDictionary *shared_identity=host_map_identity_resolve(resource.fileSystemRepresentation);
        if(serial==atomic_load(&load_serial))astc_store_select_shared_map([shared_identity[@"fingerprint"] UTF8String]);
        if(synchronous){NSString *before=stamp(path);usleep(10000000);if(before&&[before isEqual:stamp(path)])prepare(path,NULL,NULL);}
    };
    /* Only the explicitly requested screenshot barrier waits at level load. */
    if(synchronous)dispatch_sync(worker,load);else dispatch_async(worker,load);
    schedule();
}
struct import_progress_context {host_astc_progress callback;void *context;uint64_t map,total;};
static int import_texture_progress(void *opaque,uint64_t done,uint64_t total) {
    (void)done;(void)total;struct import_progress_context *progress=opaque;
    return !progress->callback||progress->callback(progress->context,progress->map,progress->total);
}
void host_astc_store_prepare_import(const char *root,host_astc_progress progress,void *context) {
    host_astc_store_init(root);if(!enabled)return;
    /* Import owns this high-priority thread; no game has started yet. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED,0);
    NSString *folder=[data_root stringByAppendingPathComponent:@"maps"];
    NSArray *names=[NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:nil];
    NSMutableArray *paths=[NSMutableArray new];for(NSString *name in names)if([name.pathExtension isEqual:@"map"])[paths addObject:[folder stringByAppendingPathComponent:name]];
    /* Observe twice even after transactional publication, matching Files copies. */
    NSMutableDictionary *before=[NSMutableDictionary new];for(NSString *path in paths)if(stamp(path))before[path]=stamp(path);
    for(unsigned i=0;i<100;i++){if(progress&&!progress(context,0,paths.count))return;usleep(100000);}
    for(NSUInteger i=0;i<paths.count;i++){if(progress&&!progress(context,i,paths.count))break;NSString *path=paths[i];__block struct import_progress_context state={progress,context,i,paths.count};if([before[path] isEqual:stamp(path)])dispatch_sync(worker,dispatch_block_create_with_qos_class(DISPATCH_BLOCK_ENFORCE_QOS_CLASS,QOS_CLASS_USER_INITIATED,0,^{prepare(path,import_texture_progress,(void *)&state);}));astc_store_map_arrived(path.fileSystemRepresentation);}
    if(progress)progress(context,paths.count,paths.count);
}

void host_astc_map_arrived(const char *path) { host_texture_upscale_scan();astc_store_map_arrived(path); }
void host_astc_map_will_load(const char *name) { astc_store_map_will_load(name); }
/* Called by game_initialize_for_new_map: a level's game begins, or the menus open. */
void host_astc_game_started(void) {
    atomic_store(&playing,atomic_load(&game_map));
    host_texture_upscale_game_started(atomic_load(&playing));
    if(!enabled)return;
    if(!atomic_load(&playing))schedule();
}

int host_texture_source_is_playing(void) {return atomic_load(&playing);}
FILE *host_texture_source_open_policy_map(const char *path,uint64_t *size,char *error,size_t error_size) {
    if(error&&error_size)error[0]=0;
    if(size)*size=0;
    if(!path||!*path||!size) {
        if(error&&error_size)snprintf(error,error_size,"map path and size output are required");
        return NULL;
    }
    @autoreleasepool {
        uint8_t header[2048];uint64_t inflated_size=0;
        FILE *file=map_open([@(path) stringByStandardizingPath],header,&inflated_size);
        if(!file) {
            if(error&&error_size)snprintf(error,error_size,"cannot open, snapshot, or inflate map");
            return NULL;
        }
        if(u32(header+4)!=5) {
            if(error&&error_size)snprintf(error,error_size,"texture policy supports Xbox cache version 5; map has version %u",u32(header+4));
            fclose(file);errno=ENOTSUP;return NULL;
        }
        *size=inflated_size;return file;
    }
}
int host_texture_source_scan(const char *root,host_texture_map_callback callback,void *context) {
    if(!root||!*root||!callback)return -1;
    @autoreleasepool {
        NSString *base=[@(root) stringByStandardizingPath];
        NSMutableArray<NSString *> *folders=[NSMutableArray arrayWithObjects:[base stringByAppendingPathComponent:@"maps"],[base stringByAppendingPathComponent:@"custom_maps"],nil];
        char install[4096];host_config_string("paths.custom_edition","",install,sizeof(install));
        if(*install)[folders addObject:[@(install) stringByAppendingPathComponent:@"maps"]];
        NSMutableSet<NSString *> *paths=[NSMutableSet new];
        for(NSString *folder in folders)for(NSString *name in [NSFileManager.defaultManager enumeratorAtPath:folder]) {
            NSString *path=[[folder stringByAppendingPathComponent:name] stringByStandardizingPath];
            if([name.pathExtension.lowercaseString isEqual:@"map"]&&![name.lastPathComponent.lowercaseString isEqual:@"bitmaps.map"]&&stamp(path))
                [paths addObject:path];
        }
        NSArray<NSString *> *sorted=[paths.allObjects sortedArrayUsingComparator:^NSComparisonResult(NSString *left,NSString *right) {
            NSComparisonResult order=[left.lastPathComponent compare:right.lastPathComponent options:NSLiteralSearch];
            return order==NSOrderedSame?[left compare:right options:NSLiteralSearch]:order;
        }];
        for(NSString *path in sorted)if(!callback(context,path.fileSystemRepresentation))return 0;
        return 1;
    }
}
int host_texture_source_walk_map(const char *path,host_texture_source_callback callback,void *context) {
    if(!path||!*path||!callback)return -1;
    @autoreleasepool {
        NSString *name=[@(path) stringByStandardizingPath],*before=stamp(name);
        if(!before)return -1;
        struct source_walk source={callback,context,NO};
        BOOL complete=walk_map(name,nil,NULL,NULL,&source);
        if(source.canceled)return 0;
        return complete&&[before isEqual:stamp(name)]?1:-1;
    }
}
