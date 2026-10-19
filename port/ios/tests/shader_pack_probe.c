#include "../host/host_shader_pack.h"
#include <stdio.h>
static void diagnostic(const char *s) { fprintf(stderr,"%s\n",s); }
int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 2;
    return host_shader_pack_validate(argv[1],argc == 3 ? argv[2] : NULL,diagnostic) ? 0 : 1;
}
