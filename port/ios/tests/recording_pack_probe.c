/* The publisher invokes exactly the validator used before runtime staging. */
#include "../host/host_recording_pack.h"
#include <stdio.h>
static void diagnostic(const char *text) { fprintf(stderr, "%s\n", text); }
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: recording-pack-probe DIRECTORY\n"); return 2; }
    return host_recording_pack_validate(argv[1], diagnostic) ? 0 : 1;
}
