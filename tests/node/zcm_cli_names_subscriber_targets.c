#include "zcm/zcm.h"
#include "zcm/zcm_node.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
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

static int parse_row_for_name(const char *table, const char *name,
                              char *out_endpoint, size_t out_endpoint_size,
                              char *out_role, size_t out_role_size,
                              char *out_sub_bytes, size_t out_sub_bytes_size) {
  if (!table || !name || !*name || !out_endpoint || out_endpoint_size == 0 ||
      !out_role || out_role_size == 0 ||
      !out_sub_bytes || out_sub_bytes_size == 0) {
    return -1;
  }

  out_endpoint[0] = '\0';
  out_role[0] = '\0';
  out_sub_bytes[0] = '\0';

  char *copy = strdup(table);
  if (!copy) return -1;

  int rc = -1;
  char *saveptr = NULL;
  for (char *line = strtok_r(copy, "\n", &saveptr);
       line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    if (!*line) continue;
    if (strncmp(line, "NAME", 4) == 0) continue;
    if (line[0] == '-') continue;

    char row_name[128] = {0};
    char endpoint[512] = {0};
    char role[512] = {0};
    char pub_port[32] = {0};
    char push_port[32] = {0};
    char pub_bytes[32] = {0};
    char sub_bytes[32] = {0};
    char push_bytes[32] = {0};
    char pull_bytes[32] = {0};
    int fields = sscanf(line, "%127s %511s %511s %31s %31s %31s %31s %31s %31s",
                        row_name, endpoint, role,
                        pub_port, push_port, pub_bytes, sub_bytes, push_bytes, pull_bytes);
    if (fields < 3) continue;
    if (strcmp(row_name, name) != 0) continue;

    snprintf(out_endpoint, out_endpoint_size, "%s", endpoint);
    snprintf(out_role, out_role_size, "%s", role);
    if (fields >= 7) snprintf(out_sub_bytes, out_sub_bytes_size, "%s", sub_bytes);
    else snprintf(out_sub_bytes, out_sub_bytes_size, "-");
    rc = 0;
    break;
  }

  free(copy);
  return rc;
}

int main(void) {
  int rc = 1;
  zcm_context_t *ctx = NULL;
  zcm_broker_t *broker = NULL;
  zcm_node_t *node = NULL;

  char build_dir[1024] = {0};
  char zcm_path[1200] = {0};
  char broker_ep[256] = {0};
  char pub_ep[256] = {0};
  char sub_ep[256] = {0};
  char ioget_ctrl_ep[256] = {0};

  if (compute_build_dir(build_dir, sizeof(build_dir)) != 0) {
    fprintf(stderr, "zcm_cli_names_subscriber_targets: cannot determine build dir\n");
    return 1;
  }
  snprintf(zcm_path, sizeof(zcm_path), "%s/tools/zcm", build_dir);

  int broker_port = pick_free_tcp_port();
  int pub_port = pick_free_tcp_port();
  if (broker_port <= 0 || pub_port <= 0 || broker_port == pub_port) {
    printf("zcm_cli_names_subscriber_targets: SKIP (no local TCP port allocation available)\n");
    return 0;
  }

  snprintf(broker_ep, sizeof(broker_ep), "tcp://127.0.0.1:%d", broker_port);
  snprintf(pub_ep, sizeof(pub_ep), "tcp://127.0.0.1:%d", pub_port);
  snprintf(sub_ep, sizeof(sub_ep), "sub://127.0.0.1:%d", pub_port);

  ctx = zcm_context_new();
  if (!ctx) {
    fprintf(stderr, "zcm_cli_names_subscriber_targets: context init failed\n");
    goto done;
  }
  broker = zcm_broker_start(ctx, broker_ep);
  if (!broker) {
    fprintf(stderr, "zcm_cli_names_subscriber_targets: broker start failed\n");
    goto done;
  }
  node = zcm_node_new(ctx, broker_ep);
  if (!node) {
    fprintf(stderr, "zcm_cli_names_subscriber_targets: node init failed\n");
    goto done;
  }

  {
    char vac_ctrl[256] = {0};
    char ioget_ctrl[256] = {0};
    snprintf(vac_ctrl, sizeof(vac_ctrl), "tcp://127.0.0.1:%d", pub_port + 1000);
    snprintf(ioget_ctrl, sizeof(ioget_ctrl), "tcp://127.0.0.1:%d", pub_port + 1001);
    snprintf(ioget_ctrl_ep, sizeof(ioget_ctrl_ep), "%s", ioget_ctrl);
    if (zcm_node_register_ex(node, "zFdVac", pub_ep, vac_ctrl, "127.0.0.1", (int)getpid(),
                             "PUB", pub_port, -1) != 0 ||
        zcm_node_register_ex(node, "zFdIOGet", sub_ep, ioget_ctrl, "127.0.0.1", (int)getpid(),
                             "SUB", -1, -1) != 0) {
      fprintf(stderr, "zcm_cli_names_subscriber_targets: register failed\n");
      goto done;
    }
  }
  if (zcm_node_report_metrics(node, "zFdVac", "PUB",
                              pub_port, -1,
                              321, -1,
                              -1, -1) != 0) {
    fprintf(stderr, "zcm_cli_names_subscriber_targets: report metrics failed\n");
    goto done;
  }

  setenv("ZCMBROKER", broker_ep, 1);
  unsetenv("ZCMDOMAIN");
  unsetenv("ZCMDOMAIN_DATABASE");
  unsetenv("ZCMMGR");
  setenv("ZCM_NAMES_QUERY_TIMEOUT_MS", "250", 1);
  setenv("ZCM_NAMES_QUERY_ATTEMPTS", "1", 1);

  {
    const char *argv[] = {zcm_path, "names", NULL};
    cmd_result_t r;
    if (run_capture_argv(argv, 15000, &r) != 0) {
      fprintf(stderr, "zcm_cli_names_subscriber_targets: run zcm names failed\n");
      goto done;
    }

    if (r.exit_code != 0) {
      fprintf(stderr, "zcm_cli_names_subscriber_targets: zcm names failed (code=%d):\n%s\n",
              r.exit_code, r.output ? r.output : "(null)");
      free(r.output);
      goto done;
    }

    char endpoint[512] = {0};
    char role[512] = {0};
    char sub_bytes[64] = {0};
    if (parse_row_for_name(r.output, "zFdIOGet",
                           endpoint, sizeof(endpoint),
                           role, sizeof(role),
                           sub_bytes, sizeof(sub_bytes)) != 0) {
      fprintf(stderr, "zcm_cli_names_subscriber_targets: missing zFdIOGet row:\n%s\n", r.output);
      free(r.output);
      goto done;
    }

    if (strcmp(endpoint, pub_ep) != 0 &&
        strcmp(endpoint, ioget_ctrl_ep) != 0) {
      fprintf(stderr, "zcm_cli_names_subscriber_targets: expected endpoint %s or %s got %s\n",
              pub_ep, ioget_ctrl_ep, endpoint);
      free(r.output);
      goto done;
    }
    char expected_role[128] = {0};
    snprintf(expected_role, sizeof(expected_role), "SUB:zFdVac:%d", pub_port);
    if (strcmp(role, expected_role) != 0) {
      fprintf(stderr, "zcm_cli_names_subscriber_targets: expected role %s got %s\n",
              expected_role,
              role);
      free(r.output);
      goto done;
    }
    if (strcmp(sub_bytes, "321") != 0 &&
        strcmp(sub_bytes, "-") != 0) {
      fprintf(stderr, "zcm_cli_names_subscriber_targets: expected SUB_BYTES=321 or - got %s\n",
              sub_bytes);
      free(r.output);
      goto done;
    }

    free(r.output);
  }

  rc = 0;
  printf("zcm_cli_names_subscriber_targets: PASS\n");

done:
  if (node) zcm_node_free(node);
  if (broker) zcm_broker_stop(broker);
  if (ctx) zcm_context_free(ctx);
  return rc;
}
