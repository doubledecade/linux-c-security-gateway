#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <poll.h>
#include <stdbool.h>
#include <time.h>
#include "config.h"
#include "log.h"
#define BACKLOG 65535
#define MAX_EVENTS 1024
#define IO_BUF_SIZE 4096
#define UDP_PROXY_TIMEOUT_MS 5000
#define TCP_CONN_IDLE_TIMEOUT_SEC 60
#define FILE_PATH_NUMBER_MAX 100
#define FILE_PATH "./server.conf"

#ifndef IP_FREEBIND
#define IP_FREEBIND 15
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;
static char file_path[FILE_PATH_NUMBER_MAX] = FILE_PATH;
static char real_config_path[PATH_MAX];
static server_config_t cfg;

typedef enum event_context_type {
    EVENT_CONTEXT_LISTENER = 1,
    EVENT_CONTEXT_TCP_CONN
} event_context_type_t;

typedef struct event_context {
    event_context_type_t event_type;
    int fd;
} event_context_t;

typedef struct channel_context {
    event_context_t event;
    listener_config_t config;
} channel_context_t;

typedef enum task_type {
    TASK_TCP_CLIENT = 0,
    TASK_UDP_PACKET
} task_type_t;

typedef struct task {
    task_type_t task_type;
    listener_config_t channel;
    int client_fd;
    char peer[64];
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len;
    size_t data_len;
    char data[IO_BUF_SIZE];
    struct task *next;
} task_t;

typedef struct tcp_conn {
    event_context_t event;
    char peer[64];
    time_t last_active;

    char out_buf[IO_BUF_SIZE];
    size_t out_len;
    size_t out_sent;
    channel_context_t *channel_config;
    struct tcp_conn *next;
} tcp_conn_t;

typedef struct task_queue {
    task_t *head;
    task_t *tail;
    int stop;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
} task_queue_t;

static task_queue_t g_queue;
static tcp_conn_t *g_tcp_conn_head = NULL;

/* ================= 信号处理 ================= */

static void signal_handler(int signo)
{
    switch (signo) {
        case SIGTERM:
        case SIGINT:
            g_stop = 1;
            break;

        case SIGHUP:
            g_reload = 1;
            break;

        default:
            break;
    }
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return -1;
    }

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return -1;
    }

    if (sigaction(SIGHUP, &sa, NULL) == -1) {
        return -1;
    }

    /*
     * 忽略 SIGPIPE，避免 socket 对端关闭后 send/write 导致进程退出
     */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        return -1;
    }

    return 0;
}

/* ================= 守护进程化 ================= */

static int daemonize(void)
{
    pid_t pid;
    long maxfd;
    long i;
    int fd;

    pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid > 0) {
        _exit(0);
    }

    if (setsid() == -1) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid > 0) {
        _exit(0);
    }

    umask(0);

    if (chdir("/") == -1) {
        return -1;
    }

    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd == -1) {
        maxfd = 8192;
    }

    for (i = 0; i < maxfd; i++) {
        close((int)i);
    }

    fd = open("/dev/null", O_RDWR);
    if (fd == -1) {
        return -1;
    }

    if (fd != STDIN_FILENO) {
        if (dup2(fd, STDIN_FILENO) == -1) {
            return -1;
        }
    }

    if (dup2(STDIN_FILENO, STDOUT_FILENO) == -1) {
        return -1;
    }

    if (dup2(STDIN_FILENO, STDERR_FILENO) == -1) {
        return -1;
    }

    if (fd > STDERR_FILENO) {
        close(fd);
    }

    return 0;
}




/* ================= 任务队列 ================= */

static int task_queue_init(task_queue_t *q)
{
    memset(q, 0, sizeof(*q));

    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        return -1;
    }

    if (pthread_cond_init(&q->cond, NULL) != 0) {
        pthread_mutex_destroy(&q->mutex);
        return -1;
    }

    q->head = NULL;
    q->tail = NULL;
    q->stop = 0;

    return 0;
}

static void task_queue_destroy(task_queue_t *q)
{
    task_t *task;
    task_t *next;

    pthread_mutex_lock(&q->mutex);

    task = q->head;
    while (task != NULL) {
        next = task->next;
        if (task->task_type == TASK_TCP_CLIENT && task->client_fd >= 0) {
            close(task->client_fd);
        }
        free(task);
        task = next;
    }

    q->head = NULL;
    q->tail = NULL;

    pthread_mutex_unlock(&q->mutex);

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int task_queue_push(task_queue_t *q, task_t *task)
{
    task->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (q->stop) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    if (q->tail == NULL) {
        q->head = task;
        q->tail = task;
    } else {
        q->tail->next = task;
        q->tail = task;
    }

    /*
     * 通知一个正在等待的 worker：
     * 队列里有任务了，可以起来干活。
     */
    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);

    return 0;
}

static task_t *task_queue_pop(task_queue_t *q)
{
    task_t *task;

    pthread_mutex_lock(&q->mutex);

    /*
     * 队列为空，并且还没有停止，就等待。
     */
    while (q->head == NULL && !q->stop) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    /*
     * 如果 stop 了，并且队列也空了，worker 就可以退出。
     */
    if (q->head == NULL && q->stop) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    task = q->head;
    q->head = task->next;

    if (q->head == NULL) {
        q->tail = NULL;
    }

    pthread_mutex_unlock(&q->mutex);

    task->next = NULL;
    return task;
}

static void task_queue_stop(task_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);

    q->stop = 1;

    /*
     * 唤醒所有 worker。
     * 否则有些 worker 可能一直卡在 pthread_cond_wait。
     */
    pthread_cond_broadcast(&q->cond);

    pthread_mutex_unlock(&q->mutex);
}

/* ================= 工作线程 ================= */

static int send_all_or_close(int fd, const char *buf, size_t len);
static void process_tcp_proxy_client(task_t *task);
static void process_udp_proxy_packet(task_t *task);

static void process_task(task_t *task)
{
    char buf[IO_BUF_SIZE];
    int fd = task->client_fd;

    if (task->task_type == TASK_UDP_PACKET) {
        if(task->channel.service_type==SERVICE_TYPE_ECHO) {
            LOG_INFO("worker handle udp packet: fd=%d, peer=%s, service_type=%d",
                     fd,
                     task->peer,
                     task->channel.service_type);

            ssize_t n = sendto(fd,
                               task->data,
                               task->data_len,
                               0,
                               (struct sockaddr *) &task->peer_addr,
                               task->peer_addr_len);
            if (n == -1) {
                LOG_ERROR("udp sendto failed: fd=%d, peer=%s, errno=%d",
                          fd,
                          task->peer,
                          errno);
            } else {
                LOG_INFO("udp packet handled: fd=%d, peer=%s, bytes=%zd",
                         fd,
                         task->peer,
                         n);
            }
            return;
        }
        else{
            process_udp_proxy_packet(task);
            return;
        }
    }else if (task->task_type == TASK_TCP_CLIENT)
    {
        if (task->channel.service_type==SERVICE_TYPE_ECHO){
            LOG_INFO("worker handle client: fd=%d, peer=%s, service_type=%d",
                     fd,
                     task->peer,
                     task->channel.service_type);

            for (;;) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);

                if (n > 0) {
                    if (send_all_or_close(fd, buf, (size_t)n) == -1) {
                        break;
                    }
                    continue;
                }

                if (n == 0) {
                    LOG_INFO("client closed: fd=%d, peer=%s", fd, task->peer);
                    break;
                }

                if (errno == EINTR) {
                    continue;
                }

                LOG_ERROR("recv failed: fd=%d, peer=%s, errno=%d", fd, task->peer, errno);
                break;
            }

            close(fd);
            task->client_fd = -1;
        }else
        {
            process_tcp_proxy_client(task);
        }
    }
}
static void *worker_thread(void *arg)
{
    task_queue_t *q = (task_queue_t *)arg;
    task_t *task;

    LOG_INFO("worker start");

    while (1) {
        task = task_queue_pop(q);
        if (task == NULL) {
            break;
        }

        process_task(task);

        free(task);
    }

    LOG_INFO("worker exit");

    return NULL;
}

/* ================= 配置重载和清理 ================= */


static void cleanup(void)
{
    LOG_INFO("cleanup");
}

static void usage(const char *prog)
{
    printf("Usage: %s [-c config_file] [-d] [-h]\n", prog);
    printf("  -c    config file path\n");
    printf("  -d    run as daemon\n");
    printf("  -h    show help\n");
}

static int addfd(int epollfd, int fd, uint32_t ev, void *ptr)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.data.ptr = ptr;
    event.events = ev;

    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event) == -1) {
        LOG_ERROR("epoll_ctl add failed: fd=%d, errno=%d", fd, errno);
        return -1;
    }

    return 0;
}

static int modfd(int epollfd, int fd, uint32_t ev, void *ptr)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.data.ptr = ptr;
    event.events = ev;

    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event) == -1) {
        LOG_ERROR("epoll_ctl mod failed: fd=%d, errno=%d", fd, errno);
        return -1;
    }

    return 0;
}

static int setSocketOpt(int socketHandle, int level, int optname)
{
    int ret = 0;
    int reuse = 1;

    ret = setsockopt(socketHandle, level, optname, &reuse, sizeof(reuse));
    if (ret < 0)
    {
        LOG_ERROR("setsockopt Set socket options error.");
    }
    return ret;
}

static bool set_nonBlocking(int nfd)
{
    int flags = fcntl(nfd, F_GETFL, 0);

    if (flags == -1) {
        return false;
    }

    if (fcntl(nfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return false;
    }

    return true;
}

static int tcp_conn_has_pending_output(const tcp_conn_t *conn)
{
    return conn->out_sent < conn->out_len;
}

static uint32_t tcp_conn_events(const tcp_conn_t *conn)
{
    uint32_t events = EPOLLERR | EPOLLHUP;

#ifdef EPOLLRDHUP
    events |= EPOLLRDHUP;
#endif

    if (tcp_conn_has_pending_output(conn)) {
        events |= EPOLLOUT;
    } else {
        events |= EPOLLIN;
    }

    return events;
}

static int update_tcp_conn_events(int epollfd, tcp_conn_t *conn)
{
    return modfd(epollfd, conn->event.fd, tcp_conn_events(conn), conn);
}

static void tcp_conn_list_add(tcp_conn_t *conn)
{
    conn->next = g_tcp_conn_head;
    g_tcp_conn_head = conn;
}

static void tcp_conn_list_remove(tcp_conn_t *conn)
{
    tcp_conn_t **pp = &g_tcp_conn_head;

    while (*pp != NULL) {
        if (*pp == conn) {
            *pp = conn->next;
            conn->next = NULL;
            return;
        }

        pp = &(*pp)->next;
    }
}

static void close_tcp_conn(int epollfd, tcp_conn_t *conn, const char *reason)
{
    if (conn == NULL) {
        return;
    }

    tcp_conn_list_remove(conn);

    if (conn->event.fd >= 0) {
        if (epollfd >= 0) {
            epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->event.fd, NULL);
        }

        LOG_INFO("tcp echo connection closed: fd=%d, peer=%s, reason=%s",
                 conn->event.fd,
                 conn->peer,
                 reason != NULL ? reason : "unknown");
        close(conn->event.fd);
        conn->event.fd = -1;
    }

    free(conn);
}

static void close_all_tcp_conns(int epollfd)
{
    while (g_tcp_conn_head != NULL) {
        close_tcp_conn(epollfd, g_tcp_conn_head, "server shutdown");
    }
}

static void close_idle_tcp_conns(int epollfd)
{
    time_t now = time(NULL);
    tcp_conn_t *conn = g_tcp_conn_head;

    while (conn != NULL) {
        tcp_conn_t *next = conn->next;

        if (now != (time_t)-1 &&
            conn->last_active != (time_t)-1 &&
            now - conn->last_active >= TCP_CONN_IDLE_TIMEOUT_SEC) {
            close_tcp_conn(epollfd, conn, "idle timeout");
        }

        conn = next;
    }
}

static int flush_tcp_echo_output(int epollfd, tcp_conn_t *conn)
{
    while (conn->out_sent < conn->out_len) {
        ssize_t n = send(conn->event.fd,
                         conn->out_buf + conn->out_sent,
                         conn->out_len - conn->out_sent,
                         MSG_NOSIGNAL);

        if (n > 0) {
            conn->out_sent += (size_t)n;
            conn->last_active = time(NULL);
            continue;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return update_tcp_conn_events(epollfd, conn);
        }

        LOG_ERROR("tcp echo send failed: fd=%d, peer=%s, errno=%d",
                  conn->event.fd,
                  conn->peer,
                  errno);
        return -1;
    }

    conn->out_len = 0;
    conn->out_sent = 0;

    return update_tcp_conn_events(epollfd, conn);
}

static int send_or_buffer_tcp_echo(int epollfd, tcp_conn_t *conn, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(conn->event.fd, buf + sent, len - sent, MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            conn->last_active = time(NULL);
            continue;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            size_t left = len - sent;

            if (left > sizeof(conn->out_buf)) {
                LOG_ERROR("tcp echo output buffer too small: fd=%d, peer=%s, left=%zu",
                          conn->event.fd,
                          conn->peer,
                          left);
                return -1;
            }

            memcpy(conn->out_buf, buf + sent, left);
            conn->out_len = left;
            conn->out_sent = 0;
            return update_tcp_conn_events(epollfd, conn);
        }

        LOG_ERROR("tcp echo send failed: fd=%d, peer=%s, errno=%d",
                  conn->event.fd,
                  conn->peer,
                  errno);
        return -1;
    }

    return 0;
}

static int handle_tcp_echo_read(int epollfd, tcp_conn_t *conn)
{
    char buf[IO_BUF_SIZE];

    while (!tcp_conn_has_pending_output(conn)) {
        ssize_t n = recv(conn->event.fd, buf, sizeof(buf), 0);

        if (n > 0) {
            conn->last_active = time(NULL);

            if (send_or_buffer_tcp_echo(epollfd, conn, buf, (size_t)n) == -1) {
                return -1;
            }

            continue;
        }

        if (n == 0) {
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return update_tcp_conn_events(epollfd, conn);
        }

        LOG_ERROR("tcp echo recv failed: fd=%d, peer=%s, errno=%d",
                  conn->event.fd,
                  conn->peer,
                  errno);
        return -1;
    }

    return update_tcp_conn_events(epollfd, conn);
}

static void handle_tcp_echo_event(int epollfd, tcp_conn_t *conn, uint32_t event_flags)
{
    int peer_closed = 0;

    if (conn == NULL || conn->event.fd < 0) {
        return;
    }

    if ((event_flags & EPOLLERR) != 0) {
        close_tcp_conn(epollfd, conn, "socket error");
        return;
    }

#ifdef EPOLLRDHUP
    if ((event_flags & EPOLLRDHUP) != 0) {
        peer_closed = 1;
    }
#endif

    if ((event_flags & EPOLLHUP) != 0) {
        peer_closed = 1;
    }

    if ((event_flags & EPOLLOUT) != 0) {
        if (flush_tcp_echo_output(epollfd, conn) == -1) {
            close_tcp_conn(epollfd, conn, "send failed");
            return;
        }
    }

    if ((event_flags & EPOLLIN) != 0) {
        if (handle_tcp_echo_read(epollfd, conn) == -1) {
            close_tcp_conn(epollfd, conn, "recv closed or failed");
            return;
        }
    }

    if (peer_closed) {
        close_tcp_conn(epollfd, conn, "peer closed");
    }
}

static bool set_blocking(int nfd)
{
    int flags = fcntl(nfd, F_GETFL, 0);

    if (flags == -1) {
        return false;
    }

    if (fcntl(nfd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        return false;
    }

    return true;
}

static int send_all_or_close(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
       //被信号中断，继续发送
        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            LOG_WARN("send would block, close slow client: fd=%d", fd);
            return -1;
        }

        LOG_ERROR("send failed: fd=%d, errno=%d", fd, errno);
        return -1;
    }

    return 0;
}

static int build_proxy_target_addr(const listener_config_t *channel, struct sockaddr_in *target_addr)
{
    if (channel == NULL || target_addr == NULL) {
        return -1;
    }

    if (channel->target_ip[0] == '\0' ||
        channel->target_port <= 0 ||
        channel->target_port > 65535) {
        LOG_ERROR("invalid proxy target config: ip=%s, port=%d",
                  channel->target_ip,
                  channel->target_port);
        return -1;
    }

    memset(target_addr, 0, sizeof(*target_addr));
    target_addr->sin_family = AF_INET;
    target_addr->sin_port = htons((uint16_t)channel->target_port);

    if (inet_pton(AF_INET, channel->target_ip, &target_addr->sin_addr) != 1) {
        LOG_ERROR("invalid proxy target ip: %s", channel->target_ip);
        return -1;
    }

    return 0;
}

static int connect_tcp_proxy_target(const listener_config_t *channel)
{
    int upstream_fd;
    int ret;
    struct sockaddr_in target_addr;

    if (build_proxy_target_addr(channel, &target_addr) == -1) {
        return -1;
    }

    upstream_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (upstream_fd == -1) {
        LOG_ERROR("tcp proxy target socket failed: errno=%d", errno);
        return -1;
    }

    do {
        ret = connect(upstream_fd, (struct sockaddr *)&target_addr, sizeof(target_addr));
    } while (ret == -1 && errno == EINTR);

    if (ret == -1) {
        LOG_ERROR("tcp proxy connect target failed: target=%s:%d, errno=%d",
                  channel->target_ip,
                  channel->target_port,
                  errno);
        close(upstream_fd);
        return -1;
    }

    return upstream_fd;
}

static int relay_proxy_data(int from_fd,
                            int to_fd,
                            const char *from_name,
                            const char *to_name,
                            const char *peer)
{
    char buf[IO_BUF_SIZE];

    for (;;) {
        ssize_t n = recv(from_fd, buf, sizeof(buf), 0);

        if (n > 0) {
            if (send_all_or_close(to_fd, buf, (size_t)n) == -1) {
                LOG_ERROR("tcp proxy send failed: from=%s, to=%s, peer=%s",
                          from_name,
                          to_name,
                          peer);
                return -1;
            }

            return 1;
        }

        if (n == 0) {
            LOG_INFO("tcp proxy peer closed: side=%s, peer=%s", from_name, peer);
            return 0;
        }

        if (errno == EINTR) {
            continue;
        }

        LOG_ERROR("tcp proxy recv failed: side=%s, peer=%s, errno=%d",
                  from_name,
                  peer,
                  errno);
        return -1;
    }
}

static int relay_tcp_proxy_streams(int client_fd,
                                   int upstream_fd,
                                   const char *peer,
                                   const listener_config_t *channel)
{
    struct pollfd fds[2];

    for (;;) {
        int ret;

        memset(fds, 0, sizeof(fds));
        fds[0].fd = client_fd;
        fds[0].events = POLLIN;
        fds[1].fd = upstream_fd;
        fds[1].events = POLLIN;

        ret = poll(fds, 2, -1);
        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }

            LOG_ERROR("tcp proxy poll failed: peer=%s, target=%s:%d, errno=%d",
                      peer,
                      channel->target_ip,
                      channel->target_port,
                      errno);
            return -1;
        }

        if ((fds[0].revents & POLLIN) != 0) {
            ret = relay_proxy_data(client_fd, upstream_fd, "client", "target", peer);
            if (ret <= 0) {
                return ret;
            }
        }

        if ((fds[1].revents & POLLIN) != 0) {
            ret = relay_proxy_data(upstream_fd, client_fd, "target", "client", peer);
            if (ret <= 0) {
                return ret;
            }
        }

        if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            LOG_INFO("tcp proxy client event: peer=%s, events=%d", peer, fds[0].revents);
            return 0;
        }

        if ((fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            LOG_INFO("tcp proxy target event: peer=%s, target=%s:%d, events=%d",
                     peer,
                     channel->target_ip,
                     channel->target_port,
                     fds[1].revents);
            return 0;
        }
    }
}

static void process_tcp_proxy_client(task_t *task)
{
    int client_fd = task->client_fd;
    int upstream_fd;

    LOG_INFO("tcp proxy start: client_fd=%d, peer=%s, target=%s:%d",
             client_fd,
             task->peer,
             task->channel.target_ip,
             task->channel.target_port);

    upstream_fd = connect_tcp_proxy_target(&task->channel);
    if (upstream_fd == -1) {
        close(client_fd);
        task->client_fd = -1;
        return;
    }

    (void)relay_tcp_proxy_streams(client_fd, upstream_fd, task->peer, &task->channel);

    close(upstream_fd);
    close(client_fd);
    task->client_fd = -1;

    LOG_INFO("tcp proxy closed: peer=%s, target=%s:%d",
             task->peer,
             task->channel.target_ip,
             task->channel.target_port);
}

static void process_udp_proxy_packet(task_t *task)
{
    int upstream_fd;
    int ret;
    char buf[IO_BUF_SIZE];
    struct sockaddr_in target_addr;
    struct pollfd pfd;
    ssize_t n;

    if (build_proxy_target_addr(&task->channel, &target_addr) == -1) {
        return;
    }

    upstream_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (upstream_fd == -1) {
        LOG_ERROR("udp proxy target socket failed: peer=%s, errno=%d",
                  task->peer,
                  errno);
        return;
    }

    if (connect(upstream_fd, (struct sockaddr *)&target_addr, sizeof(target_addr)) == -1) {
        LOG_ERROR("udp proxy connect target failed: peer=%s, target=%s:%d, errno=%d",
                  task->peer,
                  task->channel.target_ip,
                  task->channel.target_port,
                  errno);
        close(upstream_fd);
        return;
    }

    n = send(upstream_fd, task->data, task->data_len, 0);
    if (n == -1 || n != (ssize_t)task->data_len) {
        LOG_ERROR("udp proxy send target failed: peer=%s, target=%s:%d, bytes=%zu, sent=%zd, errno=%d",
                  task->peer,
                  task->channel.target_ip,
                  task->channel.target_port,
                  task->data_len,
                  n,
                  errno);
        close(upstream_fd);
        return;
    }

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = upstream_fd;
    pfd.events = POLLIN;

    do {
        ret = poll(&pfd, 1, UDP_PROXY_TIMEOUT_MS);
    } while (ret == -1 && errno == EINTR);

    if (ret == 0) {
        LOG_WARN("udp proxy target timeout: peer=%s, target=%s:%d",
                 task->peer,
                 task->channel.target_ip,
                 task->channel.target_port);
        close(upstream_fd);
        return;
    }

    if (ret == -1) {
        LOG_ERROR("udp proxy poll target failed: peer=%s, target=%s:%d, errno=%d",
                  task->peer,
                  task->channel.target_ip,
                  task->channel.target_port,
                  errno);
        close(upstream_fd);
        return;
    }

    if ((pfd.revents & POLLIN) == 0) {
        LOG_ERROR("udp proxy target event: peer=%s, target=%s:%d, events=%d",
                  task->peer,
                  task->channel.target_ip,
                  task->channel.target_port,
                  pfd.revents);
        close(upstream_fd);
        return;
    }

    n = recv(upstream_fd, buf, sizeof(buf), 0);
    if (n == -1) {
        LOG_ERROR("udp proxy recv target failed: peer=%s, target=%s:%d, errno=%d",
                  task->peer,
                  task->channel.target_ip,
                  task->channel.target_port,
                  errno);
        close(upstream_fd);
        return;
    }

    ret = (int)sendto(task->client_fd,
                      buf,
                      (size_t)n,
                      0,
                      (struct sockaddr *)&task->peer_addr,
                      task->peer_addr_len);
    if (ret == -1) {
        LOG_ERROR("udp proxy send client failed: peer=%s, errno=%d",
                  task->peer,
                  errno);
    } else {
        LOG_INFO("udp proxy packet handled: peer=%s, target=%s:%d, bytes=%zd",
                 task->peer,
                 task->channel.target_ip,
                 task->channel.target_port,
                 n);
    }

    close(upstream_fd);
}

static int dispatch_tcp_proxy_client(int epoll_fd,channel_context_t *channel,
                                     int client_fd,
                                     const struct sockaddr_in *peer_addr)
{
    char peer_ip[INET_ADDRSTRLEN] = {0};
    tcp_conn_t *conn;

    if (!set_blocking(client_fd)) {
        LOG_ERROR("set proxy client blocking failed: fd=%d, errno=%d", client_fd, errno);
        close(client_fd);
        return -1;
    }

    if (inet_ntop(AF_INET, &peer_addr->sin_addr, peer_ip, sizeof(peer_ip)) == NULL) {
        snprintf(peer_ip, sizeof(peer_ip), "unknown");
    }

    conn = malloc(sizeof(*conn));
    if (conn == NULL) {
        LOG_ERROR("malloc proxy client task failed: fd=%d", client_fd);
        close(client_fd);
        return -1;
    }

    memset(conn, 0, sizeof(*conn));
    conn->event.event_type = EVENT_CONTEXT_TCP_CONN;
    conn->event.fd = client_fd;
    conn->last_active = time(NULL);
    conn->channel_config = channel;
    if (inet_ntop(AF_INET, &peer_addr->sin_addr, peer_ip, sizeof(peer_ip)) == NULL) {
        snprintf(peer_ip, sizeof(peer_ip), "unknown");
    }

    snprintf(conn->peer,
             sizeof(conn->peer),
             "%s:%u",
             peer_ip,
             (unsigned int)ntohs(peer_addr->sin_port));

    if (addfd(epoll_fd, client_fd, tcp_conn_events(conn), conn) == -1) {
        close(client_fd);
        free(conn);
        return -1;
    }

    tcp_conn_list_add(conn);

    LOG_INFO("tcp echo client accepted: fd=%d, peer=%s",
             client_fd,
             conn->peer);

    return 0;
}

static int add_tcp_echo_client(int epoll_fd,
                               channel_context_t *channel,
                               int client_fd,
                               const struct sockaddr_in *peer_addr)
{
    char peer_ip[INET_ADDRSTRLEN] = {0};
    tcp_conn_t *conn;

    if (!set_nonBlocking(client_fd)) {
        LOG_ERROR("set echo client nonblocking failed: fd=%d, errno=%d", client_fd, errno);
        close(client_fd);
        return -1;
    }

    conn = malloc(sizeof(*conn));
    if (conn == NULL) {
        LOG_ERROR("malloc tcp_conn_t failed: fd=%d", client_fd);
        close(client_fd);
        return -1;
    }

    memset(conn, 0, sizeof(*conn));
    conn->event.event_type = EVENT_CONTEXT_TCP_CONN;
    conn->event.fd = client_fd;
    conn->last_active = time(NULL);
    conn->channel_config = channel;

    if (inet_ntop(AF_INET, &peer_addr->sin_addr, peer_ip, sizeof(peer_ip)) == NULL) {
        snprintf(peer_ip, sizeof(peer_ip), "unknown");
    }

    snprintf(conn->peer,
             sizeof(conn->peer),
             "%s:%u",
             peer_ip,
             (unsigned int)ntohs(peer_addr->sin_port));

    if (addfd(epoll_fd, client_fd, tcp_conn_events(conn), conn) == -1) {
        close(client_fd);
        free(conn);
        return -1;
    }

    tcp_conn_list_add(conn);

    LOG_INFO("tcp echo client accepted: fd=%d, peer=%s",
             client_fd,
             conn->peer);

    return 0;
}

static void accept_clients(channel_context_t *channel, int epoll_fd)
{
    int listen_fd = channel->event.fd;

    for (;;) {
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_len);

        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            LOG_ERROR("accept failed: errno=%d", errno);
            return;
        }

        if (channel->config.service_type == SERVICE_TYPE_ECHO) {
            (void)add_tcp_echo_client(epoll_fd, channel, client_fd, &peer_addr);
        } else {
            (void)dispatch_tcp_proxy_client(epoll_fd,channel, client_fd, &peer_addr);
        }
    }
}

static void dispatch_udp_packets(channel_context_t *channel)
{
    int udp_fd = channel->event.fd;

    for (;;) {
        task_t *task;
        char peer_ip[INET_ADDRSTRLEN] = {0};
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        ssize_t n;

        task = malloc(sizeof(*task));
        if (task == NULL) {
            LOG_ERROR("malloc udp task failed: fd=%d", udp_fd);
            return;
        }

        memset(task, 0, sizeof(*task));
        n = recvfrom(udp_fd,
                     task->data,
                     sizeof(task->data),
                     0,
                     (struct sockaddr *)&peer_addr,
                     &peer_len);

        if (n == -1) {
            free(task);

            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            LOG_ERROR("udp recvfrom failed: fd=%d, errno=%d", udp_fd, errno);
            return;
        }

        task->task_type = TASK_UDP_PACKET;
        task->channel = channel->config;
        task->client_fd = udp_fd;
        task->peer_addr = peer_addr;
        task->peer_addr_len = peer_len;
        task->data_len = (size_t)n;

        if (inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip)) == NULL) {
            snprintf(peer_ip, sizeof(peer_ip), "unknown");
        }

        snprintf(task->peer,
                 sizeof(task->peer),
                 "%s:%u",
                 peer_ip,
                 (unsigned int)ntohs(peer_addr.sin_port));

        if (task_queue_push(&g_queue, task) == -1) {
            free(task);
            return;
        }

        LOG_INFO("udp packet dispatched: fd=%d, peer=%s, bytes=%zd, service_type=%d",
                 udp_fd,
                 task->peer,
                 n,
                 task->channel.service_type);
    }
}

static int createListenSocket(const char *listenIp, uint16_t listenPort)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG_ERROR("Error socket\n");
        return -1;
    }

    if (setSocketOpt(listen_fd, SOL_SOCKET, SO_REUSEADDR) < 0)
    {
        LOG_ERROR("createListenSocket Option setting failed\n");
        close(listen_fd);
        return -1;
    }

    // 设置监听socket为非阻塞
    if (!set_nonBlocking(listen_fd)) {
        close(listen_fd);
        LOG_ERROR("Error set_nonBlocking\n");
        return -1;
    }
    const int one = 1;
    if (setsockopt(listen_fd, IPPROTO_IP, IP_FREEBIND, &one, sizeof(one)) < 0)
    {
        close(listen_fd);
        return -1;
    }
    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, listenIp, &listen_addr.sin_addr) != 1) {
        LOG_ERROR("invalid listen ip: %s", listenIp);
        close(listen_fd);
        return -1;
    }
    listen_addr.sin_port = htons(listenPort);
    if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        LOG_ERROR("Error bind\n");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, BACKLOG) < 0) {
        LOG_ERROR("Error listen\n");
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

static int createUdpSocket(const char *listenIp, uint16_t listenPort)
{
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        LOG_ERROR("Error udp socket\n");
        return -1;
    }

    if (setSocketOpt(udp_fd, SOL_SOCKET, SO_REUSEADDR) < 0)
    {
        LOG_ERROR("createUdpSocket Option setting failed\n");
        close(udp_fd);
        return -1;
    }

    if (!set_nonBlocking(udp_fd)) {
        close(udp_fd);
        LOG_ERROR("Error udp set_nonBlocking\n");
        return -1;
    }

    const int one = 1;
    if (setsockopt(udp_fd, IPPROTO_IP, IP_FREEBIND, &one, sizeof(one)) < 0)
    {
        close(udp_fd);
        return -1;
    }

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, listenIp, &listen_addr.sin_addr) != 1) {
        LOG_ERROR("invalid udp listen ip: %s", listenIp);
        close(udp_fd);
        return -1;
    }
    listen_addr.sin_port = htons(listenPort);

    if (bind(udp_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        LOG_ERROR("Error udp bind\n");
        close(udp_fd);
        return -1;
    }

    return udp_fd;
}
/* ================= main ================= */

int main_t(int argc, char *argv[])
{
    int daemon_mode = 0;
    int opt;
    int i;
    pthread_t *workers;

    while ((opt = getopt(argc, argv, "dhc:")) != -1) {
        switch (opt) {
            case 'd':
                daemon_mode = 1;
                break;

            case 'h':
                usage(argv[0]);
                return 0;
            case 'c':
                {
                    snprintf(file_path, sizeof(file_path), "%s", optarg);
                }
                break;

            default:
                usage(argv[0]);
                return 1;
        }
    }
    if (realpath(file_path, real_config_path) == NULL) {
        fprintf(stderr, "invalid config path: %s\n", file_path);
        return 1;
    }
    if (daemon_mode) {
        if (daemonize() == -1) {
            return 1;
        }
    }

    if (install_signal_handlers() == -1) {
        return 1;
    }


    int ret = load_config(real_config_path);
    if (ret == -1) {
        LOG_ERROR("load_config failed");
        return 1;
    }
    if (get_config_snapshot(&cfg) == -1) {
        LOG_ERROR("get config snapshot failed");
        return -1;
    }


    app_log_init(cfg.log_file,cfg.log_level);
    if (task_queue_init(&g_queue) == -1) {
        LOG_ERROR("task_queue_init failed");
        free_config();
        return 1;
    }
    LOG_INFO("program start");
    LOG_INFO("listener_count=%d", cfg.listener_count);
    for (i = 0; i < cfg.listener_count; i++) {
        LOG_INFO("listener[%d]: listen_type=%s, listen_ip=%s, listen_port=%d, type=%s, service_type=%d, target=%s:%d",
                 i,
                 cfg.listeners[i].listen_type,
                 cfg.listeners[i].listen_ip,
                 cfg.listeners[i].listen_port,
                 cfg.listeners[i].type,
                 cfg.listeners[i].service_type,
                 cfg.listeners[i].target_ip,
                 cfg.listeners[i].target_port);
    }
    /*
     * 创建 worker 线程。
     */
    int worker_num = cfg.worker_num;
    workers = malloc(sizeof(pthread_t) * worker_num);
    if(workers ==NULL)
    {
        LOG_ERROR("malloc workers failed");
        free_config();
        cleanup();
        app_log_close();
        task_queue_destroy(&g_queue);
        return 1;
    }
    int created_workers = 0;
    for (i = 0; i < worker_num; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread, &g_queue) != 0) {
            LOG_ERROR("pthread_create failed");
            g_stop = 1;
            break;
        }
        created_workers++;
    }

    /*
     * 主线程循环。
     * 这里先模拟每 2 秒产生一个任务。
     * 以后你可以替换成 accept/recv/epoll 等逻辑。
     */
    // 等待监听
    struct epoll_event events[MAX_EVENTS];
    int epollfd = epoll_create1(EPOLL_CLOEXEC);
    channel_context_t channels[MAX_LISTENERS];
    int listen_count = 0;

    for (i = 0; i < MAX_LISTENERS; i++) {
        memset(&channels[i], 0, sizeof(channels[i]));
        channels[i].event.event_type = EVENT_CONTEXT_LISTENER;
        channels[i].event.fd = -1;
    }

    if (epollfd == -1) {
        LOG_ERROR("epoll_create1 failed: errno=%d", errno);
        goto quit;
    }

    for (i = 0; i < cfg.listener_count; i++) {
        listener_config_t *listener = &cfg.listeners[i];
        int listen_fd;

        if (listener->proto == LISTEN_PROTO_UDP) {
            listen_fd = createUdpSocket(listener->listen_ip, (uint16_t)listener->listen_port);
        } else {
            listen_fd = createListenSocket(listener->listen_ip, (uint16_t)listener->listen_port);
        }

        if (listen_fd < 0){
            goto quit;
        }

        channels[listen_count].event.event_type = EVENT_CONTEXT_LISTENER;
        channels[listen_count].event.fd = listen_fd;
        channels[listen_count].config = *listener;

        if (addfd(epollfd,
                  listen_fd,
                  EPOLLIN | EPOLLERR | EPOLLHUP,
                  &channels[listen_count]) == -1) {
            close(listen_fd);
            channels[listen_count].event.fd = -1;
            goto quit;
        }

        listen_count++;

        LOG_INFO("listener started: fd=%d, listen_type=%s, ip=%s, port=%d, type=%s, service_type=%d, target=%s:%d",
                 listen_fd,
                 listener->listen_type,
                 listener->listen_ip,
                 listener->listen_port,
                 listener->type,
                 listener->service_type,
                 listener->target_ip,
                 listener->target_port);
    }

    if (listen_count == 0) {
        LOG_ERROR("no listener configured");
        goto quit;
    }

    while (!g_stop) {
        if (g_reload) {
            g_reload = 0;
            if(reload_config(real_config_path)==-1){
                LOG_ERROR("reload config failed: path=%s", real_config_path);
            } else{
                if (get_config_snapshot(&cfg) == -1) {
                    LOG_ERROR("get config snapshot failed");

                }else{
                    if(app_log_init(cfg.log_file,cfg.log_level)==-1){
                        LOG_ERROR("reopen log failed: %s", cfg.log_file);
                    }
                }
                LOG_INFO("reload config success: path=%s", real_config_path);
            }
        }

        //等待epoll事件
        int num_events = epoll_wait(epollfd, events, MAX_EVENTS, 1000);
        if(num_events < 0){
            if (errno != EINTR) {
                LOG_ERROR("ERROR epoll_wait , num_events=%d, errno=%d\n", num_events, errno);
                break;
            } else {
                continue;
            }
        }
        for(int j =0 ;j<num_events;j++){
            event_context_t *event_context = (event_context_t *)events[j].data.ptr;
            uint32_t event_flags = events[j].events;

            if (event_context == NULL || event_context->fd < 0) {
                LOG_WARN("unexpected epoll event: ptr=%p, events=%u",
                         (void *)event_context,
                         event_flags);
                continue;
            }
            //触发了开始回包
            if (event_context->event_type == EVENT_CONTEXT_TCP_CONN) {
                tcp_conn_t *  event_con = (tcp_conn_t *)event_context;
                if (event_con->channel_config->config.service_type == SERVICE_TYPE_ECHO )
                {
                    handle_tcp_echo_event(epollfd, (tcp_conn_t *)event_con, event_flags);
                }else if (event_con->channel_config->config.service_type == SERVICE_TYPE_PROXY)
                {
                    //tcp proxy 直接交给worker处理

                }

                continue;
            }

            if (event_context->event_type != EVENT_CONTEXT_LISTENER) {
                LOG_WARN("unknown epoll event type: fd=%d, type=%d, events=%u",
                         event_context->fd,
                         event_context->event_type,
                         event_flags);
                continue;
            }

            channel_context_t *channel = (channel_context_t *)event_context;

            if (event_flags & (EPOLLERR | EPOLLHUP)) {
                LOG_ERROR("listen socket error: fd=%d, events=%u", channel->event.fd, event_flags);
                g_stop = 1;
                break;
            }

            if (event_flags & EPOLLIN) {
                if (channel->config.proto == LISTEN_PROTO_UDP) {
                    dispatch_udp_packets(channel);
                } else {
                    accept_clients(channel, epollfd);
                }
            }
        }

        close_idle_tcp_conns(epollfd);
    }
quit:
    close_all_tcp_conns(epollfd);

    for (i = 0; i < listen_count; i++) {
        if (channels[i].event.fd >= 0) {
            close(channels[i].event.fd);
            channels[i].event.fd = -1;
        }
    }

    if (epollfd >= 0) {
        close(epollfd);
    }

    /*
     * 通知所有 worker 准备退出。
     */
    task_queue_stop(&g_queue);

    /*
     * 等待 worker 全部退出。
     */
    for (i = 0; i < created_workers; i++) {
        pthread_join(workers[i], NULL);
    }
    task_queue_destroy(&g_queue);
    free(workers);
    workers =NULL;

    cleanup();
    LOG_INFO("program exit");
    app_log_close();
    free_config();




    return 0;
}
