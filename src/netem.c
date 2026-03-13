#include "netem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

int netem_apply(netem_config_t *nc)
{
    if (!nc->iface[0])
        return -1;

    /* Remove any existing qdisc first */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "tc qdisc del dev %s root >/dev/null 2>&1", nc->iface);
    system(cmd);

    /* Build the netem command */
    int pos = snprintf(cmd, sizeof(cmd),
                       "tc qdisc add dev %s root netem", nc->iface);

    if (nc->loss_percent > 0)
        pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                        " loss %.2f%%", nc->loss_percent);

    if (nc->delay_ms > 0) {
        pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                        " delay %dms", nc->delay_ms);
        if (nc->jitter_ms > 0)
            pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                            " %dms", nc->jitter_ms);
    }

    if (nc->reorder_percent > 0)
        pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                        " reorder %.2f%%", nc->reorder_percent);

    if (nc->duplicate_percent > 0)
        pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                        " duplicate %.2f%%", nc->duplicate_percent);

    if (nc->corrupt_percent > 0)
        pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                        " corrupt %.2f%%", nc->corrupt_percent);

    snprintf(cmd + pos, sizeof(cmd) - pos, " >/dev/null 2>&1");

    int ret = system(cmd);
    nc->active = (WIFEXITED(ret) && WEXITSTATUS(ret) == 0);
    return nc->active ? 0 : -1;
}

int netem_clear(const char *iface)
{
    if (!iface || !iface[0])
        return -1;

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "tc qdisc del dev %s root >/dev/null 2>&1", iface);
    int ret = system(cmd);
    return (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) ? 0 : -1;
}

int netem_query(const char *iface, char *buf, int buflen)
{
    if (!iface || !iface[0] || !buf || buflen < 1)
        return -1;

    buf[0] = '\0';

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "tc -s qdisc show dev %s 2>/dev/null", iface);

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;

    int total = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && total < buflen - 1) {
        int n = snprintf(buf + total, buflen - total, "%s", line);
        if (n > 0) total += n;
    }

    pclose(fp);
    return total;
}
