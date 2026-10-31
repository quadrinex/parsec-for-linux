#define _GNU_SOURCE
/* test_token.c — portal token: mode 0600, symlink refused, read back. No portal needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../cap_internal.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    char t[] = "/tmp/plh-tokentest.XXXXXX", p[600], buf[64] = "";
    CHECK(mkdtemp(t));
    setenv("XDG_CONFIG_HOME", t, 1);
    CHECK(cap_token_save("DP-2", "abc123") == 0);
    snprintf(p, sizeof p, "%s/parsec-linux-host/portal-DP-2.token", t);
    struct stat st;
    CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(cap_token_load("DP-2", buf, sizeof buf) && !strcmp(buf, "abc123"));
    /* pre-existing file that is too open: brought back to 0600 */
    chmod(p, 0666);
    CHECK(cap_token_save("DP-2", "def") == 0 && stat(p, &st) == 0 && (st.st_mode & 0777) == 0600);
    /* pre-existing symlink: refused, target intact */
    char cible[600];
    snprintf(cible, sizeof cible, "%s/cible", t);
    FILE *f = fopen(cible, "w"); CHECK(f); fputs("intact\n", f); fclose(f);
    snprintf(p, sizeof p, "%s/parsec-linux-host/portal-HDMI-A-1.token", t);
    CHECK(symlink(cible, p) == 0);
    CHECK(cap_token_save("HDMI-A-1", "pirate") < 0);
    CHECK(!cap_token_load("HDMI-A-1", buf, sizeof buf));
    f = fopen(cible, "r"); CHECK(f); CHECK(fgets(buf, sizeof buf, f) && !strcmp(buf, "intact\n")); fclose(f);
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", t);
    if (system(cmd)) return 1;
    puts("token OK");
    return 0;
}
