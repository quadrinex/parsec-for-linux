/* portal_token.c — portal restore token: ~/.config/parsec-linux-host/portal-<output>.token.
 * Owner: capture. Write: created directly as 0600, symlink refused (O_NOFOLLOW). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cap_internal.h"

static void token_path(const char *out, char *p, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    char dir[400], nm[PLH_NAME_MAX];
    snprintf(nm, sizeof nm, "%s", out);
    for (char *c = nm; *c; c++) if (*c == '/' || *c == ' ') *c = '_';
    if (x && *x) snprintf(dir, sizeof dir, "%s/parsec-linux-host", x);
    else snprintf(dir, sizeof dir, "%s/.config/parsec-linux-host", h ? h : "/tmp");
    snprintf(p, n, "%s/portal-%s.token", dir, nm);
}

int cap_token_load(const char *out, char *tok, size_t n)
{
    char p[512];
    token_path(out, p, sizeof p);
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return 0; }
    if (!fgets(tok, (int)n, f)) tok[0] = 0;
    fclose(f);
    tok[strcspn(tok, "\r\n")] = 0;
    return tok[0] != 0;
}

/* 0 = written, <0 = refused or failed */
int cap_token_save(const char *out, const char *tok)
{
    char p[512], d[512], par[512];
    token_path(out, p, sizeof p);
    snprintf(d, sizeof d, "%s", p);
    *strrchr(d, '/') = 0;
    snprintf(par, sizeof par, "%s", d);
    *strrchr(par, '/') = 0;
    mkdir(par, 0700);
    mkdir(d, 0700);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return -1; }
    fchmod(fd, 0600); /* pre-existing file with a wider mode */
    fprintf(f, "%s\n", tok);
    return fclose(f) ? -1 : 0;
}
