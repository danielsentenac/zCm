#include "zcm/zcm.h"
#include "zcm/zcm_node.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
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

static int start_daemon_argv(const char *const argv[], const char *log_path, pid_t *out_pid) {
  if (!argv || !argv[0] || !out_pid) return -1;

  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int out_fd = -1;
    if (log_path && *log_path) out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) out_fd = open("/dev/null", O_WRONLY);
    if (out_fd >= 0) {
      dup2(out_fd, STDOUT_FILENO);
      dup2(out_fd, STDERR_FILENO);
      close(out_fd);
    }
    execv(argv[0], (char *const *)argv);
    _exit(127);
  }

  *out_pid = pid;
  return 0;
}

static void stop_pid(pid_t *pid) {
  if (!pid || *pid <= 0) return;
  pid_t p = *pid;

  kill(p, SIGTERM);
  for (int i = 0; i < 20; i++) {
    int st = 0;
    pid_t r = waitpid(p, &st, WNOHANG);
    if (r == p) {
      *pid = -1;
      return;
    }
    usleep(100 * 1000);
  }

  kill(p, SIGKILL);
  (void)waitpid(p, NULL, 0);
  *pid = -1;
}

static int compute_build_dir(char *out, size_t out_size) {
  if (!out || out_size == 0) return -1;
  char exe[1024];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0 || n >= (ssize_t)sizeof(exe)) return -1;
  exe[n] = '\0';

  char *p = strrchr(exe, '/');
  if (!p) return -1;
  *p = '\0';
  p = strrchr(exe, '/');
  if (!p) return -1;
  *p = '\0';

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

static int write_text_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  if (fputs(text, f) == EOF) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int read_domain_entry(const char *path, char *out_domain, size_t out_domain_size,
                             char *out_host, size_t out_host_size, int *out_port) {
  FILE *f = NULL;
  char line[1024] = {0};
  char domain[256] = {0};
  char host[256] = {0};
  int port = 0;

  if (!path || !out_domain || out_domain_size == 0 ||
      !out_host || out_host_size == 0 || !out_port) {
    return -1;
  }

  f = fopen(path, "r");
  if (!f) return -1;
  if (!fgets(line, sizeof(line), f)) {
    fclose(f);
    return -1;
  }
  fclose(f);

  if (sscanf(line, "%255s %255s %d", domain, host, &port) != 3) return -1;
  if (snprintf(out_domain, out_domain_size, "%s", domain) >= (int)out_domain_size) return -1;
  if (snprintf(out_host, out_host_size, "%s", host) >= (int)out_host_size) return -1;
  *out_port = port;
  return 0;
}

static int ping_endpoint(const char *endpoint) {
  int ok = 0;
  zcm_context_t *ctx = NULL;
  zcm_socket_t *req = NULL;
  char reply[64] = {0};
  size_t reply_len = 0;

  if (!endpoint || !*endpoint) return 0;

  ctx = zcm_context_new();
  if (!ctx) return 0;
  req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) goto done;

  zcm_socket_set_timeouts(req, 500);
  if (zcm_socket_connect(req, endpoint) != 0) goto done;
  if (zcm_socket_send_bytes(req, "PING", 4) != 0) goto done;
  if (zcm_socket_recv_bytes(req, reply, sizeof(reply) - 1, &reply_len) != 0) goto done;
  reply[reply_len] = '\0';
  ok = (strcmp(reply, "PONG") == 0);

done:
  if (req) zcm_socket_free(req);
  if (ctx) zcm_context_free(ctx);
  return ok;
}

static int wait_for_active_broker(const char *db_path, const char *expected_domain,
                                  const char *initial_host, char *out_host,
                                  size_t out_host_size, int *out_port, int timeout_ms) {
  int elapsed = 0;

  if (!db_path || !expected_domain || !initial_host ||
      !out_host || out_host_size == 0 || !out_port) {
    return -1;
  }

  while (elapsed <= timeout_ms) {
    char domain[256] = {0};
    char host[256] = {0};
    int port = 0;

    if (read_domain_entry(db_path, domain, sizeof(domain), host, sizeof(host), &port) == 0) {
      char endpoint[512] = {0};
      if (strcmp(domain, expected_domain) == 0 &&
          strcmp(host, initial_host) != 0 &&
          snprintf(endpoint, sizeof(endpoint), "tcp://%s:%d", host, port) < (int)sizeof(endpoint) &&
          ping_endpoint(endpoint)) {
        snprintf(out_host, out_host_size, "%s", host);
        *out_port = port;
        return 0;
      }
    }

    usleep(100 * 1000);
    elapsed += 100;
  }

  return -1;
}

static int assert_broker_listed(const char *broker_endpoint) {
  for (int attempt = 0; attempt < 20; attempt++) {
    int found = 0;
    zcm_context_t *ctx = NULL;
    zcm_node_t *node = NULL;
    zcm_node_entry_t *entries = NULL;
    size_t count = 0;

    ctx = zcm_context_new();
    if (!ctx) return -1;
    node = zcm_node_new(ctx, broker_endpoint);
    if (node && zcm_node_list(node, &entries, &count) == 0) {
      for (size_t i = 0; i < count; i++) {
        if (entries[i].name && strcmp(entries[i].name, "zcmbroker") == 0) {
          found = 1;
          break;
        }
      }
    }

    if (entries) zcm_node_list_free(entries, count);
    if (node) zcm_node_free(node);
    if (ctx) zcm_context_free(ctx);

    if (found) return 0;
    usleep(100 * 1000);
  }

  return -1;
}

int main(void) {
  int rc = 1;
  pid_t broker_pid = -1;
  cmd_result_t second = {-1, NULL};
  char tmp_dir[] = "/tmp/zcm-broker-tool-XXXXXX";
  char db_path[512] = {0};
  char log_path[512] = {0};
  char build_dir[1024] = {0};
  char broker_path[1200] = {0};
  char active_host[256] = {0};
  char domain[256] = {0};
  char file_host[256] = {0};
  char broker_endpoint[512] = {0};
  const char *test_domain = "tool-singleton";
  const char *stale_host = "127.0.0.2";
  int file_port = 0;

  if (!mkdtemp(tmp_dir)) {
    perror("mkdtemp");
    return 1;
  }

  int port = pick_free_tcp_port();
  if (port <= 0) {
    printf("zcm_broker_tool_singleton: SKIP (no local TCP port allocation available)\n");
    rc = 0;
    goto done;
  }

  snprintf(db_path, sizeof(db_path), "%s/ZCmDomains", tmp_dir);
  snprintf(log_path, sizeof(log_path), "%s/zcm_broker.log", tmp_dir);
  {
    FILE *f = fopen(db_path, "w");
    if (!f) {
      perror("fopen");
      goto done;
    }
    fprintf(f, "%s %s %d 61234 64 repo\n", test_domain, stale_host, port);
    fclose(f);
  }

  if (compute_build_dir(build_dir, sizeof(build_dir)) != 0) {
    fprintf(stderr, "zcm_broker_tool_singleton: cannot determine build dir\n");
    goto done;
  }
  snprintf(broker_path, sizeof(broker_path), "%s/tools/zcm_broker", build_dir);
  if (access(broker_path, X_OK) != 0) {
    printf("zcm_broker_tool_singleton: SKIP (missing zcm_broker executable)\n");
    rc = 0;
    goto done;
  }

  (void)unsetenv("ZCMBROKER");
  (void)unsetenv("ZCMBROKER_ENDPOINT");
  (void)unsetenv("ZCMMGR");
  (void)setenv("ZCMDOMAIN", test_domain, 1);
  (void)setenv("ZCMDOMAIN_DATABASE", tmp_dir, 1);

  {
    const char *const broker_argv[] = {broker_path, NULL};
    if (start_daemon_argv(broker_argv, log_path, &broker_pid) != 0) {
      fprintf(stderr, "zcm_broker_tool_singleton: failed to start broker tool\n");
      goto done;
    }

    if (wait_for_active_broker(db_path, test_domain, stale_host,
                               active_host, sizeof(active_host), &file_port, 5000) != 0) {
      fprintf(stderr, "zcm_broker_tool_singleton: broker did not become active\n");
      goto done;
    }

    snprintf(broker_endpoint, sizeof(broker_endpoint), "tcp://%s:%d", active_host, file_port);
    if (!ping_endpoint(broker_endpoint)) {
      fprintf(stderr, "zcm_broker_tool_singleton: ping failed at %s\n", broker_endpoint);
      goto done;
    }
    if (assert_broker_listed(broker_endpoint) != 0) {
      fprintf(stderr, "zcm_broker_tool_singleton: zcmbroker entry missing at %s\n",
              broker_endpoint);
      goto done;
    }

    if (run_capture_argv(broker_argv, 4000, &second) != 0) {
      fprintf(stderr, "zcm_broker_tool_singleton: second invocation failed to run\n");
      goto done;
    }
  }

  if (second.exit_code != 0) {
    fprintf(stderr, "zcm_broker_tool_singleton: second invocation exit=%d\n", second.exit_code);
    goto done;
  }
  if (!second.output || !strstr(second.output, "already running")) {
    fprintf(stderr, "zcm_broker_tool_singleton: unexpected second invocation output: %s\n",
            second.output ? second.output : "(null)");
    goto done;
  }
  if (read_domain_entry(db_path, domain, sizeof(domain), file_host, sizeof(file_host), &file_port) != 0) {
    fprintf(stderr, "zcm_broker_tool_singleton: failed to reread ZCmDomains\n");
    goto done;
  }
  if (strcmp(domain, test_domain) != 0 || strcmp(file_host, active_host) != 0 || file_port != port) {
    fprintf(stderr, "zcm_broker_tool_singleton: unexpected domain file after second start\n");
    goto done;
  }
  if (!ping_endpoint(broker_endpoint)) {
    fprintf(stderr, "zcm_broker_tool_singleton: broker not reachable after second start\n");
    goto done;
  }

  printf("zcm_broker_tool_singleton: PASS\n");
  rc = 0;

done:
  free(second.output);
  stop_pid(&broker_pid);
  (void)unsetenv("ZCMBROKER");
  (void)unsetenv("ZCMBROKER_ENDPOINT");
  (void)unsetenv("ZCMDOMAIN");
  (void)unsetenv("ZCMDOMAIN_DATABASE");
  (void)unsetenv("ZCMMGR");
  if (db_path[0]) unlink(db_path);
  if (log_path[0]) unlink(log_path);
  rmdir(tmp_dir);
  return rc;
}
