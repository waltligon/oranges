/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <syslog.h>
#include <stdlib.h>
#include "pvfs2-config.h"
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#include "gossip.h"

/* controls whether debugging is on or off */
int gossip_debug_on = 0;

/* controls the mask level for debugging messages */
uint64_t gossip_debug_mask = 0;

enum
{
    GOSSIP_STDERR = 1,
    GOSSIP_FILE = 2,
    GOSSIP_SYSLOG = 4
};

enum
{
    GOSSIP_BUF_SIZE = 128
};

/* determines which logging facility to use */
/* default to stderr to begin with */
int gossip_facility = GOSSIP_STDERR;

/* file handle used for file logging */
static FILE *internal_log_file = NULL;

/* syslog priority setting */
static int internal_syslog_priority = LOG_USER;

/*****************************************************************
 * prototypes
 */
static int gossip_debug_stderr(
    const char *format,
    va_list ap);
static int gossip_err_stderr(
    const char *format,
    va_list ap);
static int gossip_disable_stderr(
    void);

static int gossip_debug_file(
    const char *format,
    va_list ap);
static int gossip_err_file(
    const char *format,
    va_list ap);
static int gossip_disable_file(
    void);

static int gossip_debug_syslog(
    const char *format,
    va_list ap);
static int gossip_err_syslog(
    const char *format,
    va_list ap);
static int gossip_disable_syslog(
    void);


/*****************************************************************
 * visible functions
 */

/* gossip_enable_syslog()
 * 
 * Turns on syslog logging facility.  The priority argument is a
 * combination of the facility and level to use, as seen in the
 * syslog(3) man page.
 *
 * returns 0 on success, -errno on failure
 */
int gossip_enable_syslog(
    int priority)
{

    /* keep up with the existing logging settings */
    int tmp_debug_on = gossip_debug_on;
    uint64_t tmp_debug_mask = gossip_debug_mask;

    /* turn off any running facility */
    gossip_disable();

    internal_syslog_priority = priority;
    gossip_facility = GOSSIP_SYSLOG;

    /* restore the logging settings */
    gossip_debug_on = tmp_debug_on;
    gossip_debug_mask = tmp_debug_mask;

    return 0;
}

/* gossip_enable_stderr()
 *
 * Turns on logging to stderr.
 *
 * returns 0 on success, -errno on failure
 */
int gossip_enable_stderr(
    void)
{

    /* keep up with the existing logging settings */
    int tmp_debug_on = gossip_debug_on;
    uint64_t tmp_debug_mask = gossip_debug_mask;

    /* turn off any running facility */
    gossip_disable();

    gossip_facility = GOSSIP_STDERR;

    /* restore the logging settings */
    gossip_debug_on = tmp_debug_on;
    gossip_debug_mask = tmp_debug_mask;

    return 0;
}

/* gossip_enable_file()
 * 
 * Turns on logging to a file.  The filename argument indicates which
 * file to use for logging messages, and the mode indicates whether the
 * file should be truncated or appended (see fopen() man page).
 *
 * returns 0 on success, -errno on failure
 */
int gossip_enable_file(
    const char *filename,
    const char *mode)
{

    /* keep up with the existing logging settings */
    int tmp_debug_on = gossip_debug_on;
    uint64_t tmp_debug_mask = gossip_debug_mask;

    /* turn off any running facility */
    gossip_disable();

    internal_log_file = fopen(filename, mode);
    if (!internal_log_file)
    {
        return -errno;
    }

    gossip_facility = GOSSIP_FILE;

    /* restore the logging settings */
    gossip_debug_on = tmp_debug_on;
    gossip_debug_mask = tmp_debug_mask;

    return 0;
}

/* gossip_disable()
 * 
 * Turns off any active logging facility and disables debugging.
 *
 * returns 0 on success, -errno on failure
 */
int gossip_disable(
    void)
{
    int ret = -EINVAL;

    switch (gossip_facility)
    {
    case GOSSIP_STDERR:
        ret = gossip_disable_stderr();
        break;
    case GOSSIP_FILE:
        ret = gossip_disable_file();
        break;
    case GOSSIP_SYSLOG:
        ret = gossip_disable_syslog();
        break;
    default:
        break;
    }

    gossip_debug_on = 0;
    gossip_debug_mask = 0;

    return ret;
}

/* gossip_get_debug_mask()
 *
 * fills in args indicating whether debugging is on or off, and what the 
 * mask level is
 *
 * returns 0 on success, -errno on failure
 */
int gossip_get_debug_mask(
    int *debug_on,
    uint64_t *mask)
{
    *debug_on = gossip_debug_on;
    *mask = gossip_debug_mask;
    return 0;
}

/* gossip_set_debug_mask()
 *
 * Determines whether debugging messages are turned on or off.  Also
 * specifies the mask that determines which debugging messages are
 * printed.
 *
 * returns 0 on success, -errno on failure
 */
int gossip_set_debug_mask(
    int debug_on,
    uint64_t mask)
{
    if ((debug_on != 0) && (debug_on != 1))
    {
        return -EINVAL;
    }

    gossip_debug_on = debug_on;
    gossip_debug_mask = mask;
    return 0;
}

/* __gossip_debug_stub()
 * 
 * stub for gossip_debug that doesn't do anything; used when debugging
 * is "compiled out" on non-gcc builds
 *
 * returns 0
 */
int __gossip_debug_stub(
    uint64_t mask,
    const char *format,
    ...)
{
    return 0;
}


/* __gossip_debug()
 * 
 * Logs a standard debugging message.  It will not be printed unless the
 * mask value matches (logical "and" operation) with the mask specified in
 * gossip_set_debug_mask() and debugging is turned on.
 *
 * returns 0 on success, -errno on failure
 */
int __gossip_debug(
    uint64_t mask,
    const char *format,
    ...)
{
    va_list ap;
    int ret = -EINVAL;

    /* NOTE: this check happens in the macro (before making a function call)
     * if we use gcc 
     */
#ifndef __GNUC__
    /* exit quietly if we aren't meant to print */
    if ((!gossip_debug_on) || !(gossip_debug_mask & mask) ||
        (!gossip_facility))
    {
        return 0;
    }
#endif

    /* rip out the variable arguments */
    va_start(ap, format);

    switch (gossip_facility)
    {
    case GOSSIP_STDERR:
        ret = gossip_debug_stderr(format, ap);
        break;
    case GOSSIP_FILE:
        ret = gossip_debug_file(format, ap);
        break;
    case GOSSIP_SYSLOG:
        ret = gossip_debug_syslog(format, ap);
        break;
    default:
        break;
    }

    va_end(ap);

    return ret;
}

/* gossip_err()
 * 
 * Logs a critical error message.  This will print regardless of the
 * mask value and whether debugging is turned on or off, as long as some
 * logging facility has been enabled.
 *
 * returns 0 on success, -errno on failure
 */
int gossip_err(
    const char *format,
    ...)
{
    va_list ap;
    int ret = -EINVAL;

    if (!gossip_facility)
    {
        return 0;
    }

    /* rip out the variable arguments */
    va_start(ap, format);

    switch (gossip_facility)
    {
    case GOSSIP_STDERR:
        ret = gossip_err_stderr(format, ap);
        break;
    case GOSSIP_FILE:
        ret = gossip_err_file(format, ap);
        break;
    case GOSSIP_SYSLOG:
        ret = gossip_err_syslog(format, ap);
        break;
    default:
        break;
    }

    va_end(ap);

    return ret;
}

#ifdef GOSSIP_ENABLE_BACKTRACE
    #ifndef GOSSIP_BACKTRACE_DEPTH
    #define GOSSIP_BACKTRACE_DEPTH 8
    #endif
/* gossip_backtrace()
 *
 * prints out a dump of the current stack (excluding this function)
 * using gossip_err
 *
 * no return value
 */
void gossip_backtrace(void)
{
    void *trace[GOSSIP_BACKTRACE_DEPTH];
    char **messages = NULL;
    int i, trace_size;

    trace_size = backtrace(trace, GOSSIP_BACKTRACE_DEPTH);
    messages = backtrace_symbols(trace, trace_size);
    for(i = 1; i < trace_size; i++)
    {
        gossip_err("\t[bt] %s\n", messages[i]);
    }
    free(messages);
}
#endif

/****************************************************************
 * Internal functions
 */

/* gossip_debug_syslog()
 * 
 * This is the standard debugging message function for the syslog logging
 * facility
 *
 * returns 0 on success, -errno on failure
 */
static int gossip_debug_syslog(
    const char *format,
    va_list ap)
{
    char buffer[GOSSIP_BUF_SIZE];
    int ret = -EINVAL;

    ret = vsnprintf(buffer, GOSSIP_BUF_SIZE, format, ap);
    if (ret < 0)
    {
        return -errno;
    }

    syslog(internal_syslog_priority, buffer);

    return 0;
}


/* gossip_debug_file()
 * 
 * This is the standard debugging message function for the file logging
 * facility
 *
 * returns 0 on success, -errno on failure
 */
static int gossip_debug_file(
    const char *format,
    va_list ap)
{
    char buffer[GOSSIP_BUF_SIZE];
    int ret = -EINVAL;

    ret = vsnprintf(buffer, GOSSIP_BUF_SIZE, format, ap);
    if (ret < 0)
    {
        return -errno;
    }

    ret = fprintf(internal_log_file, buffer);
    if (ret < 0)
    {
        return -errno;
    }
    fflush(internal_log_file);

    return 0;
}

/* gossip_debug_stderr()
 * 
 * This is the standard debugging message function for the stderr
 * facility.
 *
 * returns 0 on success, -errno on failure
 */
static int gossip_debug_stderr(
    const char *format,
    va_list ap)
{

    int ret = vfprintf(stderr, format, ap);
    if (ret < 0)
    {
        return -errno;
    }
    return 0;
}

/* gossip_err_syslog()
 * 
 * error message function for the syslog logging facility
 *
 * returns 0 on success, -errno on failure
 */
static int gossip_err_syslog(
    const char *format,
    va_list ap)
{
    /* for syslog we have the opportunity to change the priority level
     * for errors
     */
    int tmp_priority = internal_syslog_priority;
    internal_syslog_priority = LOG_ERR;

    gossip_debug_syslog(format, ap);

    internal_syslog_priority = tmp_priority;

    return 0;
}


/* gossip_err_file()
 * 
 * error message function for the file logging facility
 *
 * returns 0 on success, -errno on failure
 */
static int gossip_err_file(
    const char *format,
    va_list ap)
{
    /* we don't do anything special with errors here */
    return gossip_debug_file(format, ap);
}


/* gossip_err_stderr()
 * 
 * This is the error message function for the stderr facility.
 *
 * returns 0 on success, -errno on failure
 */
static int gossip_err_stderr(
    const char *format,
    va_list ap)
{
    /* we don't do anything special for errors here */
    return gossip_debug_stderr(format, ap);
}


/* gossip_disable_stderr()
 * 
 * The shutdown function for the stderr logging facility.
 *
 * returns 0 on success, -errno on failure
 */
static int gossip_disable_stderr(
    void)
{
    /* this function doesn't need to do anything... */
    return 0;
}

/* gossip_disable_file()
 * 
 * The shutdown function for the file logging facility.
 *
 * returns 0 on success, -errno on failure
 */
static int gossip_disable_file(
    void)
{
    fclose(internal_log_file);
    return 0;
}

/* gossip_disable_syslog()
 * 
 * The shutdown function for the syslog logging facility.
 *
 * returns 0 on success, -errno on failure
 */
static int gossip_disable_syslog(
    void)
{
    closelog();
    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
