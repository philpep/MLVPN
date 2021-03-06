#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <time.h>

#include "strlcpy.h"
#include "debug.h"
#include "control.h"
#include "mlvpn.h"
#include "tuntap_generic.h"

extern struct tuntap_s tuntap;
extern mlvpn_tunnel_t *rtun_start;
extern char *progname;
extern time_t start_time;
extern time_t last_reload;

void mlvpn_control_write_status(struct mlvpn_control *ctrl);

#define HTTP_HEADERS "HTTP/1.1 200 OK\r\n" \
    "Connection: close\r\n" \
    "Content-type: application/json\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Server: mlvpn\r\n" \
    "\r\n"

/* Yeah this is a bit uggly I admit :-) */
#define JSON_STATUS_BASE "{" \
    "\"name\": \"%s\",\n" \
    "\"version\": \"%d.%d\",\n" \
    "\"uptime\": %u,\n" \
    "\"last_reload\": %u,\n" \
    "\"pid\": %d,\n" \
    "\"tuntap\": {\n" \
    "   \"type\": \"%s\",\n" \
    "   \"name\": \"%s\"\n" \
    "},\n" \
    "\"tunnels\": [\n"

#define JSON_STATUS_RTUN "{\n" \
    "   \"name\": \"%s\",\n" \
    "   \"mode\": \"%s\",\n" \
    "   \"encap\": \"%s\",\n" \
    "   \"bindaddr\": \"%s\",\n" \
    "   \"bindport\": \"%s\",\n" \
    "   \"destaddr\": \"%s\",\n" \
    "   \"destport\": \"%s\",\n" \
    "   \"status\": \"%s\",\n" \
    "   \"sentpackets\": %llu,\n" \
    "   \"recvpackets\": %llu,\n" \
    "   \"sentbytes\": %llu,\n" \
    "   \"recvbytes\": %llu,\n" \
    "   \"bandwidth\": %d,\n" \
    "   \"disconnects\": %d,\n" \
    "   \"last_packet\": %u,\n" \
    "   \"timeout\": %u\n" \
    "}%s\n"
#define JSON_STATUS_ERROR_UNKNOWN_COMMAND "{\"error\": 'unknown command'}\n"

void
mlvpn_control_close_client(struct mlvpn_control *ctrl)
{
    if (ctrl->clientfd >= 0)
        close(ctrl->clientfd);
    ctrl->clientfd = -1;
}

void
mlvpn_control_init(struct mlvpn_control *ctrl)
{
    if (ctrl->mode == MLVPN_CONTROL_DISABLED)
        return;

    struct sockaddr_un un_addr;
    struct addrinfo hints, *res, *bak;
    int ret;
    int val;

    res = bak = NULL;

    ctrl->fifofd = -1;
    ctrl->sockfd = -1;
    ctrl->clientfd = -1;
    ctrl->wbuflen = 4096;
    ctrl->wbuf = malloc(ctrl->wbuflen);
    ctrl->http = 0;
    ctrl->close_after_write = 0;

    /* UNIX domain socket */
    if (*ctrl->fifo_path)
    {
        ctrl->fifofd = socket(AF_LOCAL, SOCK_STREAM, 0);
        if (ctrl->fifofd < 0)
            _ERROR("Unable to create unix socket.\n");
        else {
            memset(&un_addr, 0, sizeof(un_addr));
            un_addr.sun_family = AF_UNIX;
            strlcpy(un_addr.sun_path, ctrl->fifo_path, strlen(ctrl->fifo_path)+1);
            /* remove existing sock if exists! (bad stop) */
            /* TODO: handle proper "at_exit" removal of this socket */
            unlink(un_addr.sun_path);
            if (bind(ctrl->fifofd, (struct sockaddr *) &un_addr,
                sizeof(un_addr)) < 0)
            {
                _ERROR("Error binding socket %s: %s\n", un_addr.sun_path,
                    strerror(errno));
                close(ctrl->fifofd);
                ctrl->fifofd = -1;
            }
        }
    }


    /* INET socket */
    if (*ctrl->bindaddr && *ctrl->bindport)
    {
        ctrl->http = 1;
        memset(&hints, 0, sizeof(hints));
        hints.ai_flags = AI_PASSIVE;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        ret = priv_getaddrinfo(ctrl->bindaddr, ctrl->bindport,
            &res, &hints);
        bak = res;
        if (ret < 0 || ! res)
        {
            _ERROR("_getaddrinfo(%s,%s) failed: %s\n",
                ctrl->bindaddr, ctrl->bindport,
                gai_strerror(ret));
        }

        while(res)
        {
            if ( (ctrl->sockfd = socket(res->ai_family,
                            res->ai_socktype,
                            res->ai_protocol)) < 0)
            {
                _ERROR("Socket creation error (%s:%s): %s\n",
                    ctrl->bindaddr, ctrl->bindport, strerror(errno));
            } else {
                val = 1;
                setsockopt(ctrl->sockfd, SOL_SOCKET, SO_REUSEADDR,
                    &val, sizeof(int));
                setsockopt(ctrl->sockfd, IPPROTO_TCP, TCP_NODELAY,
                    &val, sizeof(int));
                if (bind(ctrl->sockfd, res->ai_addr, res->ai_addrlen) < 0)
                {
                    _ERROR("Bind error on %d: %s\n", ctrl->sockfd, strerror(errno));
                    close(ctrl->sockfd);
                    ctrl->sockfd = -1;
                }
                break;
            }
            res = res->ai_next;
        }
    }
    if (bak)
        freeaddrinfo(bak);

    /* bind */
    if (ctrl->fifofd >= 0)
    {
        if (mlvpn_sock_set_nonblocking(ctrl->fifofd) < 0)
        {
            close(ctrl->fifofd);
            ctrl->fifofd = -1;
        } else {
            if (listen(ctrl->fifofd, 1) < 0)
            {
                _ERROR("Error listening: %s\n", strerror(errno));
                close(ctrl->fifofd);
                ctrl->fifofd = -1;
            }
        }
    }
    if (ctrl->sockfd >= 0)
    {
        if (mlvpn_sock_set_nonblocking(ctrl->sockfd) < 0)
        {
            close(ctrl->sockfd);
            ctrl->sockfd = -1;
        } else {
            if (listen(ctrl->sockfd, 1) < 0)
            {
                _ERROR("Error listening: %s\n", strerror(errno));
                close(ctrl->sockfd);
                ctrl->sockfd = -1;
            }
        }
    }

    return;
}

int
mlvpn_control_accept(struct mlvpn_control *ctrl, int fd)
{
    /* Early exit */
    if (fd < 0 || ctrl->mode == MLVPN_CONTROL_DISABLED)
        return 0;

    int cfd;
    int accepted = 0;
    struct sockaddr_storage clientaddr;
    socklen_t addrlen = sizeof(clientaddr);

    cfd = accept(fd, (struct sockaddr *)&clientaddr, &addrlen);
    if (cfd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            _ERROR("Error during accept: %s\n", strerror(errno));
    } else {
        if (ctrl->clientfd != -1)
        {
            _DEBUG("Remote control already connected on fd %d.\n",
                ctrl->clientfd);
            send(cfd, "ERR: Already connected.\n", 24, 0);
            close(cfd);
            return 0;
        }
        accepted++;
        if (mlvpn_sock_set_nonblocking(cfd) < 0)
        {
            _ERROR("Unable to set client control socket non blocking: %s\n",
                strerror(errno));
            ctrl->clientfd = -1;
            close(cfd);
        } else
            ctrl->clientfd = cfd;
        ctrl->rbufpos = 0;
        ctrl->wbufpos = 0;
        ctrl->last_activity = time((time_t *) NULL);
    }
    return accepted;
}

int
mlvpn_control_timeout(struct mlvpn_control *ctrl)
{
    if (ctrl->mode != MLVPN_CONTROL_DISABLED &&
        ctrl->clientfd >= 0)
    {
        if (ctrl->last_activity + MLVPN_CTRL_TIMEOUT <=
            time((time_t *)NULL))
        {
            _INFO("Control socket %d timeout.\n", ctrl->clientfd);
            mlvpn_control_close_client(ctrl);
            return 1;
        }
    }
    return 0;
}

/* Parse a message received from the client.
 * Example messages:
 * STATS
 * UPTIME
 * START tunX
 * STOP tunX
 * RESTART tunX
 */
void
mlvpn_control_parse(struct mlvpn_control *ctrl, char *line)
{
    char cline[MLVPN_CTRL_BUFSIZ];
    char *cmd = NULL;
    int i, j;

    /* Cleanup \r */
    for (i = 0, j = 0; i <= strlen(line); i++)
        if (line[i] != '\r')
            cline[j++] = line[i];
    cmd = strtok(cline, " ");
    if (ctrl->http)
        cmd = strtok(NULL, " ");

    if (! cmd)
        return;
    else
        _DEBUG("control command: %s\n", cmd);

    if (ctrl->http)
        mlvpn_control_write(ctrl, HTTP_HEADERS, strlen(HTTP_HEADERS));

    if (strncasecmp(cmd, "status", 6) == 0 || strncasecmp(cmd, "/status", 7) == 0)
    {
        mlvpn_control_write_status(ctrl);
    } else if (strncasecmp(cmd, "quit", 4) == 0) {
        mlvpn_control_write(ctrl, "bye.", 4);
        mlvpn_control_close_client(ctrl);
    } else {
        mlvpn_control_write(ctrl, JSON_STATUS_ERROR_UNKNOWN_COMMAND,
            strlen(JSON_STATUS_ERROR_UNKNOWN_COMMAND));
    }

    if (ctrl->http)
        ctrl->close_after_write = 1;
}

void mlvpn_control_write_status(struct mlvpn_control *ctrl)
{
    char buf[1024];
    size_t ret;
    mlvpn_tunnel_t *t = rtun_start;

    ret = snprintf(buf, 1024, JSON_STATUS_BASE,
        progname,
        1, 1,
        (uint32_t) start_time,
        (uint32_t) last_reload,
        0,
        tuntap.type == MLVPN_TUNTAPMODE_TUN ? "tun" : "tap",
        tuntap.devname
    );
    mlvpn_control_write(ctrl, buf, ret);
    while (t)
    {
        char *mode = t->server_mode ? "server" : "client";
        char *status;
        char *encap = t->encap_prot == ENCAP_PROTO_UDP ? "udp" : "tcp";

        if (t->status == MLVPN_CHAP_DISCONNECTED)
            status = "disconnected";
        else if (t->status == MLVPN_CHAP_AUTHSENT)
            status = "waiting peer";
        else
            status = "connected";

        ret = snprintf(buf, 1024, JSON_STATUS_RTUN,
            t->name,
            mode,
            encap,
            t->bindaddr ? t->bindaddr : "any",
            t->bindport ? t->bindport : "any",
            t->destaddr ? t->destaddr : "",
            t->destport ? t->destport : "",
            status,
            (long long unsigned int)t->sentpackets,
            (long long unsigned int)t->recvpackets,
            (long long unsigned int)t->sentbytes,
            (long long unsigned int)t->recvbytes,
            mlvpn_pktbuffer_bandwidth(t->sbuf),
            t->disconnects,
            (uint32_t) t->last_packet_time,
            (uint32_t) t->timeout,
            (t->next) ? "," : ""
        );
        mlvpn_control_write(ctrl, buf, ret);

        t = t->next;
    }
    mlvpn_control_write(ctrl, "]}\n", 3);
}

/* Returns 1 if a valid line is found. 0 otherwise. */
int
mlvpn_control_read_check(struct mlvpn_control *ctrl)
{
    char line[MLVPN_CTRL_BUFSIZ];
    char c;
    int i;
    for (i = 0; i < ctrl->rbufpos; i++)
    {
        c = ctrl->rbuf[i];
        if (c == MLVPN_CTRL_EOF)
        {
            _DEBUG("Received EOF from client %d.\n", ctrl->clientfd);
            mlvpn_control_close_client(ctrl);
            break;
        }

        if (c == MLVPN_CTRL_TERMINATOR)
        {
            memcpy(line, ctrl->rbuf, i);
            line[i] = '\0';
            /* Shift the actual buffer */
            memmove(ctrl->rbuf, ctrl->rbuf+i,
                MLVPN_CTRL_BUFSIZ - i);
            ctrl->rbufpos -= i+1;
            mlvpn_control_parse(ctrl, line);
            return 1;
        }
    }
    return 0;
}

/* Read from the socket to rbuf */
int
mlvpn_control_read(struct mlvpn_control *ctrl)
{
    int len;

    len = read(ctrl->clientfd, ctrl->rbuf + ctrl->rbufpos,
        MLVPN_CTRL_BUFSIZ - ctrl->rbufpos);
    if (len > 0)
    {
        ctrl->last_activity = time((time_t *)NULL);
        _DEBUG("Read %d bytes on control fd.\n", len);
        ctrl->rbufpos += len;
        if (ctrl->rbufpos >= MLVPN_CTRL_BUFSIZ)
        {
            _ERROR("Buffer overflow on control read buffer.\n");
            mlvpn_control_close_client(ctrl);
            return -1;
        }

        /* Parse the message */
        while (mlvpn_control_read_check(ctrl) != 0);
    } else if (len < 0) {
        _ERROR("Read error on fd %d: %s\n", ctrl->clientfd,
            strerror(errno));
        mlvpn_control_close_client(ctrl);
    } else {
        /* End of file */
        ctrl->clientfd = -1;
    }

    return 0;
}

int
mlvpn_control_write(struct mlvpn_control *ctrl, void *buf, size_t len)
{
    if (ctrl->wbuflen - (ctrl->wbufpos+len) <= 0)
    {
        /* Hard realloc */
        ctrl->wbuflen += 1024*32;
        ctrl->wbuf = realloc(ctrl->wbuf, ctrl->wbuflen);
    }

    if (ctrl->wbuflen - (ctrl->wbufpos+len) <= 0)
    {
        _ERROR("Buffer overflow.\n");
        mlvpn_control_close_client(ctrl);
        return -1;
    }

    memcpy(ctrl->wbuf+ctrl->wbufpos, buf, len);
    ctrl->wbufpos += len;
    return len;
}

/* Flush the control client wbuf */
int
mlvpn_control_send(struct mlvpn_control *ctrl)
{
    int len;

    if (ctrl->wbufpos <= 0)
    {
        _ERROR("Nothing to write on control socket.\n");
        return -1;
    }
    len = write(ctrl->clientfd, ctrl->wbuf, ctrl->wbufpos);
    if (len < 0)
    {
        _ERROR("Error writing on control socket %d: %s\n",
            ctrl->clientfd, strerror(errno));
        mlvpn_control_close_client(ctrl);
    } else {
        ctrl->wbufpos -= len;
        if (ctrl->wbufpos > 0)
            memmove(ctrl->wbuf, ctrl->wbuf+len, ctrl->wbufpos);
    }

    if (ctrl->close_after_write && ctrl->wbufpos <= 0)
        mlvpn_control_close_client(ctrl);

    return len;
}

