/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
 
#include "monitor_ioctl.h"
 
/* ------------------------------------------------------------------ */
/* Constants (match professor's boilerplate names)                      */
/* ------------------------------------------------------------------ */
#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  256
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)
#define DEFAULT_HARD_LIMIT   (64UL << 20)
#define MAX_CONTAINERS       32
 
/* ------------------------------------------------------------------ */
/* Types (from professor's boilerplate — kept exactly)                  */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0, CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP
} command_kind_t;
 
typedef enum {
    CONTAINER_STARTING = 0, CONTAINER_RUNNING, CONTAINER_STOPPED,
    CONTAINER_KILLED, CONTAINER_EXITED, CONTAINER_HARD_LIMIT_KILLED
} container_state_t;
 
typedef struct container_record {
    char              id[CONTAINER_ID_LEN];
    pid_t             host_pid;
    time_t            started_at;
    container_state_t state;
    unsigned long     soft_limit_bytes;
    unsigned long     hard_limit_bytes;
    int               exit_code;
    int               exit_signal;
    char              log_path[PATH_MAX];
    int               stop_requested;   /* set before signalling stop */
    int               pipe_rd;          /* supervisor reads container stdout here */
    struct container_record *next;
} container_record_t;
 
typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;
 
typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t     head;
    size_t     tail;
    size_t     count;
    int        shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;
 
typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
    int            wait_for_exit;   /* 1 for "run", 0 for "start" */
} control_request_t;
 
typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;
 
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  pipe_wr;   /* child writes stdout here */
    int  err_wr;    /* child writes stderr here */
} child_config_t;
 
typedef struct {
    int               server_fd;
    int               monitor_fd;
    int               should_stop;
    pthread_t         logger_thread;
    bounded_buffer_t  log_buffer;
    pthread_mutex_t   metadata_lock;
    container_record_t *containers;   /* linked list head */
} supervisor_ctx_t;
 
/* ------------------------------------------------------------------ */
/* Global supervisor context (set by supervisor, used by signal handler)*/
/* ------------------------------------------------------------------ */
static supervisor_ctx_t *g_ctx = NULL;
 
/* ------------------------------------------------------------------ */
/* Utility                                                              */
/* ------------------------------------------------------------------ */
static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:          return "starting";
    case CONTAINER_RUNNING:           return "running";
    case CONTAINER_STOPPED:           return "stopped";
    case CONTAINER_KILLED:            return "killed";
    case CONTAINER_EXITED:            return "exited";
    case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
    default:                          return "unknown";
    }
}
 
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}
 
static int parse_mib_flag(const char *flag, const char *value, unsigned long *out)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    *out = mib * (1UL << 20);
    return 0;
}
 
static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start)
{
    int i;
    for (i = start; i < argc; i += 2) {
        char *end = NULL;
        long nv;
        if (i + 1 >= argc) { fprintf(stderr, "Missing value for %s\n", argv[i]); return -1; }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes)) return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes)) return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nv = strtol(argv[i+1], &end, 10);
            if (errno || end == argv[i+1] || *end || nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid --nice value: %s\n", argv[i+1]); return -1;
            }
            req->nice_value = (int)nv;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]); return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n"); return -1;
    }
    return 0;
}
 
/* ------------------------------------------------------------------ */
/* Bounded buffer — fully implemented                                   */
/* ------------------------------------------------------------------ */
static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    rc = pthread_mutex_init(&b->mutex, NULL); if (rc) return rc;
    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc) { pthread_mutex_destroy(&b->mutex); return rc; }
    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc) { pthread_cond_destroy(&b->not_empty); pthread_mutex_destroy(&b->mutex); return rc; }
    return 0;
}
 
static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}
 
static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}
 
/* Producer: insert log chunk into buffer.
 * Blocks if full. Returns 0 on success, -1 if shutting down. */
static int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}
 
/* Consumer: remove log chunk from buffer.
 * Blocks if empty. Returns 0 on success, -1 when shutdown+empty. */
static int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}
 
/* ------------------------------------------------------------------ */
/* Logging consumer thread                                              */
/* ------------------------------------------------------------------ */
static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
 
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        /* Route to per-container log file under logs/<id>.log */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t w = write(fd, item.data, item.length);
            (void)w;
            close(fd);
        }
    }
    return NULL;
}
 
/* ------------------------------------------------------------------ */
/* Per-container pipe reader thread (producer into bounded buffer)      */
/* ------------------------------------------------------------------ */
typedef struct {
    supervisor_ctx_t *ctx;
    int               pipe_fd;
    char              container_id[CONTAINER_ID_LEN];
} reader_args_t;
 
static void *pipe_reader_thread(void *arg)
{
    reader_args_t *ra = (reader_args_t *)arg;
    log_item_t item;
    ssize_t n;
 
    memset(&item, 0, sizeof(item));
    snprintf(item.container_id, CONTAINER_ID_LEN, "%s", ra->container_id);
 
    while ((n = read(ra->pipe_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(&ra->ctx->log_buffer, &item);
        memset(item.data, 0, LOG_CHUNK_SIZE);
    }
    close(ra->pipe_fd);
    free(ra);
    return NULL;
}
 
/* ------------------------------------------------------------------ */
/* Container child entrypoint (runs inside new namespaces)              */
/* ------------------------------------------------------------------ */
static char child_stack[MAX_CONTAINERS][STACK_SIZE];
static int  child_slot = 0;
 
static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
 
    /* Redirect stdout + stderr through pipe to supervisor */
    dup2(cfg->pipe_wr, STDOUT_FILENO);
    dup2(cfg->err_wr,  STDERR_FILENO);
    close(cfg->pipe_wr);
    close(cfg->err_wr);
 
    /* Mount /proc inside new mount namespace */
    mount("proc", "/proc", "proc", 0, NULL);
 
    /* chroot into container rootfs */
    if (chroot(cfg->rootfs) < 0) {
        fprintf(stderr, "chroot(%s): %s\n", cfg->rootfs, strerror(errno));
        return 1;
    }
    if (chdir("/") < 0) {
        fprintf(stderr, "chdir(/): %s\n", strerror(errno));
        return 1;
    }
 
    /* Apply nice value */
    if (cfg->nice_value != 0) {
        errno = 0;
        int r = nice(cfg->nice_value);
        (void)r;
    }
 
    /* Execute the command via /bin/sh */
    char *argv[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", argv);
    fprintf(stderr, "execv failed: %s\n", strerror(errno));
    return 127;
}
 
/* ------------------------------------------------------------------ */
/* Kernel monitor registration                                          */
/* ------------------------------------------------------------------ */
static int register_with_monitor(int monitor_fd, const char *cid,
                                 pid_t pid, unsigned long soft, unsigned long hard)
{
    struct monitor_request req;
    if (monitor_fd < 0) return 0;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    snprintf(req.container_id, MONITOR_NAME_LEN, "%s", cid);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}
 
static int unregister_from_monitor(int monitor_fd, const char *cid, pid_t pid)
{
    struct monitor_request req;
    if (monitor_fd < 0) return 0;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    snprintf(req.container_id, MONITOR_NAME_LEN, "%s", cid);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}
 
/* ------------------------------------------------------------------ */
/* Container metadata helpers                                           */
/* ------------------------------------------------------------------ */
static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c;
    for (c = ctx->containers; c; c = c->next)
        if (strcmp(c->id, id) == 0)
            return c;
    return NULL;
}
 
/* ------------------------------------------------------------------ */
/* SIGCHLD handler — reaps children, updates metadata                  */
/* ------------------------------------------------------------------ */
static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;
 
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;
 
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c;
        for (c = g_ctx->containers; c; c = c->next) {
            if (c->host_pid != pid) continue;
            if (WIFEXITED(status)) {
                c->exit_code  = WEXITSTATUS(status);
                c->exit_signal = 0;
                c->state = CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                if (c->stop_requested)
                    c->state = CONTAINER_STOPPED;
                else if (c->exit_signal == SIGKILL)
                    c->state = CONTAINER_HARD_LIMIT_KILLED;
                else
                    c->state = CONTAINER_KILLED;
            }
            unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
            fprintf(stderr, "[supervisor] container '%s' pid=%d exited: %s\n",
                    c->id, pid, state_to_string(c->state));
            break;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}
 
/* ------------------------------------------------------------------ */
/* Launch a container                                                   */
/* ------------------------------------------------------------------ */
static int launch_container(supervisor_ctx_t *ctx,
                            const char *id, const char *rootfs,
                            const char *command,
                            unsigned long soft, unsigned long hard,
                            int nice_val)
{
    /* Create pipe: parent reads [0], child writes [1] */
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;
 
    int slot = __sync_fetch_and_add(&child_slot, 1) % MAX_CONTAINERS;
 
    child_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return -1; }
    snprintf(cfg->id,     CONTAINER_ID_LEN, "%s", id);
    snprintf(cfg->rootfs, PATH_MAX, "%s", rootfs);
    snprintf(cfg->command, CHILD_COMMAND_LEN, "%s", command);
    cfg->nice_value = nice_val;
    cfg->pipe_wr    = pipefd[1];
    cfg->err_wr     = pipefd[1]; /* merge stderr into same pipe */
 
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, child_stack[slot] + STACK_SIZE, flags, cfg);
    free(cfg);
    close(pipefd[1]); /* parent closes write end */
 
    if (pid < 0) { close(pipefd[0]); return -1; }
 
    /* Add metadata record */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) { close(pipefd[0]); return -1; }
    snprintf(rec->id, CONTAINER_ID_LEN, "%s", id);
    snprintf(rec->log_path, PATH_MAX, "%s", LOG_DIR);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = soft;
    rec->hard_limit_bytes  = hard;
    rec->pipe_rd           = pipefd[0];
 
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);
 
    /* Register with kernel monitor */
    register_with_monitor(ctx->monitor_fd, id, pid, soft, hard);
 
    /* Spawn pipe-reader (producer) thread for this container */
    reader_args_t *ra = malloc(sizeof(*ra));
    if (ra) {
        ra->ctx    = ctx;
        ra->pipe_fd = pipefd[0];
        snprintf(ra->container_id, CONTAINER_ID_LEN, "%s", id);
        pthread_t tid;
        pthread_create(&tid, NULL, pipe_reader_thread, ra);
        pthread_detach(tid);
    }
 
    fprintf(stderr, "[supervisor] started container '%s' pid=%d\n", id, pid);
    return 0;
}
 
/* ------------------------------------------------------------------ */
/* Command dispatch (supervisor side)                                   */
/* ------------------------------------------------------------------ */
static void handle_request(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           int client_fd)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
 
    switch (req->kind) {
 
    case CMD_START:
    case CMD_RUN: {
        /* Ensure log directory exists */
        mkdir(LOG_DIR, 0755);
 
        pthread_mutex_lock(&ctx->metadata_lock);
        int exists = (find_container(ctx, req->container_id) != NULL);
        pthread_mutex_unlock(&ctx->metadata_lock);
 
        if (exists) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "ERROR: container '%s' already exists", req->container_id);
            ssize_t w = write(client_fd, &resp, sizeof(resp)); (void)w;
            return;
        }
 
        if (launch_container(ctx, req->container_id, req->rootfs,
                             req->command, req->soft_limit_bytes,
                             req->hard_limit_bytes, req->nice_value) < 0) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "ERROR: launch failed: %s", strerror(errno));
            ssize_t w = write(client_fd, &resp, sizeof(resp)); (void)w;
            return;
        }
 
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req->container_id);
        pid_t pid = c ? c->host_pid : -1;
        pthread_mutex_unlock(&ctx->metadata_lock);
 
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "OK: container '%s' started pid=%d", req->container_id, pid);
        ssize_t w = write(client_fd, &resp, sizeof(resp)); (void)w;
 
        if (req->kind == CMD_RUN) {
            /* Block until container exits */
            while (1) {
                usleep(200000);
                pthread_mutex_lock(&ctx->metadata_lock);
                c = find_container(ctx, req->container_id);
                container_state_t st = c ? c->state : CONTAINER_EXITED;
                pthread_mutex_unlock(&ctx->metadata_lock);
                if (st != CONTAINER_RUNNING && st != CONTAINER_STARTING) break;
            }
            /* Send final status */
            pthread_mutex_lock(&ctx->metadata_lock);
            c = find_container(ctx, req->container_id);
            memset(&resp, 0, sizeof(resp));
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "EXITED: state=%s", c ? state_to_string(c->state) : "unknown");
            pthread_mutex_unlock(&ctx->metadata_lock);
            w = write(client_fd, &resp, sizeof(resp)); (void)w;
        }
        break;
    }
 
    case CMD_PS: {
        char line[256];
        snprintf(line, sizeof(line),
                 "%-20s %-8s %-22s %-10s %-10s\n",
                 "ID", "PID", "STATE", "SOFT_MIB", "HARD_MIB");
        ssize_t w = write(client_fd, line, strlen(line)); (void)w;
 
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c;
        for (c = ctx->containers; c; c = c->next) {
            snprintf(line, sizeof(line),
                     "%-20s %-8d %-22s %-10lu %-10lu\n",
                     c->id, c->host_pid, state_to_string(c->state),
                     c->soft_limit_bytes >> 20,
                     c->hard_limit_bytes >> 20);
            w = write(client_fd, line, strlen(line)); (void)w;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        break;
    }
 
    case CMD_LOGS: {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);
        FILE *f = fopen(path, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "ERROR: no log for '%s'", req->container_id);
            ssize_t w = write(client_fd, &resp, sizeof(resp)); (void)w;
            return;
        }
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            ssize_t w = write(client_fd, buf, n); (void)w;
        }
        fclose(f);
        break;
    }
 
    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req->container_id);
        if (!c || c->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "ERROR: container '%s' not running", req->container_id);
            ssize_t w = write(client_fd, &resp, sizeof(resp)); (void)w;
            return;
        }
        c->stop_requested = 1;
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);
 
        kill(pid, SIGTERM);
        usleep(500000);
 
        pthread_mutex_lock(&ctx->metadata_lock);
        c = find_container(ctx, req->container_id);
        if (c && c->state == CONTAINER_RUNNING)
            kill(pid, SIGKILL);
        pthread_mutex_unlock(&ctx->metadata_lock);
 
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "OK: stop signal sent to '%s'", req->container_id);
        ssize_t w = write(client_fd, &resp, sizeof(resp)); (void)w;
        break;
    }
 
    default:
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "ERROR: unknown command");
        ssize_t w = write(client_fd, &resp, sizeof(resp)); (void)w;
        break;
    }
}
 
/* ------------------------------------------------------------------ */
/* Supervisor main loop                                                  */
/* ------------------------------------------------------------------ */
static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}
 
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
 
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;
 
    /* 1) Init metadata lock and bounded buffer */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc) { errno = rc; perror("pthread_mutex_init"); return 1; }
 
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc) { errno = rc; perror("bounded_buffer_init"); return 1; }
 
    /* 2) Open kernel monitor device */
    mkdir(LOG_DIR, 0755);
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] warning: /dev/container_monitor not available\n");
    else
        fprintf(stderr, "[supervisor] connected to kernel monitor\n");
 
    /* 3) Create UNIX domain socket (Path B) */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }
 
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) { perror("listen"); return 1; }
    chmod(CONTROL_PATH, 0777);
 
    /* 4) Install signal handlers */
    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);
 
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sa_term.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);
 
    /* 5) Start logging consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc) { errno = rc; perror("pthread_create"); return 1; }
 
    fprintf(stderr, "[supervisor] ready (base-rootfs=%s socket=%s)\n",
            rootfs, CONTROL_PATH);
 
    /* 6) Event loop */
    while (!ctx.should_stop) {
        fd_set rfds;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        int r = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
 
        int client = accept(ctx.server_fd, NULL, NULL);
        if (client < 0) continue;
 
        control_request_t req;
        if (read(client, &req, sizeof(req)) == (ssize_t)sizeof(req))
            handle_request(&ctx, &req, client);
        close(client);
    }
 
    /* 7) Graceful shutdown */
    fprintf(stderr, "[supervisor] shutting down...\n");
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c;
    for (c = ctx.containers; c; c = c->next) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    sleep(1);
 
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
 
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
 
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
 
    /* Free metadata list */
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
 
    fprintf(stderr, "[supervisor] exited cleanly\n");
    return 0;
}
 
/* ------------------------------------------------------------------ */
/* CLI client — sends request over UNIX socket, prints response        */
/* ------------------------------------------------------------------ */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
 
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
 
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                        "Is the supervisor running?\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }
 
    ssize_t w = write(fd, req, sizeof(*req)); (void)w;
 
    /* Read response (may be binary control_response_t or raw text) */
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, stdout);
 
    close(fd);
    return 0;
}
 
/* ------------------------------------------------------------------ */
/* CLI command builders                                                  */
/* ------------------------------------------------------------------ */
static int cmd_start(int argc, char *argv[], int is_run)
{
    control_request_t req;
    if (argc < 5) { usage(argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind             = is_run ? CMD_RUN : CMD_START;
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    snprintf(req.container_id, CONTAINER_ID_LEN, "%s", argv[2]);
    snprintf(req.rootfs, PATH_MAX, "%s", argv[3]);
    snprintf(req.command, CHILD_COMMAND_LEN, "%s", argv[4]);
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}
 
static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}
 
static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) { usage(argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    snprintf(req.container_id, CONTAINER_ID_LEN, "%s", argv[2]);
    return send_control_request(&req);
}
 
static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) { usage(argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    snprintf(req.container_id, CONTAINER_ID_LEN, "%s", argv[2]);
    return send_control_request(&req);
}
 
/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
 
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]); return 1; }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start")    == 0) return cmd_start(argc, argv, 0);
    if (strcmp(argv[1], "run")      == 0) return cmd_start(argc, argv, 1);
    if (strcmp(argv[1], "ps")       == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")     == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")     == 0) return cmd_stop(argc, argv);
 
    usage(argv[0]);
    return 1;
}
