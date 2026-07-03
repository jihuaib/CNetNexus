/**
 * @file   syslog_report.c
 * @brief  Lightweight local/remote syslog reporter.
 */
#include "syslog_report.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define SYSLOG_MSG_MAX 1024
#define SYSLOG_PACKET_MAX 1400
#define SYSLOG_DEFAULT_IDENT "netnexus"
#define SYSLOG_DEFAULT_PORT 514u

static pthread_mutex_t g_syslog_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_syslog_initialized = 0;
static char g_syslog_ident[64] = SYSLOG_DEFAULT_IDENT;
static syslog_report_remote_config_t g_syslog_remote = {0};

static int env_false(const char *v)
{
    return (v && (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0)) ? 1 : 0;
}

static int env_true(const char *v)
{
    return (v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0)) ? 1 : 0;
}

static uint16_t parse_port_env(const char *v)
{
    if (!v || v[0] == '\0')
    {
        return SYSLOG_DEFAULT_PORT;
    }
    char *end = NULL;
    unsigned long port = strtoul(v, &end, 10);
    if (!end || *end != '\0' || port == 0 || port > 65535)
    {
        return SYSLOG_DEFAULT_PORT;
    }
    return (uint16_t)port;
}

static void syslog_report_lazy_init_locked(void)
{
    if (g_syslog_initialized)
    {
        return;
    }

    const char *ident = getenv("NN_SYSLOG_IDENT");
    if (ident && ident[0] != '\0')
    {
        snprintf(g_syslog_ident, sizeof(g_syslog_ident), "%s", ident);
    }
    openlog(g_syslog_ident, LOG_PID | LOG_NDELAY, LOG_LOCAL0);

    if (!env_false(getenv("NN_SYSLOG_ENABLE")) && !env_true(getenv("NN_SYSLOG_DISABLE")))
    {
        const char *server = getenv("NN_SYSLOG_REMOTE");
        if (!server || server[0] == '\0')
        {
            server = getenv("NN_SYSLOG_SERVER");
        }
        if (server && server[0] != '\0')
        {
            g_syslog_remote.enabled = 1u;
            g_syslog_remote.port = parse_port_env(getenv("NN_SYSLOG_PORT"));
            snprintf(g_syslog_remote.server, sizeof(g_syslog_remote.server), "%s", server);
        }
    }

    g_syslog_initialized = 1;
}

void syslog_report_init(const char *ident)
{
    pthread_mutex_lock(&g_syslog_lock);
    if (ident && ident[0] != '\0')
    {
        snprintf(g_syslog_ident, sizeof(g_syslog_ident), "%s", ident);
    }
    if (!g_syslog_initialized)
    {
        syslog_report_lazy_init_locked();
    }
    pthread_mutex_unlock(&g_syslog_lock);
}

void syslog_report_set_remote(const char *server, uint16_t port)
{
    pthread_mutex_lock(&g_syslog_lock);
    syslog_report_lazy_init_locked();
    if (!server || server[0] == '\0' || port == 0)
    {
        memset(&g_syslog_remote, 0, sizeof(g_syslog_remote));
    }
    else
    {
        g_syslog_remote.enabled = 1u;
        g_syslog_remote.port = port;
        snprintf(g_syslog_remote.server, sizeof(g_syslog_remote.server), "%s", server);
    }
    pthread_mutex_unlock(&g_syslog_lock);
}

void syslog_report_disable_remote(void)
{
    syslog_report_set_remote(NULL, 0);
}

void syslog_report_get_remote(syslog_report_remote_config_t *out)
{
    if (!out)
    {
        return;
    }
    pthread_mutex_lock(&g_syslog_lock);
    syslog_report_lazy_init_locked();
    *out = g_syslog_remote;
    pthread_mutex_unlock(&g_syslog_lock);
}

static int syslog_priority(syslog_report_severity_t severity)
{
    switch (severity)
    {
        case SYSLOG_REPORT_DEBUG:
            return LOG_DEBUG;
        case SYSLOG_REPORT_INFO:
            return LOG_INFO;
        case SYSLOG_REPORT_NOTICE:
            return LOG_NOTICE;
        case SYSLOG_REPORT_WARNING:
            return LOG_WARNING;
        case SYSLOG_REPORT_ERROR:
            return LOG_ERR;
        default:
            return LOG_INFO;
    }
}

static int rfc3164_severity(syslog_report_severity_t severity)
{
    switch (severity)
    {
        case SYSLOG_REPORT_DEBUG:
            return 7;
        case SYSLOG_REPORT_INFO:
            return 6;
        case SYSLOG_REPORT_NOTICE:
            return 5;
        case SYSLOG_REPORT_WARNING:
            return 4;
        case SYSLOG_REPORT_ERROR:
            return 3;
        default:
            return 6;
    }
}

static void send_udp_syslog(const syslog_report_remote_config_t *remote, const char *packet, size_t len)
{
    if (!remote || !remote->enabled || remote->server[0] == '\0' || !packet || len == 0)
    {
        return;
    }

    char port[16];
    snprintf(port, sizeof(port), "%u", (unsigned)remote->port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *res = NULL;
    if (getaddrinfo(remote->server, port, &hints, &res) != 0)
    {
        return;
    }

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
    {
        int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        ssize_t n = sendto(fd, packet, len, 0, ai->ai_addr, ai->ai_addrlen);
        close(fd);
        if (n >= 0)
        {
            break;
        }
    }
    freeaddrinfo(res);
}

void syslog_report(syslog_report_severity_t severity, const char *module, const char *event, const char *fmt, ...)
{
    if (!fmt)
    {
        return;
    }

    char msg[SYSLOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    syslog_report_remote_config_t remote;
    char ident[sizeof(g_syslog_ident)];
    pthread_mutex_lock(&g_syslog_lock);
    syslog_report_lazy_init_locked();
    remote = g_syslog_remote;
    snprintf(ident, sizeof(ident), "%s", g_syslog_ident);
    pthread_mutex_unlock(&g_syslog_lock);

    const char *mod = (module && module[0] != '\0') ? module : "unknown";
    const char *evt = (event && event[0] != '\0') ? event : "event";
    syslog(syslog_priority(severity), "[%s][%s] %s", mod, evt, msg);

    if (!remote.enabled)
    {
        return;
    }

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%b %e %H:%M:%S", &tmv);

    int pri = LOG_LOCAL0 + rfc3164_severity(severity);
    char packet[SYSLOG_PACKET_MAX];
    int n = snprintf(packet, sizeof(packet), "<%d>%s %s %s/%s: %s", pri, ts, ident, mod, evt, msg);
    if (n <= 0)
    {
        return;
    }
    size_t len = (n < (int)sizeof(packet)) ? (size_t)n : sizeof(packet) - 1u;
    send_udp_syslog(&remote, packet, len);
}
