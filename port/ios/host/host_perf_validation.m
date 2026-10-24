/* Apache-2.0. Bounded streaming JSON grammar and duplicated CloudKit-field check. */
#import "host_perf_validation.h"
#import <CommonCrypto/CommonDigest.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <zlib.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
typedef struct reader {
    gzFile file;uint8_t bytes[16384];size_t position,count,total;int pushed;BOOL valid;
    CC_SHA256_CTX hash;__unsafe_unretained NSDictionary *metadata;uint32_t seen;
} reader;
static int next(reader *r) {
    if(r->pushed>=0){int c=r->pushed;r->pushed=-1;return c;}
    if(r->position==r->count){int n=gzread(r->file,r->bytes,sizeof(r->bytes));if(n<=0){if(n<0)r->valid=NO;return -1;}
        r->position=0;r->count=(size_t)n;r->total+=(size_t)n;if(r->total>32*1024*1024){r->valid=NO;return -1;}CC_SHA256_Update(&r->hash,r->bytes,(CC_LONG)n);
    }return r->bytes[r->position++];
}
static int token(reader *r) {int c;do{c=next(r);}while(c==' ' || c=='\t' || c=='\r' || c=='\n');return c;}
static BOOL string(reader *r,char *out,size_t capacity) {
    size_t length=0;int c;
    while((c=next(r))>=0 && c!='"'){
        if(c<32)return NO;
        if(c>=128){int lead=c,following=c>=0xc2 && c<=0xdf?1:c>=0xe0 && c<=0xef?2:c>=0xf0 && c<=0xf4?3:0;if(!following)return NO;
            if(out && length+1<capacity)out[length]=(char)c;length++;
            for(int i=0;i<following;i++){c=next(r);if(c<0x80 || c>0xbf || (!i && ((lead==0xe0 && c<0xa0) || (lead==0xed && c>0x9f) || (lead==0xf0 && c<0x90) || (lead==0xf4 && c>0x8f))))return NO;if(out && length+1<capacity)out[length]=(char)c;length++;}continue;
        }
        if(c=='\\'){c=next(r);if(c=='u'){unsigned code=0;for(int i=0;i<4;i++){int digit=next(r);if(!isxdigit(digit))return NO;code=code*16+(digit<='9'?digit-'0':tolower(digit)-'a'+10);}if(code>=0xdc00 && code<=0xdfff)return NO;if(code>=0xd800 && code<=0xdbff){if(next(r)!='\\' || next(r)!='u')return NO;unsigned low=0;for(int i=0;i<4;i++){int digit=next(r);if(!isxdigit(digit))return NO;low=low*16+(digit<='9'?digit-'0':tolower(digit)-'a'+10);}if(low<0xdc00 || low>0xdfff)return NO;}c='?';}
            else if(c!='"' && c!='\\' && c!='/' && c!='b' && c!='f' && c!='n' && c!='r' && c!='t')return NO;
        }
        if(out && length+1<capacity)out[length]=(char)c;length++;
    }
    if(out){if(length>=capacity)return NO;out[length]=0;}return c=='"';
}
static BOOL value(reader *,int,unsigned);
static BOOL container(reader *r,int c,unsigned depth) {
    if(depth>32)return NO;BOOL object=c=='{';int end=object?'}':']';c=token(r);if(c==end)return YES;r->pushed=c;
    uint32_t fields=0;
    for(;;){char key[128]={0};if(object){if(token(r)!='"' || !string(r,key,sizeof(key)) || token(r)!=':')return NO;}
        int first=token(r);id expected=nil;int index=-1;
        if(object && depth==0){
            NSArray *allowed=@[@"schema",@"nonce",@"durationMS",@"frameCount",@"model",@"platform",@"profileID",@"mapFingerprint",@"renderer",@"mode",@"role",@"playerCount",@"localPlayerCount",@"width",@"height",@"refreshMilliHz",@"output",@"appVersion",@"appBuild",@"guestSHA256",@"mapFormat",@"os",@"profileHash",@"graphics",@"timingSemantics",@"networkVersion",@"journalABI",@"journalFrameSize",@"resumed",@"interrupted",@"endReason",@"abnormal",@"truncated",@"recoveryGap",@"droppedFrames",@"droppedEvents",@"droppedDetail",@"droppedGraphics",@"gpuTimestampIntervalsDropped",@"summary",@"traceSummary",@"gpuSummaryCoverage",@"trace",@"epochs",@"events",@"networkSamples",@"windows",@"overflow",@"gaps"];
            if(![allowed containsObject:@(key)])return NO;
            NSArray *names=@[@"schema",@"durationMS",@"frameCount",@"model",@"platform",@"profileID",@"mapFingerprint",@"renderer",@"mode",@"appVersion",@"appBuild",@"guestSHA256"];
            NSUInteger found=[names indexOfObject:@(key)];if(found!=NSNotFound){index=(int)found;if(fields&(1u<<index))return NO;fields|=1u<<index;expected=r->metadata[names[found]];if(!expected)return NO;}
        }
        if(expected){if([expected isKindOfClass:NSString.class]){char text[512];if(first!='"' || !string(r,text,sizeof(text)) || ![expected isEqual:@(text)])return NO;}
            else if([expected isKindOfClass:NSNumber.class]){if(CFGetTypeID((__bridge CFTypeRef)expected)==CFBooleanGetTypeID() || CFNumberIsFloatType((__bridge CFNumberRef)expected) || [expected unsignedLongLongValue]>INT64_MAX)return NO;char number[32];size_t n=0;int digit=first;if(digit<'0' || digit>'9')return NO;do{if(n+1>=sizeof(number))return NO;number[n++]=(char)digit;digit=next(r);}while(digit>='0' && digit<='9');number[n]=0;r->pushed=digit;if(n>1 && number[0]=='0')return NO;errno=0;unsigned long long parsed=strtoull(number,NULL,10);if(errno==ERANGE || parsed!=[expected unsignedLongLongValue])return NO;}
            else return NO;
        }else if(!value(r,first,depth+1))return NO;
        c=token(r);if(c==end)break;if(c!=',')return NO;
    }
    if(depth==0)r->seen=fields;return YES;
}
static BOOL value(reader *r,int c,unsigned depth) {
    if(c=='{' || c=='[')return container(r,c,depth);
    if(c=='"')return string(r,NULL,0);
    if(c=='t' || c=='f' || c=='n'){const char *tail=c=='t'?"rue":c=='f'?"alse":"ull";while(*tail)if(next(r)!=*tail++)return NO;return YES;}
    char number[128];size_t n=0;while(c=='-' || c=='+' || c=='.' || c=='e' || c=='E' || (c>='0' && c<='9')){if(n+1>=sizeof(number))return NO;number[n++]=(char)c;c=next(r);}r->pushed=c;number[n]=0;
    if(!n)return NO;size_t i=number[0]=='-'?1:0;if(i>=n)return NO;
    if(number[i]=='0')i++;else {if(number[i]<'1' || number[i]>'9')return NO;while(i<n && isdigit((unsigned char)number[i]))i++;}
    if(i<n && number[i]=='.'){i++;size_t start=i;while(i<n && isdigit((unsigned char)number[i]))i++;if(i==start)return NO;}
    if(i<n && (number[i]=='e' || number[i]=='E')){i++;if(i<n && (number[i]=='+' || number[i]=='-'))i++;size_t start=i;while(i<n && isdigit((unsigned char)number[i]))i++;if(i==start)return NO;}
    return i==n;
}
BOOL host_perf_validate_asset(NSString *path,NSDictionary *metadata,NSString *hash) {
    if(![path isKindOfClass:NSString.class] || !path.length || ![metadata isKindOfClass:NSDictionary.class])return NO;
    NSDictionary *attributes=[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];if(![attributes[NSFileType] isEqual:NSFileTypeRegular])return NO;NSNumber *size=attributes[NSFileSize];
    if(!size || size.unsignedLongLongValue>4*1024*1024 || ![metadata[@"schema"] isEqual:@1] ||
       ![hash isKindOfClass:NSString.class] || hash.length!=64 || [hash rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location!=NSNotFound)return NO;
    reader r={.pushed=-1,.valid=YES,.metadata=metadata};r.file=gzopen(path.fileSystemRepresentation,"rb");if(!r.file)return NO;CC_SHA256_Init(&r.hash);
    BOOL valid=token(&r)=='{' && container(&r,'{',0) && token(&r)==-1 && r.valid && r.seen==4095;
    if(gzclose(r.file)!=Z_OK)valid=NO;uint8_t digest[32];CC_SHA256_Final(digest,&r.hash);NSMutableString *actual=[NSMutableString string];for(int i=0;i<32;i++)[actual appendFormat:@"%02x",digest[i]];
    return valid && [actual isEqual:hash];
}
