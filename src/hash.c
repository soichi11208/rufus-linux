/*
 * hash.c: compute MD5/SHA-1/SHA-256/SHA-512 in a single pass via OpenSSL EVP.
 */
#include "rufus.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void hex_of(const unsigned char *in, size_t len, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0xf];
    }
    out[len * 2] = '\0';
}

int hash_file(const char *path,
              char md5[33], char sha1[41],
              char sha256[65], char sha512[129],
              progress_cb_t cb, void *user)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    struct stat st;
    uint64_t total = (fstat(fd, &st) == 0) ? (uint64_t)st.st_size : 0;

    EVP_MD_CTX *c_md5    = EVP_MD_CTX_new();
    EVP_MD_CTX *c_sha1   = EVP_MD_CTX_new();
    EVP_MD_CTX *c_sha256 = EVP_MD_CTX_new();
    EVP_MD_CTX *c_sha512 = EVP_MD_CTX_new();

    if (!c_md5 || !c_sha1 || !c_sha256 || !c_sha512 ||
        !EVP_DigestInit_ex(c_md5,    EVP_md5(),    NULL) ||
        !EVP_DigestInit_ex(c_sha1,   EVP_sha1(),   NULL) ||
        !EVP_DigestInit_ex(c_sha256, EVP_sha256(), NULL) ||
        !EVP_DigestInit_ex(c_sha512, EVP_sha512(), NULL)) {
        rufus_log("hash_file: EVP init failed");
        EVP_MD_CTX_free(c_md5);
        EVP_MD_CTX_free(c_sha1);
        EVP_MD_CTX_free(c_sha256);
        EVP_MD_CTX_free(c_sha512);
        close(fd);
        return -1;
    }

    enum { CHUNK = 1 << 20 };
    unsigned char *buf = malloc(CHUNK);
    if (!buf) {
        EVP_MD_CTX_free(c_md5);
        EVP_MD_CTX_free(c_sha1);
        EVP_MD_CTX_free(c_sha256);
        EVP_MD_CTX_free(c_sha512);
        close(fd);
        return -1;
    }

    uint64_t seen = 0;
    ssize_t  n;
    while ((n = read(fd, buf, CHUNK)) > 0) {
        EVP_DigestUpdate(c_md5,    buf, (size_t)n);
        EVP_DigestUpdate(c_sha1,   buf, (size_t)n);
        EVP_DigestUpdate(c_sha256, buf, (size_t)n);
        EVP_DigestUpdate(c_sha512, buf, (size_t)n);
        seen += (uint64_t)n;
        if (cb && total) cb((double)seen / (double)total, "Hashing…", user);
    }
    free(buf);
    close(fd);

    unsigned char out[64];
    unsigned int  outlen = 0;

    if (md5)    { EVP_DigestFinal_ex(c_md5,    out, &outlen); hex_of(out, 16, md5);    }
    if (sha1)   { EVP_DigestFinal_ex(c_sha1,   out, &outlen); hex_of(out, 20, sha1);   }
    if (sha256) { EVP_DigestFinal_ex(c_sha256, out, &outlen); hex_of(out, 32, sha256); }
    if (sha512) { EVP_DigestFinal_ex(c_sha512, out, &outlen); hex_of(out, 64, sha512); }

    EVP_MD_CTX_free(c_md5);
    EVP_MD_CTX_free(c_sha1);
    EVP_MD_CTX_free(c_sha256);
    EVP_MD_CTX_free(c_sha512);
    return 0;
}
