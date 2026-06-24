#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DEFAULT_LISTEN_IP "127.0.0.1"
#define DEFAULT_LISTEN_PORT 8080
#define BACKLOG 128
#define IO_BUF_SIZE 4096
#define MAX_TCP_CLIENTS 1024

typedef enum protocol {
    PROTO_TCP = 0,
    PROTO_UDP,
    PROTO_BOTH
} protocol_t;

typedef struct options {
    protocol_t protocol;
    const char *listen_ip;
    int listen_port;
} options_t;

static void usage(const char *prog)
{
    printf("Usage: %s [-M tcp|udp|both] [-a listen_ip] [-p listen_port]\n", prog);
    printf("  -M    protocol mode, tcp, udp, or both (default: both)\n");
    printf("  -a    listen IPv4 address (default: %s)\n", DEFAULT_LISTEN_IP);
    printf("  -p    listen port (default: %d)\n", DEFAULT_LISTEN_PORT);
    printf("  -h    show this help\n");
}

static int parse_port(const char *text, int *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > 65535) {
        fprintf(stderr, "invalid port: %s\n", text);
        return -1;
    }

    *out = (int)value;
    return 0;
}

static int parse_args(int argc, char *argv[], options_t *opts)
{
    int opt;

    opts->protocol = PROTO_BOTH;
    opts->listen_ip = DEFAULT_LISTEN_IP;
    opts->listen_port = DEFAULT_LISTEN_PORT;

    while ((opt = getopt(argc, argv, "M:a:p:h")) != -1) {
        switch (opt) {
            case 'M':
                if (strcmp(optarg, "tcp") == 0) {
                    opts->protocol = PROTO_TCP;
                } else if (strcmp(optarg, "udp") == 0) {
                    opts->protocol = PROTO_UDP;
                } else if (strcmp(optarg, "both") == 0) {
                    opts->protocol = PROTO_BOTH;
                } else {
                    fprintf(stderr, "invalid protocol: %s\n", optarg);
                    return -1;
                }
                break;

            case 'a':
                opts->listen_ip = optarg;
                break;

            case 'p':
                if (parse_port(optarg, &opts->listen_port) == -1) {
                    return -1;
                }
                break;

            case 'h':
                usage(argv[0]);
                exit(0);

            default:
                return -1;
        }
    }

    return 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int fill_addr(const char *ip, int port, struct sockaddr_in *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr->sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", ip);
        return -1;
    }

    return 0;
}

static int set_reuseaddr(int fd)
{
    int on = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        return -1;
    }

    return 0;
}

static int create_tcp_listener(const char *ip, int port)
{
    struct sockaddr_in addr;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("tcp socket");
        return -1;
    }

    if (set_reuseaddr(fd) == -1 || set_nonblocking(fd) == -1) {
        close(fd);
        return -1;
    }

    if (fill_addr(ip, port, &addr) == -1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("tcp bind");
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) == -1) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static int create_udp_socket(const char *ip, int port)
{
    struct sockaddr_in addr;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        perror("udp socket");
        return -1;
    }

    if (set_reuseaddr(fd) == -1 || set_nonblocking(fd) == -1) {
        close(fd);
        return -1;
    }

    if (fill_addr(ip, port, &addr) == -1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("udp bind");
        close(fd);
        return -1;
    }

    return fd;
}

static int wait_fd(int fd, short events)
{
    struct pollfd pfd;
    int ret;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = events;

    do {
        ret = poll(&pfd, 1, -1);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1) {
        return -1;
    }

    if ((pfd.revents & events) == 0) {
        errno = ECONNRESET;
        return -1;
    }

    return 0;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT) == -1) {
                perror("poll tcp send");
                return -1;
            }
            continue;
        }

        perror("tcp send");
        return -1;
    }

    return 0;
}

static int send_udp_reply(int fd,
                          const char *buf,
                          size_t len,
                          const struct sockaddr_in *peer,
                          socklen_t peer_len)
{
    for (;;) {
        ssize_t n = sendto(fd, buf, len, 0, (const struct sockaddr *)peer, peer_len);

        if (n == (ssize_t)len) {
            return 0;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT) == -1) {
                perror("poll udp send");
                return -1;
            }
            continue;
        }

        if (n >= 0) {
            fprintf(stderr, "short udp send: %zd/%zu\n", n, len);
            return -1;
        }

        perror("udp sendto");
        return -1;
    }
}

static const char *peer_to_string(const struct sockaddr_in *peer, char *buf, size_t len)
{
    char ip[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip)) == NULL) {
        snprintf(buf, len, "unknown:%u", (unsigned int)ntohs(peer->sin_port));
    } else {
        snprintf(buf, len, "%s:%u", ip, (unsigned int)ntohs(peer->sin_port));
    }

    return buf;
}

static int add_client(int clients[], int fd)
{
    int i;

    for (i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (clients[i] == -1) {
            clients[i] = fd;
            return 0;
        }
    }

    return -1;
}

static void remove_client(int clients[], int index)
{
    if (clients[index] != -1) {
        close(clients[index]);
        clients[index] = -1;
    }
}

static void accept_tcp_clients(int listen_fd, int clients[])
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        char peer_text[64];
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);

        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            perror("accept");
            return;
        }

        if (set_nonblocking(client_fd) == -1) {
            perror("set client nonblocking");
            close(client_fd);
            continue;
        }

        if (add_client(clients, client_fd) == -1) {
            fprintf(stderr, "too many tcp clients, closing fd=%d\n", client_fd);
            close(client_fd);
            continue;
        }

        printf("tcp target accepted: peer=%s fd=%d\n",
               peer_to_string(&peer, peer_text, sizeof(peer_text)),
               client_fd);
    }
}

static bool handle_tcp_client(int fd)
{
    char buf[IO_BUF_SIZE];

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);

        if (n > 0) {
            if (send_all(fd, buf, (size_t)n) == -1) {
                return false;
            }
            printf("tcp target echo: fd=%d bytes=%zd\n", fd, n);
            continue;
        }

        if (n == 0) {
            printf("tcp target closed: fd=%d\n", fd);
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        perror("tcp recv");
        return false;
    }
}

static void handle_udp_packets(int udp_fd)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        char buf[IO_BUF_SIZE];
        char peer_text[64];
        ssize_t n;

        memset(&peer, 0, sizeof(peer));
        peer_len = sizeof(peer);
        n = recvfrom(udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            perror("udp recvfrom");
            return;
        }

        if (send_udp_reply(udp_fd, buf, (size_t)n, &peer, peer_len) == -1) {
            return;
        }

        printf("udp target echo: peer=%s bytes=%zd\n",
               peer_to_string(&peer, peer_text, sizeof(peer_text)),
               n);
    }
}

static int run_server(const options_t *opts)
{
    int tcp_fd = -1;
    int udp_fd = -1;
    int clients[MAX_TCP_CLIENTS];
    int i;

    for (i = 0; i < MAX_TCP_CLIENTS; i++) {
        clients[i] = -1;
    }

    if (opts->protocol == PROTO_TCP || opts->protocol == PROTO_BOTH) {
        tcp_fd = create_tcp_listener(opts->listen_ip, opts->listen_port);
        if (tcp_fd == -1) {
            return 1;
        }
    }

    if (opts->protocol == PROTO_UDP || opts->protocol == PROTO_BOTH) {
        udp_fd = create_udp_socket(opts->listen_ip, opts->listen_port);
        if (udp_fd == -1) {
            if (tcp_fd != -1) {
                close(tcp_fd);
            }
            return 1;
        }
    }

    printf("target_echo_server listening: addr=%s port=%d mode=%s\n",
           opts->listen_ip,
           opts->listen_port,
           opts->protocol == PROTO_TCP ? "tcp" : opts->protocol == PROTO_UDP ? "udp" : "both");

    for (;;) {
        struct pollfd fds[2 + MAX_TCP_CLIENTS];
        int client_index_for_poll[2 + MAX_TCP_CLIENTS];
        nfds_t nfds = 0;

        if (tcp_fd != -1) {
            fds[nfds].fd = tcp_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            client_index_for_poll[nfds] = -1;
            nfds++;
        }

        if (udp_fd != -1) {
            fds[nfds].fd = udp_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            client_index_for_poll[nfds] = -1;
            nfds++;
        }

        for (i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (clients[i] != -1) {
                fds[nfds].fd = clients[i];
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                client_index_for_poll[nfds] = i;
                nfds++;
            }
        }

        if (poll(fds, nfds, -1) == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        for (i = 0; i < (int)nfds; i++) {
            if (fds[i].revents == 0) {
                continue;
            }

            if ((fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                int client_index = client_index_for_poll[i];
                if (client_index >= 0) {
                    remove_client(clients, client_index);
                }
                continue;
            }

            if (fds[i].fd == tcp_fd) {
                accept_tcp_clients(tcp_fd, clients);
            } else if (fds[i].fd == udp_fd) {
                handle_udp_packets(udp_fd);
            } else if (client_index_for_poll[i] >= 0) {
                int client_index = client_index_for_poll[i];
                if (!handle_tcp_client(fds[i].fd)) {
                    remove_client(clients, client_index);
                }
            }
        }
    }

    if (tcp_fd != -1) {
        close(tcp_fd);
    }
    if (udp_fd != -1) {
        close(udp_fd);
    }
    for (i = 0; i < MAX_TCP_CLIENTS; i++) {
        remove_client(clients, i);
    }

    return 1;
}

int main(int argc, char *argv[])
{
    options_t opts;

    if (parse_args(argc, argv, &opts) == -1) {
        usage(argv[0]);
        return 2;
    }

    return run_server(&opts);
}
