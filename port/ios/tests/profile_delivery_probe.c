/* The publisher invokes the runtime hardware-key and shard validators. */
#include "../host/host_profile.h"
#include <stdio.h>
#include <string.h>
static void diagnostic(const char *s) { fprintf(stderr,"%s\n",s); }
int main(int argc, char **argv)
{
    struct host_device d = {0}; char key[192];
    if (argc != 5 && argc != 6) return 2;
    int at = argc == 6 ? 3 : 2;
    if (strlen(argv[at]) >= sizeof(d.platform) || strlen(argv[at+1]) >= sizeof(d.model) || strlen(argv[at+2]) >= sizeof(d.chip)) return 1;
    snprintf(d.platform,sizeof(d.platform),"%s",argv[at]); snprintf(d.model,sizeof(d.model),"%s",argv[at+1]); snprintf(d.chip,sizeof(d.chip),"%s",argv[at+2]);
    if (!host_profile_device_key(&d,key,sizeof(key))) return 1;
    if (!strcmp(argv[1],"key") && argc == 5) { puts(key); return 0; }
    if (!strcmp(argv[1],"validate") && argc == 6) return host_profile_validate_device_folder(argv[2],&d,diagnostic) ? 0 : 1;
    if (!strcmp(argv[1],"select") && argc == 6) {
        struct host_profile p; d.gpu_family = 1;
        host_profile_resolve(&d,argv[2],NULL,NULL,NULL,&p);
        printf("%s\n%s\n",p.class_file,p.model_file); return 0;
    }
    return 2;
}
