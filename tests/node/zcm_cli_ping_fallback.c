#include "zcm/zcm.h"
#include "zcm/zcm_node.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct cmd_result {
  int exit_code;
  char *output;
} cmd_result_t;

typedef struct ping_server {
  zcm_context_t *ctx;
  char endpoint[256];
  volatile int ready;
  volatile int done;
  pthread_t tid;
} ping_server_t;

static int append_buf(char **dst, size_t *len, size_t *cap, const char *src, size_t n) {
  if (!dst || !len || !cap || (!src && n > 0)) return -1;
  size_t need = *len + n + 1;
  if (need > *cap) {
    size_t new_cap = (*cap == 0) ? 1024 : *cap;
    while (new_cap < need) new_cap *= 2;
    char *p = (char *)realloc(*dst, new_cap);
    if (!p) return -1;
    *dst = p;
    *cap = new_cap;
  }
  if (n > 0) memcpy(*dst + *len, src, n);
  *len += n;
  (*dst)[*len] = '\0';
  return 0;
}

static int run_capture_argv(const char *const argv[], int timeout_ms, cmd_result_t *out) {
  if (!argv || !argv[0] || !out) return -1;
  out->exit_code = -1;
  out->output = NULL;

  int pipefd[2];
  if (pipe(pipefd) != 0) return -1;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    execv(argv[0], (char *const *)argv);
    _exit(127);
  }

  close(pipefd[1]);
  int flags = fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

  char *buf = NULL;
  size_t len = 0;
  size_t cap = 0;
  int elapsed = 0;
  int status = 0;
  int child_done = 0;

  while (1) {
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;

    int wait_ms = 100;
    if (!child_done && timeout_ms > 0 && elapsed >= timeout_ms) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      child_done = 1;
    }

    int prc = poll(&pfd, 1, wait_ms);
    if (prc > 0 && (pfd.revents & POLLIN)) {
      char tmp[1024];
      ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
      if (n > 0) {
        if (append_buf(&buf, &len, &cap, tmp, (size_t)n) != 0) {
          close(pipefd[0]);
          free(buf);
          return -1;
        }
      }
    }

    if (!child_done) {
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) child_done = 1;
    }

    if (child_done) {
      char tmp[1024];
      while (1) {
        ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
        if (n > 0) {
          if (append_buf(&buf, &len, &cap, tmp, (size_t)n) != 0) {
            close(pipefd[0]);
            free(buf);
            return -1;
          }
        } else {
          break;
        }
      }
      break;
    }

    elapsed += wait_ms;
  }

  close(pipefd[0]);
  if (!buf) {
    buf = (char *)calloc(1, 1);
    if (!buf) return -1;
  }

  out->output = buf;
  if (WIFEXITED(status)) out->exit_code = WEXITSTATUS(status);
  else out->exit_code = 255;
  return 0;
}

static int compute_build_dir(char *out, size_t out_size) {
  if (!out || out_size == 0) return -1;
  char exe[1024];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0 || n >= (ssize_t)sizeof(exe)) return -1;
  exe[n] = '\0';

  char *p = strrchr(exe, '/');
  if (!p) return -1;
  *p = '\0'; /* .../build/tests */
  p = strrchr(exe, '/');
  if (!p) return -1;
  *p = '\0'; /* .../build */

  if (snprintf(out, out_size, "%s", exe) >= (int)out_size) return -1;
  return 0;
}

static int pick_free_tcp_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  socklen_t len = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
    close(fd);
    return -1;
  }

  int port = (int)ntohs(addr.sin_port);
  close(fd);
  return port;
}

static int pick_distinct_ports(int *broker_port, int *data_port) {
  if (!broker_port || !data_port) return -1;
  for (int i = 0; i < 64; i++) {
    int b = pick_free_tcp_port();
    int d = pick_free_tcp_port();
    if (b <= 0 || d <= 0) continue;
    if (b == d) continue;
    *broker_port = b;
    *data_port = d;
    return 0;
  }
  return -1;
}

static void *legacy_ping_server_main(void *arg) {
  ping_server_t *srv = (ping_server_t *)arg;
  zcm_socket_t *rep = zcm_socket_new(srv->ctx, ZCM_SOCK_REP);
  if (!rep) {
    srv->ready = -1;
    return NULL;
  }
  zcm_socket_set_timeouts(rep, 200);
  if (zcm_socket_bind(rep, srv->endpoint) != 0) {
    srv->ready = -1;
    zcm_socket_free(rep);
    return NULL;
  }
  srv->ready = 1;

  while (!srv->done) {
    char buf[2048] = {0};
    size_t n = 0;
    if (zcm_socket_recv_bytes(rep, buf, sizeof(buf), &n) != 0 || n == 0) {
      continue;
    }
    if (n == 4 && memcmp(buf, "PING", 4) == 0) {
      (void)zcm_socket_send_bytes(rep, "PONG", 4);
      srv->done = 1;
    } else {
      (void)zcm_socket_send_bytes(rep, "ERR", 3);
    }
  }

  zcm_socket_free(rep);
  return NULL;
}

int main(void) {
  int rc = 1;
  zcm_context_t *ctx = NULL;
  zcm_broker_t *broker = NULL;
  zcm_node_t *node = NULL;
  ping_server_t server;
  memset(&server, 0, sizeof(server));

  char build_dir[1024] = {0};
  char zcm_path[1200] = {0};
  char broker_ep[256] = {0};
  char data_ep[256] = {0};

  if (compute_build_dir(build_dir, sizeof(build_dir)) != 0) {
    fprintf(stderr, "zcm_cli_ping_fallback: cannot determine build dir\n");
    return 1;
  }
  snprintf(zcm_path, sizeof(zcm_path), "%s/tools/zcm", build_dir);

  int broker_port = 0;
  int data_port = 0;
  if (pick_distinct_ports(&broker_port, &data_port) != 0) {
    printf("zcm_cli_ping_fallback: SKIP (no local TCP port allocation available)\n");
    return 0;
  }
  snprintf(broker_ep, sizeof(broker_ep), "tcp://127.0.0.1:%d", broker_port);
  snprintf(data_ep, sizeof(data_ep), "tcp://127.0.0.1:%d", data_port);

  ctx = zcm_context_new();
  if (!ctx) {
    fprintf(stderr, "zcm_cli_ping_fallback: context init failed\n");
    goto done;
  }
  broker = zcm_broker_start(ctx, broker_ep);
  if (!broker) {
    fprintf(stderr, "zcm_cli_ping_fallback: broker start failed\n");
    goto done;
  }
  node = zcm_node_new(ctx, broker_ep);
  if (!node) {
    fprintf(stderr, "zcm_cli_ping_fallback: node init failed\n");
    goto done;
  }
  if (zcm_node_register(node, "legacy", data_ep) != 0) {
    fprintf(stderr, "zcm_cli_ping_fallback: legacy register failed\n");
    goto done;
  }

  server.ctx = ctx;
  snprintf(server.endpoint, sizeof(server.endpoint), "%s", data_ep);
  server.ready = 0;
  server.done = 0;
  if (pthread_create(&server.tid, NULL, legacy_ping_server_main, &server) != 0) {
    fprintf(stderr, "zcm_cli_ping_fallback: server thread start failed\n");
    goto done;
  }

  int waited_ms = 0;
  while (server.ready == 0 && waited_ms < 3000) {
    usleep(100 * 1000);
    waited_ms += 100;
  }
  if (server.ready <= 0) {
    fprintf(stderr, "zcm_cli_ping_fallback: server thread not ready\n");
    goto done;
  }

  setenv("ZCMBROKER", broker_ep, 1);
  unsetenv("ZCMDOMAIN");
  unsetenv("ZCMDOMAIN_DATABASE");
  unsetenv("ZCMMGR");

  {
    const char *argv[] = {zcm_path, "ping", "legacy", NULL};
    cmd_result_t r;
    if (run_capture_argv(argv, 8000, &r) != 0) {
      fprintf(stderr, "zcm_cli_ping_fallback: run zcm ping failed\n");
      goto done;
    }
    if (r.exit_code != 0 || strstr(r.output, "PONG legacy") == NULL) {
      fprintf(stderr, "zcm_cli_ping_fallback: unexpected ping output (code=%d):\n%s\n",
              r.exit_code, r.output ? r.output : "(null)");
      free(r.output);
      goto done;
    }
    free(r.output);
  }

  rc = 0;
  printf("zcm_cli_ping_fallback: PASS\n");

done:
  server.done = 1;
  if (server.ready != 0) pthread_join(server.tid, NULL);
  if (node) zcm_node_free(node);
  if (broker) zcm_broker_stop(broker);
  if (ctx) zcm_context_free(ctx);
  return rc;
}
