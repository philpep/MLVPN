/*    $OpenBSD: privsep.c,v 1.34 2008/11/23 04:29:42 brad Exp $    */

/*
 * Copyright (c) 2003 Anil Madhavapeddy <anil@recoil.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _BSD_SOURCE
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#ifdef __FreeBSD__
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_tun.h>
#endif
#include <sys/stat.h>
#include <netdb.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include "mlvpn.h"
#include "ps_status.h"
#include "strlcpy.h"

/*
 * mlvpn can only go forward in these states; each state should represent
 * less privilege.   After STATE_INIT, the child is allowed to parse its
 * config file once, and communicate the information regarding what logfile
 * it needs access to back to the parent.  When that is done, it sends a
 * message to the priv parent revoking this access, moving to STATE_RUNNING.
 * In this state, any log-files not in the access list are rejected.
 *
 * This allows a HUP signal to the child to reopen its log files, and
 * the config file to be parsed if it hasn't been changed (this is still
 * useful to force resolution of remote syslog servers again).
 * If the config file has been modified, then the child dies, and
 * the priv parent restarts itself.
 */
enum priv_state {
    STATE_INIT,       /* just started up */
    STATE_CONFIG,     /* parsing config file for first time */
    STATE_RUNNING,    /* running and accepting network traffic */
    STATE_QUIT        /* shutting down */
};

enum cmd_types {
    PRIV_OPEN_LOG,      /* open logfile for appending */
    PRIV_OPEN_CONFIG,   /* open config file for reading only */
    PRIV_INIT_SCRIPT,   /* set allowed status script */
    PRIV_RUN_SCRIPT,    /* run status script */
    PRIV_OPEN_TUN,
    PRIV_GETADDRINFO,
    PRIV_DONE_CONFIG_PARSE    /* signal that the initial config parse is done */
};

static int priv_fd = -1;
static volatile pid_t child_pid = -1;
static char config_file[MAXPATHLEN];
static struct stat cf_info;
static volatile sig_atomic_t cur_state = STATE_INIT;

/* Allowed logfile */
static char allowed_logfile[MAXPATHLEN];

static void root_check_log_name(char *, size_t);
static int root_open_file(char *, int);
int root_open_tun(int tuntapmode, char *devname);
static int root_launch_script(char *, int, char **);
static void increase_state(int);
static void sig_got_chld(int);
static void sig_pass_to_chld(int);
static void must_read(int, void *, size_t);
static void must_write(int, void *, size_t);
static int  may_read(int, void *, size_t);


int
priv_init(char *conf, char *argv[], char *username)
{
    int i, fd, socks[2], cmd, restart;
    int nullfd;
    int tuntapmode;
    size_t len;
    size_t path_len;
    size_t hostname_len, servname_len, addrinfo_len;
    char path[MAXPATHLEN];
    struct passwd *pw;
    struct sigaction sa;
    struct addrinfo hints, *res0, *res;
    char hostname[MLVPN_MAXHNAMSTR], servname[MLVPN_MAXHNAMSTR];
    char script_path[MAXPATHLEN] = {0};
    char tuntapname[MLVPN_IFNAMSIZ];
    char **script_argv;
    int script_argc;
    struct stat st;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = SIG_DFL;
    for (i = 1; i < MLVPN_NSIG; i++)
        sigaction(i, &sa, NULL);

    /* Create sockets */
    if (socketpair(AF_LOCAL, SOCK_STREAM, PF_UNSPEC, socks) == -1)
        err(1, "socketpair() failed");

    pw = getpwnam(username);
    if (pw == NULL)
        errx(1, "unknown user %s", username);

    child_pid = fork();
    if (child_pid < 0)
        err(1, "fork() failed");

    if (!child_pid) {
        /* Child - drop privileges and return */
        if (chroot(pw->pw_dir) != 0)
            err(1, "unable to chroot");
        if (chdir("/") != 0)
            err(1, "unable to chdir");

        if (setgroups(1, &pw->pw_gid) == -1)
            err(1, "setgroups() failed");
        if (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) == -1)
            err(1, "setresgid() failed");
        if (setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid) == -1)
            err(1, "setresuid() failed");
        close(socks[0]);
        priv_fd = socks[1];
        return 0;
    }
    /* Father */
    /* Pass TERM/HUP/INT/QUIT through to child, and accept CHLD */
    sa.sa_handler = sig_pass_to_chld;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sa.sa_handler = sig_got_chld;
    sa.sa_flags |= SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    init_ps_display("", "", "", "mlvpn [priv]");
    close(socks[1]);

    /* Save the config file specified by the child process */
    if (strlcpy(config_file, conf, sizeof(config_file)) >= sizeof(config_file))
        errx(1, "config_file truncated");

    if (stat(config_file, &cf_info) < 0)
        err(1, "stat config file failed");

    increase_state(STATE_CONFIG);
    restart = 0;

    nullfd = open("/dev/null", O_RDONLY);
    if (nullfd < 0) {
        perror("/dev/null");
        _exit(1);
    }
    dup2(nullfd, 0);
    dup2(nullfd, 1);
    //dup2(nullfd, 2);
    if (nullfd > 2)
        close(nullfd);

    while (cur_state < STATE_QUIT) {
        if (may_read(socks[0], &cmd, sizeof(cmd)))
            break;
        switch (cmd) {
        case PRIV_OPEN_LOG:
            /* TODO: forbid completly access to open log
             *  if STATUS_RUNING 
             */
            //dprintf("[priv]: msg PRIV_OPEN_LOG received\n")
            /* Expecting: length, path */
            must_read(socks[0], &path_len, sizeof(path_len));
            if (path_len == 0 || path_len > sizeof(path))
                _exit(0);
            must_read(socks[0], &path, path_len);
            path[path_len - 1] = '\0';
            root_check_log_name(path, path_len);
            fd = root_open_file(path, O_WRONLY|O_APPEND|O_NONBLOCK|O_CREAT);
            send_fd(socks[0], fd);
            if (fd < 0)
                warnx("priv_open_log failed");
            else
                close(fd);
            break;

        case PRIV_OPEN_CONFIG:
            //dprintf("[priv]: msg PRIV_OPEN_CONFIG received\n");
            stat(config_file, &cf_info);
            fd = root_open_file(config_file, O_RDONLY|O_NONBLOCK);
            send_fd(socks[0], fd);
            if (fd < 0)
                warnx("priv_open_config failed");
            else
                close(fd);
            break;

        case PRIV_OPEN_TUN:
            /* we should not re-open the tuntap device ever! */
            if (cur_state != STATE_CONFIG)
                _exit(0);

            must_read(socks[0], &tuntapmode, sizeof(tuntapmode));
            if (tuntapmode != MLVPN_TUNTAPMODE_TUN &&
                tuntapmode != MLVPN_TUNTAPMODE_TAP)
                _exit(0);

            must_read(socks[0], &len, sizeof(len));
            if (len < 0 || len >= MLVPN_IFNAMSIZ)
                _exit(0);

            else if (len > 0) {
                must_read(socks[0], &tuntapname, len);
                tuntapname[len] = '\0';
            } else {
                tuntapname[0] = '\0';
            }

            fd = root_open_tun(tuntapmode, tuntapname);
            if (fd < 0)
            {
                must_write(socks[0], &fd, sizeof(fd));
            } else {
                len = strlen(tuntapname);
                must_write(socks[0], &len, sizeof(len));
                must_write(socks[0], tuntapname, len);
            }
            send_fd(socks[0], fd);
            if (fd >= 0)
                close(fd);
            break;

        case PRIV_GETADDRINFO:
            /* Expecting: len, hostname, len, servname, hints */
            must_read(socks[0], &hostname_len, sizeof(hostname_len));
            if (hostname_len > sizeof(hostname))
                _exit(0);
            must_read(socks[0], &hostname, hostname_len);
            hostname[hostname_len - 1] = '\0';

            must_read(socks[0], &servname_len, sizeof(servname_len));
            if (servname_len > sizeof(servname))
                _exit(0);
            must_read(socks[0], &servname, servname_len);
            servname[servname_len - 1] = '\0';

            memset(&hints, '\0', sizeof(struct addrinfo));
            must_read(socks[0], &hints, sizeof(struct addrinfo));

            i = getaddrinfo(hostname, servname, &hints, &res0);
            if (i != 0 || res0 == NULL) {
                addrinfo_len = 0;
                must_write(socks[0], &addrinfo_len, sizeof(addrinfo_len));
            } else {
                addrinfo_len = 0;
                res = res0;
                while (res)
                {
                    addrinfo_len++;
                    res = res->ai_next;
                }
                must_write(socks[0], &addrinfo_len, sizeof(addrinfo_len));

                res = res0;
                while (res)
                {
                    must_write(socks[0], &res->ai_flags, sizeof(res->ai_flags));
                    must_write(socks[0], &res->ai_family, sizeof(res->ai_family));
                    must_write(socks[0], &res->ai_socktype, sizeof(res->ai_socktype));
                    must_write(socks[0], &res->ai_protocol, sizeof(res->ai_protocol));
                    must_write(socks[0], &res->ai_addrlen, sizeof(res->ai_addrlen));
                    must_write(socks[0], res->ai_addr, res->ai_addrlen);
                    res = res->ai_next;
                }
                freeaddrinfo(res0);
            }
            break;

        case PRIV_RUN_SCRIPT:
            must_read(socks[0], &script_argc, sizeof(script_argc));
            if (script_argc == 0)
                _exit(0);

            script_argv = (char **)malloc((script_argc+1)*sizeof(char *));
            for(i = 0; i < script_argc; i++)
            {
                must_read(socks[0], &len, sizeof(len));
                if (len == 0)
                {
                    script_argv[i] = NULL;
                    break;
                }
                script_argv[i] = (char *)malloc(len);
                must_read(socks[0], script_argv[i], len);
                script_argv[i][len-1] = 0;
            }
            script_argv[i] = NULL;

            if (! *script_path)
            {
                i = -1;
            } else {
                i = root_launch_script(script_path, script_argc, script_argv);
            }
            must_write(socks[0], &i, sizeof(i));
            for(i = 0; i < script_argc; i++)
                free(script_argv[i]);
            free(script_argv);
            break;

        case PRIV_INIT_SCRIPT:
            if (cur_state != STATE_CONFIG)
                _exit(0);
            must_read(socks[0], &path_len, sizeof(path_len));
            if (path_len == 0 || path_len > sizeof(script_path))
                _exit(0);
            must_read(socks[0], &path, path_len);
            path[path_len] = '\0';
            if (stat(path, &st) < 0)
                warn("stat: %s", path);
            else if (st.st_mode & (S_IRWXG|S_IRWXO))
                warnx("file %s is group or other accessible", path);
            else if (!(st.st_mode & S_IXUSR))
                warnx("file %s is not executable", path);
            /* TODO check directory + check owner */
            else
                strncpy(script_path, path, path_len);
            break;
        case PRIV_DONE_CONFIG_PARSE:
            increase_state(STATE_RUNNING);
            break;

        default:
            errx(1, "unknown command %d", cmd);
            break;
        }
    }

    close(socks[0]);

    if (restart) {
        int r;

        wait(&r);
        execvp(argv[0], argv);
    }
    _exit(1);
}

static int
root_open_file(char *path, int flags)
{
    /* must not start with | */
    if (path[0] == '|')
        return (-1);
    return (open(path, flags, 0));
}

/* If we are in the initial configuration state, accept a logname and add
 * it to the list of acceptable logfiles.  Otherwise, check against this list
 * and rewrite to /dev/null if it's a bad path.
 */
static void
root_check_log_name(char *lognam, size_t loglen)
{
    char *p;

    /* Any path containing '..' is invalid.  */
    for (p = lognam; *p && (p - lognam) < loglen; p++)
        if (*p == '.' && *(p + 1) == '.')
            goto bad_path;

    switch (cur_state) {
    case STATE_CONFIG:
        strncpy(allowed_logfile, lognam, MAXPATHLEN);
        break;
    case STATE_RUNNING:
        if (!strcmp(allowed_logfile, lognam))
            return;
        goto bad_path;
        break;
    default:
        /* Any other state should just refuse the request */
        goto bad_path;
        break;
    }
    return;

bad_path:
    warnx("%s: invalid attempt to open %s: rewriting to /dev/null",
        "root_check_log_name", lognam);
    strlcpy(lognam, "/dev/null", loglen);
}

static int
root_launch_script(char *setup_script, int argc, char **argv)
{
    sigset_t oldmask, mask;
    int pid, status = -1;
    int open_max, i;
    char **newargs;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    /* try to launch network script */
    pid = fork();
    if (pid == 0) {
        open_max = sysconf(_SC_OPEN_MAX);
        for (i = 0; i < open_max; i++) {
            if (i != STDIN_FILENO &&
                i != STDOUT_FILENO &&
                i != STDERR_FILENO) {
                close(i);
            }
        }

        newargs = (char **)malloc((argc+2)*sizeof(char *));
        newargs[0] = setup_script;
        for(i = 0; i < argc; i++)
            newargs[i+1] = argv[i];
        newargs[i+1] = NULL;

        chdir("/");

        execv(setup_script, newargs);
        _exit(1);
    } else if (pid > 0) {
        while (waitpid(pid, &status, 0) != pid) {
            /* loop */
        }
        sigprocmask(SIG_SETMASK, &oldmask, NULL);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return status;
        }
    }
    fprintf(stderr, "%s: could not launch network script\n", setup_script);
    return status;
}

/* Crank our state into less permissive modes */
static void
increase_state(int state)
{
    if (state <= cur_state)
        errx(1, "attempt to decrease or match current state");
    if (state < STATE_INIT || state > STATE_QUIT)
        errx(1, "attempt to switch to invalid state");
    cur_state = state;
}

/* Open log-file */
FILE *
priv_open_log(char *lognam)
{
    char path[MAXPATHLEN];
    int cmd, fd;
    size_t path_len;
    FILE *fp;

    if (priv_fd < 0)
        errx(1, "%s: called from privileged child", "priv_open_log");

    strncpy(path, lognam, sizeof path);
    path_len = strlen(path) + 1;

    cmd = PRIV_OPEN_LOG;
    must_write(priv_fd, &cmd, sizeof(cmd));
    must_write(priv_fd, &path_len, sizeof(path_len));
    must_write(priv_fd, path, path_len);
    fd = receive_fd(priv_fd);

    if (fd < 0)
        return NULL;

    fp = fdopen(fd, "a");
    if (!fp) {
        warn("priv_open_log: fdopen() failed");
        close(fd);
        return NULL;
    }

    return fp;
}

/* Open mlvpn config file for reading */
FILE *
priv_open_config(void)
{
    int cmd, fd;
    FILE *fp;

    if (priv_fd < 0)
        errx(1, "%s: called from privileged portion", "priv_open_config");

    cmd = PRIV_OPEN_CONFIG;
    must_write(priv_fd, &cmd, sizeof(cmd));
    fd = receive_fd(priv_fd);
    if (fd < 0)
        return NULL;

    fp = fdopen(fd, "r");
    if (!fp) {
        warn("priv_open_config: fdopen() failed");
        close(fd);
        return NULL;
    }

    return fp;
}

/* Open tun from unpriviled code
 * Scope: public
 */
int priv_open_tun(int tuntapmode, char *devname)
{
    int cmd, fd;
    size_t len;

    if (priv_fd < 0)
        errx(1, "priv_open_tun: called from privileged portion");

    len = strlen(devname);

    cmd = PRIV_OPEN_TUN;
    must_write(priv_fd, &cmd, sizeof(cmd));
    must_write(priv_fd, &tuntapmode, sizeof(tuntapmode));

    must_write(priv_fd, &len, sizeof(len));
    must_write(priv_fd, devname, len);

    must_read(priv_fd, &len, sizeof(len));
    if (len > 0)
        must_read(priv_fd, devname, len);

    devname[len] = '\0';
    fd = receive_fd(priv_fd);
    return fd;
}

/* Really open the tun/tap device.
 * returns tun/tap file descriptor.
 *
 * Compatibility: Linux 2.4+, FreeBSD
 * Scope: private
 */
#ifdef __linux__
int root_open_tun(int tuntapmode, char *devname)
{
    struct ifreq ifr;
    int fd;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd >= 0)
    {
        memset(&ifr, 0, sizeof(ifr));
        if (tuntapmode == MLVPN_TUNTAPMODE_TAP)
            ifr.ifr_flags = IFF_TUN;
        else
            ifr.ifr_flags = IFF_TAP;

        /* We do not want kernel packet info (IFF_NO_PI) */
        ifr.ifr_flags |= IFF_NO_PI; 

        /* Allocate with specified name, otherwise the kernel
         * will find a name for us. */
        if (*devname)
            strncpy(ifr.ifr_name, devname, MLVPN_IFNAMSIZ);

        /* ioctl to create the if */
        if (ioctl(fd, TUNSETIFF, (void *) &ifr) < 0)
            warn("priv_open_tun failed");

        /* The kernel is the only one able to "name" the if.
         * so we reread it to get the real name set by the kernel. */
        strncpy(devname, ifr.ifr_name, MLVPN_IFNAMSIZ);
   }
   return fd;
}
#elif __FreeBSD__
int root_open_tun(int tuntapmode, char *devname)
{
    int i;
    int fd = -1;
    char ifname[MLVPN_IFNAMSIZ];
    char ifpath[MLVPN_IFNAMSIZ+5];
    for (i = 0; i < 256; i++) {
        snprintf(ifname, sizeof(ifname), "%s%d",
                (tuntapmode == MLVPN_TUNTAPMODE_TUN) ? "tun" : "tap",
                i);
        snprintf(ifpath, sizeof(ifpath), "/dev/%s", ifname);
        if ((fd = open(ifpath, O_RDWR)) > 0)
            strncpy(devname, ifname, MLVPN_IFNAMSIZ);
            break;
    }
    if (fd >= 0 && tuntapmode == MLVPN_TUNTAPMODE_TUN) {
        i = IFF_POINTOPOINT | IFF_MULTICAST;
        if (ioctl(fd, TUNSIFMODE, &i) < 0) {
            warn("ioctl(TUNSIFMODE)");
        }
        i = 1;
        if (ioctl(fd, TUNSIFHEAD, &i) < 0) {
            warn("ioctl(TUNSIFHEAD)");
        }
    }
    return fd;
}
#endif

/* Name/service to address translation.  Response is placed into addr, and
 * the length is returned (zero on error) */
int
priv_getaddrinfo(char *host, char *serv, struct addrinfo **addrinfo,
    struct addrinfo *hints)
{
    char hostcpy[MLVPN_MAXHNAMSTR], servcpy[MLVPN_MAXHNAMSTR];
    int cmd, i;
    size_t hostname_len, servname_len, ret_len;
    struct addrinfo *new, *last = NULL;

    if (priv_fd < 0)
        errx(1, "%s: called from privileged portion", "priv_gethostserv");

    strlcpy(hostcpy, host, sizeof(hostcpy));
    hostname_len = strlen(hostcpy) + 1;
    strlcpy(servcpy, serv, sizeof(servcpy));
    servname_len = strlen(servcpy) + 1;

    cmd = PRIV_GETADDRINFO;
    must_write(priv_fd, &cmd, sizeof(cmd));
    must_write(priv_fd, &hostname_len, sizeof(hostname_len));
    must_write(priv_fd, hostcpy, hostname_len);
    must_write(priv_fd, &servname_len, sizeof(servname_len));
    must_write(priv_fd, servcpy, servname_len);
    must_write(priv_fd, hints, sizeof(struct addrinfo));

    /* How much addrinfo we have */
    must_read(priv_fd, &ret_len, sizeof(ret_len));

    /* Check there was no error (indicated by a return of 0) */
    if (!ret_len)
        return 0;

    for (i=0; i < ret_len; i++)
    {
        new = (struct addrinfo *)malloc(sizeof(struct addrinfo));
        must_read(priv_fd, &new->ai_flags, sizeof(new->ai_flags));
        must_read(priv_fd, &new->ai_family, sizeof(new->ai_family));
        must_read(priv_fd, &new->ai_socktype, sizeof(new->ai_socktype));
        must_read(priv_fd, &new->ai_protocol, sizeof(new->ai_protocol));
        must_read(priv_fd, &new->ai_addrlen, sizeof(new->ai_addrlen));
        new->ai_addr = (struct sockaddr *)malloc(new->ai_addrlen);
        must_read(priv_fd, new->ai_addr, new->ai_addrlen);
        new->ai_canonname = NULL;
        new->ai_next = NULL;

        if (i == 0)
            *addrinfo = new;
        if (last)
            last->ai_next = new;
        last = new;
    }

    return ret_len;
}

/* init script path */
void
priv_init_script(char *path)
{
    int cmd;
    size_t len;

    if (priv_fd < 0)
        errx(1, "%s: called from privileged portion",
                "priv_init_script");

    cmd = PRIV_INIT_SCRIPT;
    must_write(priv_fd, &cmd, sizeof(cmd));
    len = strlen(path);
    must_write(priv_fd, &len, sizeof(len));
    must_write(priv_fd, path, len);
}

/* run script */
int
priv_run_script(int argc, char **argv)
{
    int cmd, retval;
    int i;
    size_t len;

    if (priv_fd < 0)
        errx(1, "%s: called from privileged portion",
                "priv_run_script");

    cmd = PRIV_RUN_SCRIPT;
    must_write(priv_fd, &cmd, sizeof(cmd));

    must_write(priv_fd, &argc, sizeof(argc));
    for (i=0; i < argc; i++)
    {
        len = strlen(argv[i])+1;
        must_write(priv_fd, &len, sizeof(len));
        must_write(priv_fd, argv[i], len);
    }

    must_read(priv_fd, &retval, sizeof(retval));
    return retval;
}

/* Child can signal that its initial parsing is done, so that parent
 * can revoke further logfile permissions.  This call only works once. */
void
priv_config_parse_done(void)
{
    int cmd;

    if (priv_fd < 0)
        errx(1, "%s: called from privileged portion",
            "priv_config_parse_done");

    cmd = PRIV_DONE_CONFIG_PARSE;
    must_write(priv_fd, &cmd, sizeof(cmd));
}

/* When child dies, move into the shutdown state */
/* ARGSUSED */
static void
sig_got_chld(int sig)
{
    int save_errno = errno;
    pid_t    pid;

    do {
        pid = waitpid(WAIT_ANY, NULL, WNOHANG);
        if (pid == child_pid && cur_state < STATE_QUIT)
            cur_state = STATE_QUIT;
    } while (pid > 0 || (pid == -1 && errno == EINTR));
    errno = save_errno;
}

static void
sig_pass_to_chld(int sig)
{
    int save_errno = errno;
    if (child_pid != -1) {
       kill(child_pid, sig);
       errno = save_errno;
    }
}

/* Read all data or return 1 for error.  */
static int
may_read(int fd, void *buf, size_t n)
{
    char *s = buf;
    ssize_t res, pos = 0;

    while (n > pos) {
        res = read(fd, s + pos, n - pos);
        switch (res) {
        case -1:
            if (errno == EINTR || errno == EAGAIN)
                continue;
        case 0:
            return (1);
        default:
            pos += res;
        }
    }
    return (0);
}

/* Read data with the assertion that it all must come through, or
 * else abort the process.  Based on atomicio() from openssh. */
static void
must_read(int fd, void *buf, size_t n)
{
    char *s = buf;
    ssize_t res, pos = 0;

    while (n > pos) {
        res = read(fd, s + pos, n - pos);
        switch (res) {
        case -1:
            if (errno == EINTR || errno == EAGAIN)
                continue;
        case 0:
            _exit(0);
        default:
            pos += res;
        }
    }
}

/* Write data with the assertion that it all has to be written, or
 * else abort the process.  Based on atomicio() from openssh. */
static void
must_write(int fd, void *buf, size_t n)
{
    char *s = buf;
    ssize_t res, pos = 0;

    while (n > pos) {
        res = write(fd, s + pos, n - pos);
        switch (res) {
        case -1:
            if (errno == EINTR || errno == EAGAIN)
                continue;
        case 0:
            _exit(0);
        default:
            pos += res;
        }
    }
}
