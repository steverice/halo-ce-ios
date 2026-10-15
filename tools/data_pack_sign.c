/* Offline Ed25519 signing only. No release seed is stored in the application or repository. */
#include "monocypher-ed25519.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned char *read_bounded(const char *path, size_t maximum, size_t *size, int private_key)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    unsigned char *data = NULL, extra;
    size_t at = 0;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > maximum ||
        (private_key && (st.st_size != 32 || st.st_uid != getuid() || st.st_nlink != 1 || ((st.st_mode & 07777) != 0600 && (st.st_mode & 07777) != 0400)))) goto done;
    data = malloc((size_t)st.st_size + 1);
    if (!data) goto done;
    while (at < (size_t)st.st_size) {
        ssize_t count = read(fd, data + at, (size_t)st.st_size - at);
        if (count <= 0) goto bad;
        at += (size_t)count;
    }
    if (read(fd, &extra, 1) != 0) goto bad;
    *size = at;
    goto done;
bad:
    crypto_wipe(data, (size_t)st.st_size); free(data); data = NULL;
done:
    close(fd); return data;
}
static int decode_public(const char *hex, uint8_t public_key[32])
{
    size_t i;
    if (strlen(hex) != 64 || strspn(hex, "0123456789abcdef") != 64) return 0;
    for (i = 0; i < 32; i++) {
        char pair[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        public_key[i] = (uint8_t)strtoul(pair, NULL, 16);
    }
    return 1;
}
int main(int argc, char **argv)
{
    unsigned char *seed = NULL, *message = NULL, *signature_file = NULL;
    uint8_t secret[64] = {0}, public_key[32] = {0}, signature[64] = {0};
    size_t size = 0, key_size = 0, signature_size = 0;
    int ok = 0, i;
    if (argc == 5 && !strcmp(argv[1], "--verify")) {
        if (!decode_public(argv[2], public_key)) goto done;
        message = read_bounded(argv[3], 65536, &size, 0);
        signature_file = read_bounded(argv[4], 64, &signature_size, 0);
        if (message && signature_file && signature_size == 64)
            ok = crypto_ed25519_check(signature_file, public_key, message, size) == 0;
        goto done;
    }
    if (!((argc == 3 && !strcmp(argv[1], "--public-key")) || (argc == 4 && !strcmp(argv[1], "--sign")))) {
        fprintf(stderr, "usage: data-pack-sign --public-key SEED | --sign SEED MANIFEST | --verify PUBLIC_HEX MANIFEST SIGNATURE\n");
        goto done;
    }
    seed = read_bounded(argv[2], 32, &key_size, 1);
    if (!seed || key_size != 32) {
        fprintf(stderr, "signing seed must be a private, owned, regular 32-byte file with no links or group/other permissions\n");
        goto done;
    }
    crypto_ed25519_key_pair(secret, public_key, seed);
    if (argc == 3) {
        for (i = 0; i < 32; i++) printf("%02x", public_key[i]);
        ok = putchar('\n') != EOF && !ferror(stdout);
    } else {
        message = read_bounded(argv[3], 65536, &size, 0);
        if (!message) goto done;
        crypto_ed25519_sign(signature, secret, message, size);
        ok = fwrite(signature, 1, 64, stdout) == 64 && fflush(stdout) == 0;
    }
done:
    if (seed) { crypto_wipe(seed, key_size); free(seed); }
    crypto_wipe(secret, sizeof(secret)); crypto_wipe(signature, sizeof(signature));
    free(message); free(signature_file);
    return ok ? 0 : 1;
}
