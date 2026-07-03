/**
 * @file   syslog_report.h
 * @brief  Lightweight syslog reporting helpers.
 */
#ifndef SYSLOG_REPORT_H
#define SYSLOG_REPORT_H

#include <stddef.h>
#include <stdint.h>

#define SYSLOG_REPORT_SERVER_MAX 128

typedef enum syslog_report_severity
{
    SYSLOG_REPORT_DEBUG = 0,
    SYSLOG_REPORT_INFO = 1,
    SYSLOG_REPORT_NOTICE = 2,
    SYSLOG_REPORT_WARNING = 3,
    SYSLOG_REPORT_ERROR = 4,
} syslog_report_severity_t;

typedef struct syslog_report_remote_config
{
    uint32_t enabled; /**< 1=send UDP remote syslog, 0=disabled */
    uint32_t port;    /**< UDP destination port, host byte order */
    char server[SYSLOG_REPORT_SERVER_MAX];
} syslog_report_remote_config_t;

/**
 * @brief Initialize syslog reporting. Usually optional; lazy init uses NN_SYSLOG_IDENT or "netnexus".
 */
void syslog_report_init(const char *ident);

void syslog_report_set_remote(const char *server, uint16_t port);
void syslog_report_disable_remote(void);
void syslog_report_get_remote(syslog_report_remote_config_t *out);

/**
 * @brief Send one syslog report to local syslog and optional UDP remote.
 *
 * Remote UDP is enabled by NN_SYSLOG_REMOTE or NN_SYSLOG_SERVER. NN_SYSLOG_PORT defaults to 514.
 * Set NN_SYSLOG_ENABLE=0/false/no or NN_SYSLOG_DISABLE=1 to disable reporting.
 */
void syslog_report(syslog_report_severity_t severity, const char *module, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#endif /* SYSLOG_REPORT_H */
