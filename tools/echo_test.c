#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "8080"
#define DEFAULT_MESSAGE "hello echo"
#define DEFAULT_COUNT 1
#define DEFAULT_TIMEOUT_MS 3000
#define MAX_RESPONSE_SIZE 65535

typedef enum protocol {
    PROTO_TCP = 0,
    PROTO_UDP,
    PROTO_BOTH
} protocol_t;

typedef struct options {
    protocol_t protocol;
    const char *host;
    const char *port;
    const char *message;
    int count;
    int timeout_ms;
} options_t;

typedef struct udp_target {
    struct addrinfo *list;
    const struct addrinfo *addr;
} udp_target_t;

static void usage(const char *prog)
{
    printf("Usage: %s [-M tcp|udp|both] [-a host] [-p port] [-s message] [-n count] [-T timeout_ms]\n", prog);
    printf("  -M    protocol mode, tcp, udp, or both (default: both)\n");
    printf("  -a    server address (default: %s)\n", DEFAULT_HOST);
    printf("  -p    server port (default: %s)\n", DEFAULT_PORT);
    printf("  -s    message payload (default: \"%s\")\n", DEFAULT_MESSAGE);
    printf("  -n    request count (default: %d)\n", DEFAULT_COUNT);
    printf("  -T    receive timeout in milliseconds (default: %d)\n", DEFAULT_TIMEOUT_MS);
    printf("  -h    show this help\n");
}

static int parse_positive_int(const char *text, const char *name, int *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > 2147483647L) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        return -1;
    }

    *out = (int)value;
    return 0;
}

static int parse_args(int argc, char *argv[], options_t *opts)
{
    int opt;

    opts->protocol = PROTO_BOTH;
    opts->host = DEFAULT_HOST;
    opts->port = DEFAULT_PORT;
    opts->message = DEFAULT_MESSAGE;
    opts->count = DEFAULT_COUNT;
    opts->timeout_ms = DEFAULT_TIMEOUT_MS;

    while ((opt = getopt(argc, argv, "M:a:p:s:n:T:h")) != -1) {
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
                opts->host = optarg;
                break;

            case 'p':
                opts->port = optarg;
                break;

            case 's':
                opts->message = optarg;
                break;

            case 'n':
                if (parse_positive_int(optarg, "count", &opts->count) == -1) {
                    return -1;
                }
                break;

            case 'T':
                if (parse_positive_int(optarg, "timeout", &opts->timeout_ms) == -1) {
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

    if (opts->message[0] == '\0') {
        fprintf(stderr, "message must not be empty\n");
        return -1;
    }

    return 0;
}

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    int ret;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = events;

    do {
        ret = poll(&pfd, 1, timeout_ms);
    } while (ret == -1 && errno == EINTR);

    if (ret == 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    if (ret == -1) {
        return -1;
    }

    if ((pfd.revents & events) == 0) {
        errno = ECONNRESET;
        return -1;
    }

    return 0;
}

static int resolve_address(const char *host,
                           const char *port,
                           int socktype,
                           struct addrinfo **result)
{
    struct addrinfo hints;
    int ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;

    ret = getaddrinfo(host, port, &hints, result);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo failed: %s:%s: %s\n", host, port, gai_strerror(ret));
        return -1;
    }

    return 0;
}

static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    int fd = -1;

    if (resolve_address(host, port, SOCK_STREAM, &result) == -1) {
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) {
            continue;
        }

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if (fd == -1) {
        fprintf(stderr, "connect failed: %s:%s\n", host, port);
    }

    return fd;
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

        perror("send");
        return -1;
    }

    return 0;
}

static int recv_exact(int fd, char *buf, size_t len, int timeout_ms)
{
    size_t received = 0;

    while (received < len) {
        ssize_t n;

        if (wait_fd(fd, POLLIN, timeout_ms) == -1) {
            perror("poll/recv timeout");
            return -1;
        }

        n = recv(fd, buf + received, len - received, 0);
        if (n > 0) {
            received += (size_t)n;
            continue;
        }

        if (n == 0) {
            fprintf(stderr, "connection closed before full echo was received\n");
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        perror("recv");
        return -1;
    }

    return 0;
}

static int check_echo(const char *expected, const char *actual, size_t len)
{
    if (memcmp(expected, actual, len) != 0) {
        fprintf(stderr, "echo mismatch\n");
        fprintf(stderr, "expected: %.*s\n", (int)len, expected);
        fprintf(stderr, "actual:   %.*s\n", (int)len, actual);
        return -1;
    }

    return 0;
}

static int run_tcp_test(const options_t *opts)
{
    size_t message_len = strlen(opts->message);
    char *response = NULL;
    int fd;
    int i;
    int ret = -1;

    if (message_len > MAX_RESPONSE_SIZE) {
        fprintf(stderr, "message too large: %zu bytes\n", message_len);
        return -1;
    }

    response = malloc(message_len);
    if (response == NULL) {
        perror("malloc");
        return -1;
    }

    fd = connect_to_server(opts->host, opts->port);
    if (fd == -1) {
        free(response);
        return -1;
    }

    for (i = 0; i < opts->count; i++) {
        if (send_all(fd, opts->message, message_len) == -1) {
            goto out;
        }

        memset(response, 0, message_len);
        if (recv_exact(fd, response, message_len, opts->timeout_ms) == -1) {
            goto out;
        }

        if (check_echo(opts->message, response, message_len) == -1) {
            goto out;
        }

        printf("tcp echo ok: %d/%d bytes=%zu\n", i + 1, opts->count, message_len);
    }

    ret = 0;

out:
    close(fd);
    free(response);
    return ret;
}

static int create_udp_socket(const char *host,
                             const char *port,
                             udp_target_t *target)
{
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    int fd = -1;

    if (resolve_address(host, port, SOCK_DGRAM, &result) == -1) {
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd != -1) {
            target->list = result;
            target->addr = rp;
            return fd;
        }
    }

    freeaddrinfo(result);
    fprintf(stderr, "udp socket create failed: %s:%s\n", host, port);
    return -1;
}

static int run_udp_test(const options_t *opts)
{
    udp_target_t target;
    char response[MAX_RESPONSE_SIZE];
    size_t message_len = strlen(opts->message);
    int fd;
    int i;
    int ret = -1;

    memset(&target, 0, sizeof(target));

    if (message_len > MAX_RESPONSE_SIZE) {
        fprintf(stderr, "message too large: %zu bytes\n", message_len);
        return -1;
    }

    fd = create_udp_socket(opts->host, opts->port, &target);
    if (fd == -1) {
        return -1;
    }

    for (i = 0; i < opts->count; i++) {
        ssize_t n;

        n = sendto(fd,
                   opts->message,
                   message_len,
                   0,
                   target.addr->ai_addr,
                   target.addr->ai_addrlen);
        if (n == -1) {
            perror("sendto");
            goto out;
        }

        if ((size_t)n != message_len) {
            fprintf(stderr, "short udp send: %zd/%zu\n", n, message_len);
            goto out;
        }

        if (wait_fd(fd, POLLIN, opts->timeout_ms) == -1) {
            perror("poll/recvfrom timeout");
            goto out;
        }

        n = recvfrom(fd, response, sizeof(response), 0, NULL, NULL);
        if (n == -1) {
            perror("recvfrom");
            goto out;
        }

        if ((size_t)n != message_len) {
            fprintf(stderr, "udp echo size mismatch: expected=%zu actual=%zd\n", message_len, n);
            goto out;
        }

        if (check_echo(opts->message, response, message_len) == -1) {
            goto out;
        }

        printf("udp echo ok: %d/%d bytes=%zu\n", i + 1, opts->count, message_len);
    }

    ret = 0;

out:
    close(fd);
    freeaddrinfo(target.list);
    return ret;
}

int main(int argc, char *argv[])
{
    options_t opts;
    int failed = 0;

    if (parse_args(argc, argv, &opts) == -1) {
        usage(argv[0]);
        return 2;
    }

    if (opts.protocol == PROTO_TCP || opts.protocol == PROTO_BOTH) {
        if (run_tcp_test(&opts) == -1) {
            failed = 1;
        }
    }

    if (opts.protocol == PROTO_UDP || opts.protocol == PROTO_BOTH) {
        if (run_udp_test(&opts) == -1) {
            failed = 1;
        }
    }

    return failed ? 1 : 0;
}
