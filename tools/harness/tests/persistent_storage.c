/* The production save call chain with real temporary-file I/O and injected failures. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#include <io.h>
#define open _open
#define close _close
#define read _read
#define write _write
#define lseek _lseek
#define ftruncate _chsize
#else
#include <unistd.h>
#define O_BINARY 0
#endif

typedef int boolean;
typedef unsigned char byte;
typedef void *HANDLE;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_SIZE 0xffffffffUL
#define INVALID_SET_FILE_POINTER 0xffffffffUL
#define GENERIC_READ 1
#define GENERIC_WRITE 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define CREATE_NEW 1
#define FILE_BEGIN SEEK_SET
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define GAME_STATE_FILE_SIZE 65536
#define GAME_STATE_SIZE 65536
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4
#define _error_silent 2
#define match_assert(file,line,condition) assert(condition)
#define match_vassert(file,line,condition,message) do { if (!(condition)) fprintf(stderr,"%s\n",message); } while (0)
#define csprintf(buffer, ...) (sprintf(buffer, __VA_ARGS__), buffer)
static char temporary[2048], profile_directory[256], fault[32];
static int writes, live, deletes, notes, reverts, menus, dialogs;
static boolean paused = TRUE;
static unsigned long last_error;
static byte buffer[GAME_STATE_SIZE];
static struct header { unsigned long checksum; byte padding[56]; } header;
static struct { void *base_address; struct header *header; } game_state_globals = {buffer, &header};
static int player_spawn_count = 1;
struct widget_instance { int unused; };
struct event_record { short controller_index; };
static int fd(HANDLE h) { return (int)(intptr_t)h - 1; }
static unsigned long GetLastError(void) { return last_error; }
static boolean game_state_get_persistent_storage_path(char *path) { strcpy(path,profile_directory); return TRUE; }
static void error(int priority, const char *format, ...) {
    va_list args; va_start(args,format); vfprintf(stderr,format,args); va_end(args); fputc('\n',stderr);
}
static HANDLE CreateFileA(const char *path, int access, int share, void *security, int creation, int attributes, void *template_file) {
    if (!strcmp(fault,"denied")) { last_error=ERROR_ACCESS_DENIED; return INVALID_HANDLE_VALUE; }
    if (!strcmp(fault,"race") && creation==CREATE_NEW) {
        FILE *racer=fopen(path,"wb"); assert(racer); assert(fwrite("racing save",1,11,racer)==11); fclose(racer);
    }
    int flags=O_RDWR | O_BINARY;
    if (creation==OPEN_ALWAYS) flags|=O_CREAT;
    if (creation==CREATE_NEW) flags|=O_CREAT|O_EXCL;
    int descriptor=open(path,flags,0600);
    if (descriptor<0) { last_error=errno==ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED; return INVALID_HANDLE_VALUE; }
    live++; last_error=0; return (HANDLE)(intptr_t)(descriptor+1);
}
static boolean CloseHandle(HANDLE h) { assert(live>0); assert(close(fd(h))==0); live--; return TRUE; }
static unsigned long GetFileSize(HANDLE h, unsigned long *high) {
    if (!strcmp(fault,"size")) { last_error=5; return INVALID_FILE_SIZE; }
    long position=lseek(fd(h),0,SEEK_CUR), size=lseek(fd(h),0,SEEK_END);
    assert(position>=0 && size>=0); assert(lseek(fd(h),position,SEEK_SET)==position);
    if (high) *high=!strcmp(fault,"high") ? 1 : 0;
    last_error=0; return (unsigned long)size;
}
static boolean WriteFile(HANDLE h, const void *data, unsigned long count, unsigned long *written, void *overlapped) {
    writes++; *written=0;
    if (!strcmp(fault,"write") || (!strcmp(fault,"header_write") && writes==2)) { last_error=5; return FALSE; }
    if (!strcmp(fault,"short") && count) count--;
    long result=write(fd(h),data,count); if (result<0) { last_error=5; return FALSE; }
    *written=(unsigned long)result; return TRUE;
}
static unsigned long SetFilePointer(HANDLE h, long offset, void *high, int origin) {
    if (!strcmp(fault,"seek")) { last_error=5; return INVALID_SET_FILE_POINTER; }
    long result=lseek(fd(h),offset,origin); return result<0 ? INVALID_SET_FILE_POINTER : (unsigned long)result;
}
static boolean SetEndOfFile(HANDLE h) {
    if (!strcmp(fault,"truncate")) { last_error=5; return FALSE; }
    return ftruncate(fd(h),lseek(fd(h),0,SEEK_CUR))==0;
}
static void delete_persistent_storage(void) { deletes++; }
static void crc_new(unsigned long *checksum) { *checksum=0; }
static void crc_checksum_buffer(unsigned long *checksum, void *data, long size) { *checksum=123; }
static void game_state_note_event(const char *event) { notes++; }
static void game_state_revert(void) {
    reverts++; paused=FALSE; memset(buffer,'c',sizeof(buffer));
    if (!strcmp(fault,"replace")) {
        char source[300], destination[300];
        snprintf(source,sizeof(source),"%ssavegame.bin",profile_directory);
        snprintf(destination,sizeof(destination),"%soriginal.bin",profile_directory);
        assert(rename(source,destination)==0);
        FILE *replacement=fopen(source,"wb"); assert(replacement);
        assert(fwrite("replacement",1,11,replacement)==11); fclose(replacement);
    }
}
static boolean network_coop_active(void) { return FALSE; }
static void network_game_client_local_player_quit(short index) {}
static long main_get_current_solo_level(void) { return 0; }
static long player_ui_get_active_player_profile_index(short player) { (void)player; return 0; }
static void main_goto_main_menu(void) { menus++; }
static void game_time_set_paused(boolean value) { paused=value; }
static void display_error_text_deferred(const wchar_t *message, short controller) { assert(message && *message); dialogs++; }
/* PRODUCTION FUNCTIONS */
int main(int argc, char **argv) {
    (void)temporary; assert(argc==4); assert(strlen(argv[1])<sizeof(profile_directory));
    strcpy(profile_directory,argv[1]); snprintf(fault,sizeof(fault),"%s",argv[3]);
    int ok;
    if (!strcmp(argv[2],"quit")) {
        struct event_record event={0}; boolean deleted=FALSE;
        ok=pause_game_quit_to_main_menu(NULL,&event,&deleted);
    } else {
        HANDLE file=game_state_open_persistent_storage(!strcmp(argv[2],"directory") ? profile_directory : NULL); ok=file!=INVALID_HANDLE_VALUE;
        if (ok) CloseHandle(file);
    }
    printf("ok=%d writes=%d live=%d deletes=%d notes=%d reverts=%d menus=%d dialogs=%d paused=%d\n",
        ok,writes,live,deletes,notes,reverts,menus,dialogs,paused);
    return 0;
}
