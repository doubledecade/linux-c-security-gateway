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
#include <stdbool.h>
#include "config.h"
#include "log.h"
#define BACKLOG 65535
#define MAX_EVENTS 1024
#define IO_BUF_SIZE 4096
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

typedef struct channel_context {
    int fd;
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

typedef struct task_queue {
    task_t *head;
    task_t *tail;
    int stop;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
} task_queue_t;

static task_queue_t g_queue;

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
            //代理模式
            return;
        }
    }

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

static void accept_clients(channel_context_t *channel)
{
    int listen_fd = channel->fd;

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

        if (!set_blocking(client_fd)) {
            LOG_ERROR("set client blocking failed: fd=%d, errno=%d", client_fd, errno);
            close(client_fd);
            continue;
        }

        char peer_ip[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip)) == NULL) {
            snprintf(peer_ip, sizeof(peer_ip), "unknown");
        }

        task_t *task = malloc(sizeof(*task));
        if (task == NULL) {
            LOG_ERROR("malloc client task failed: fd=%d", client_fd);
            close(client_fd);
            continue;
        }

        memset(task, 0, sizeof(*task));
        task->task_type = TASK_TCP_CLIENT;
        task->channel = channel->config;
        task->client_fd = client_fd;
        task->next = NULL;
        snprintf(task->peer,
                 sizeof(task->peer),
                 "%s:%u",
                 peer_ip,
                 (unsigned int)ntohs(peer_addr.sin_port));

        if (task_queue_push(&g_queue, task) == -1) {
            close(client_fd);
            free(task);
            return;
        }

        LOG_INFO("client dispatched: fd=%d, peer=%s, service_type=%d",
                 client_fd,
                 task->peer,
                 task->channel.service_type);
    }
}

static void dispatch_udp_packets(channel_context_t *channel)
{
    int udp_fd = channel->fd;

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
        LOG_INFO("listener[%d]: listen_type=%s, listen_ip=%s, listen_port=%d, type=%s, service_type=%d",
                 i,
                 cfg.listeners[i].listen_type,
                 cfg.listeners[i].listen_ip,
                 cfg.listeners[i].listen_port,
                 cfg.listeners[i].type,
                 cfg.listeners[i].service_type);
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
        channels[i].fd = -1;
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

        channels[listen_count].fd = listen_fd;
        channels[listen_count].config = *listener;

        if (addfd(epollfd,
                  listen_fd,
                  EPOLLIN | EPOLLERR | EPOLLHUP,
                  &channels[listen_count]) == -1) {
            close(listen_fd);
            channels[listen_count].fd = -1;
            goto quit;
        }

        listen_count++;

        LOG_INFO("listener started: fd=%d, listen_type=%s, ip=%s, port=%d, type=%s, service_type=%d",
                 listen_fd,
                 listener->listen_type,
                 listener->listen_ip,
                 listener->listen_port,
                 listener->type,
                 listener->service_type);
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
            channel_context_t *channel = (channel_context_t *)events[j].data.ptr;
            uint32_t event_flags = events[j].events;

            if (channel == NULL || channel->fd < 0) {
                LOG_WARN("unexpected epoll event: ptr=%p, events=%u",
                         (void *)channel,
                         event_flags);
                continue;
            }

            if (event_flags & (EPOLLERR | EPOLLHUP)) {
                LOG_ERROR("listen socket error: fd=%d, events=%u", channel->fd, event_flags);
                g_stop = 1;
                break;
            }

            if (event_flags & EPOLLIN) {
                if (channel->config.proto == LISTEN_PROTO_UDP) {
                    dispatch_udp_packets(channel);
                } else {
                    accept_clients(channel);
                }
            }
        }
    }
quit:
    for (i = 0; i < listen_count; i++) {
        if (channels[i].fd >= 0) {
            close(channels[i].fd);
            channels[i].fd = -1;
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
