#include "zcm/zcm_proc.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ZCM_PROC_CONFIG_SCHEMA_DEFAULT
#define ZCM_PROC_CONFIG_SCHEMA_DEFAULT "config/schema/proc-config.xsd"
#endif

#ifndef ZCM_PROC_REANNOUNCE_MS_DEFAULT
#define ZCM_PROC_REANNOUNCE_MS_DEFAULT 1000
#endif

#define ZCM_PROC_REANNOUNCE_MS_MIN 100
#define ZCM_PROC_REANNOUNCE_MS_MAX 60000

struct zcm_proc {
  zcm_context_t *ctx;
  zcm_node_t *node;
  zcm_socket_t *ctrl;
  zcm_socket_t *data;
  char *name;
  char *reg_endpoint;
  char *ctrl_reg_endpoint;
  char *host;
  int pid;
  int announce_interval_ms;
  int announce_ok;
  pthread_t ctrl_thread;
  pthread_t announce_thread;
  int stop;
};

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static int parse_positive_int_range(const char *text, int min_val, int max_val, int *out) {
  if (!text || !*text || !out) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < min_val || v > max_val) return -1;
  *out = (int)v;
  return 0;
}

static int proc_register_ex(struct zcm_proc *proc) {
  if (!proc || !proc->node || !proc->name || !proc->reg_endpoint ||
      !proc->ctrl_reg_endpoint || !proc->host) {
    return -1;
  }
  return zcm_node_register_ex(proc->node, proc->name,
                              proc->reg_endpoint,
                              proc->ctrl_reg_endpoint,
                              proc->host, proc->pid);
}

static void *announce_thread_main(void *arg) {
  struct zcm_proc *proc = (struct zcm_proc *)arg;
  uint64_t next_announce_ms = monotonic_ms() + (uint64_t)proc->announce_interval_ms;

  for (;;) {
    if (proc->stop) break;
    uint64_t now_ms = monotonic_ms();
    if (now_ms != 0 && now_ms >= next_announce_ms) {
      if (proc_register_ex(proc) == 0) {
        if (!proc->announce_ok) {
          printf("zcm_proc: broker reachable, re-registered %s\n", proc->name);
          fflush(stdout);
        }
        proc->announce_ok = 1;
      } else {
        if (proc->announce_ok) {
          fprintf(stderr, "zcm_proc: broker unreachable, waiting to re-register %s\n", proc->name);
        }
        proc->announce_ok = 0;
      }
      next_announce_ms = now_ms + (uint64_t)proc->announce_interval_ms;
      continue;
    }
    usleep(50 * 1000);
  }
  return NULL;
}

static void trim_ws_inplace(char *text) {
  if (!text) return;
  char *start = text;
  while (*start && isspace((unsigned char)*start)) start++;
  if (start != text) memmove(text, start, strlen(start) + 1);
  size_t n = strlen(text);
  while (n > 0 && isspace((unsigned char)text[n - 1])) {
    text[--n] = '\0';
  }
}

static int run_xmllint_validate(const char *schema_path, const char *config_path) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      (void)dup2(devnull, STDOUT_FILENO);
      (void)dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("xmllint", "xmllint",
           "--noout", "--schema", schema_path, config_path,
           (char *)NULL);
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
    fprintf(stderr, "zcm_proc: xmllint not found in PATH\n");
  }
  return -1;
}

static int run_xmllint_xpath(const char *config_path, const char *xpath_expr,
                             char *out, size_t out_size) {
  if (!out || out_size == 0) return -1;
  out[0] = '\0';

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
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
    close(pipefd[1]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      (void)dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("xmllint", "xmllint",
           "--xpath", xpath_expr, config_path,
           (char *)NULL);
    _exit(127);
  }

  close(pipefd[1]);
  size_t off = 0;
  while (off + 1 < out_size) {
    ssize_t n = read(pipefd[0], out + off, out_size - 1 - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    off += (size_t)n;
  }
  out[off] = '\0';
  close(pipefd[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
      fprintf(stderr, "zcm_proc: xmllint not found in PATH\n");
    }
    return -1;
  }
  trim_ws_inplace(out);
  return 0;
}

static int load_proc_config(const char *name,
                            zcm_socket_type_t *data_type,
                            int *bind_data,
                            int *ctrl_timeout_ms) {
  if (!name || !data_type || !bind_data || !ctrl_timeout_ms) return -1;

  const char *cfg_file = getenv("ZCM_PROC_CONFIG_FILE");
  char cfg_path[1024];
  if (cfg_file && *cfg_file) {
    if (snprintf(cfg_path, sizeof(cfg_path), "%s", cfg_file) >= (int)sizeof(cfg_path)) {
      fprintf(stderr, "zcm_proc: explicit config path too long\n");
      return -1;
    }
  } else {
    const char *cfg_dir = getenv("ZCM_PROC_CONFIG_DIR");
    if (!cfg_dir || !*cfg_dir) cfg_dir = ".";
    if (snprintf(cfg_path, sizeof(cfg_path), "%s/%s.cfg", cfg_dir, name) >= (int)sizeof(cfg_path)) {
      fprintf(stderr, "zcm_proc: config path too long for '%s'\n", name);
      return -1;
    }
  }
  if (access(cfg_path, R_OK) != 0) {
    fprintf(stderr, "zcm_proc: config file not found: %s\n", cfg_path);
    return -1;
  }

  const char *schema = getenv("ZCM_PROC_CONFIG_SCHEMA");
  if (!schema || !*schema) schema = ZCM_PROC_CONFIG_SCHEMA_DEFAULT;
  if (access(schema, R_OK) != 0) {
    fprintf(stderr, "zcm_proc: config schema not found: %s\n", schema);
    return -1;
  }

  if (run_xmllint_validate(schema, cfg_path) != 0) {
    fprintf(stderr, "zcm_proc: invalid config file: %s\n", cfg_path);
    return -1;
  }

  char value[128];
  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/@name)", value, sizeof(value)) != 0 || !value[0]) {
    fprintf(stderr, "zcm_proc: config missing process name in %s\n", cfg_path);
    return -1;
  }
  if (strcmp(value, name) != 0) {
    fprintf(stderr, "zcm_proc: config process name mismatch (%s != %s)\n", value, name);
    return -1;
  }

  /*
   * dataSocket entries are handled by the zcm_proc application for PUB/SUB
   * configuration. zcm_proc_init keeps the caller-provided data_type/bind_data
   * (typically REP/bind for daemon request handling).
   */
  (void)data_type;
  (void)bind_data;

  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/control/@timeoutMs)", value, sizeof(value)) == 0 &&
      value[0] != '\0') {
    char *end = NULL;
    long ms = strtol(value, &end, 10);
    if (!end || *end != '\0' || ms <= 0 || ms > 600000) {
      fprintf(stderr, "zcm_proc: invalid control@timeoutMs in %s\n", cfg_path);
      return -1;
    }
    *ctrl_timeout_ms = (int)ms;
  }

  return 0;
}

static int load_domain_info(char **broker_ep, char **host_out, int *first_port, int *range_size) {
  const char *domain = getenv("ZCMDOMAIN");
  if (!domain || !*domain) return -1;

  const char *env = getenv("ZCMDOMAIN_DATABASE");
  if (!env || !*env) env = getenv("ZCMMGR");

  char file_name[512];
  if (env && *env) {
    snprintf(file_name, sizeof(file_name), "%s/ZCmDomains", env);
  } else {
    const char *root = getenv("ZCMROOT");
    if (!root || !*root) return -1;
    snprintf(file_name, sizeof(file_name), "%s/mgr/ZCmDomains", root);
  }

  FILE *f = fopen(file_name, "r");
  if (!f) return -1;

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    if (line[0] == '#' || line[0] == '\0') continue;

    char *p = line;
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_domain = strsep(&p, " \t");
    if (!tok_domain || strcmp(tok_domain, domain) != 0) continue;

    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_host = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_port = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_first = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_range = strsep(&p, " \t");

    if (!tok_host || !tok_port || !*tok_host || !*tok_port) {
      fclose(f);
      return -1;
    }

    char *endpoint = malloc(512);
    if (!endpoint) {
      fclose(f);
      return -1;
    }
    snprintf(endpoint, 512, "tcp://%s:%s", tok_host, tok_port);
    fclose(f);
    *broker_ep = endpoint;
    if (host_out) *host_out = strdup(tok_host);
    if (first_port) *first_port = tok_first ? atoi(tok_first) : 0;
    if (range_size) *range_size = tok_range ? atoi(tok_range) : 0;
    return 0;
  }

  fclose(f);
  return -1;
}

static void *ctrl_thread_main(void *arg) {
  struct zcm_proc *proc = (struct zcm_proc *)arg;
  for (;;) {
    char ctrl_buf[64] = {0};
    size_t ctrl_len = 0;
    if (proc->stop) break;
    if (zcm_socket_recv_bytes(proc->ctrl, ctrl_buf, sizeof(ctrl_buf) - 1, &ctrl_len) == 0) {
      ctrl_buf[ctrl_len] = '\0';
      if (strcmp(ctrl_buf, "SHUTDOWN") == 0) {
        const char *ok = "OK";
        zcm_socket_send_bytes(proc->ctrl, ok, strlen(ok));
        zcm_node_unregister(proc->node, proc->name);
        exit(0);
      } else if (strcmp(ctrl_buf, "PING") == 0) {
        printf("PING received\n");
        fflush(stdout);
        const char *pong = "PONG";
        zcm_socket_send_bytes(proc->ctrl, pong, strlen(pong));
      } else {
        const char *err = "ERR";
        zcm_socket_send_bytes(proc->ctrl, err, strlen(err));
      }
    } else {
      if (proc->stop) break;
    }
  }
  return NULL;
}

static int bind_in_range(zcm_socket_t *sock, int first_port, int range_size, int *out_port, int skip_port) {
  if (first_port <= 0) first_port = 7000;
  if (range_size <= 0) range_size = 100;
  for (int i = 0; i < range_size; i++) {
    int port = first_port + i;
    if (port == skip_port) continue;
    char bind_ep[256];
    snprintf(bind_ep, sizeof(bind_ep), "tcp://0.0.0.0:%d", port);
    if (zcm_socket_bind(sock, bind_ep) == 0) {
      if (out_port) *out_port = port;
      return 0;
    }
  }
  return -1;
}

int zcm_proc_init(const char *name, zcm_socket_type_t data_type, int bind_data,
                  zcm_proc_t **out_proc, zcm_socket_t **out_data) {
  if (!name || !out_proc) return -1;
  *out_proc = NULL;
  if (out_data) *out_data = NULL;

  zcm_socket_type_t cfg_data_type = data_type;
  int cfg_bind_data = bind_data ? 1 : 0;
  int cfg_ctrl_timeout_ms = 200;
  int announce_interval_ms = ZCM_PROC_REANNOUNCE_MS_DEFAULT;
  int rc = -1;

  char *broker = NULL;
  char *host = NULL;
  int first_port = 0;
  int range_size = 0;

  zcm_context_t *ctx = NULL;
  zcm_node_t *node = NULL;
  zcm_socket_t *data = NULL;
  zcm_socket_t *ctrl = NULL;
  int data_port = -1;
  int ctrl_port = -1;

  const char *announce_env = getenv("ZCM_PROC_REANNOUNCE_MS");
  if (announce_env && *announce_env) {
    int parsed = 0;
    if (parse_positive_int_range(announce_env, ZCM_PROC_REANNOUNCE_MS_MIN,
                                 ZCM_PROC_REANNOUNCE_MS_MAX, &parsed) == 0) {
      announce_interval_ms = parsed;
    } else {
      fprintf(stderr,
              "zcm_proc: invalid ZCM_PROC_REANNOUNCE_MS='%s', using default %d ms\n",
              announce_env, ZCM_PROC_REANNOUNCE_MS_DEFAULT);
    }
  }

  if (load_proc_config(name, &cfg_data_type, &cfg_bind_data, &cfg_ctrl_timeout_ms) != 0) {
    goto fail;
  }
  if (load_domain_info(&broker, &host, &first_port, &range_size) != 0) {
    fprintf(stderr, "zcm_proc: missing ZCMDOMAIN or ZCmDomains entry\n");
    goto fail;
  }

  ctx = zcm_context_new();
  if (!ctx) goto fail;
  node = zcm_node_new(ctx, broker);
  if (!node) goto fail;

  if (cfg_bind_data) {
    data = zcm_socket_new(ctx, cfg_data_type);
    if (!data) goto fail;
    if (bind_in_range(data, first_port, range_size, &data_port, -1) != 0) {
      fprintf(stderr, "zcm_proc: data bind failed\n");
      goto fail;
    }
  }

  ctrl = zcm_socket_new(ctx, ZCM_SOCK_REP);
  if (!ctrl) goto fail;
  zcm_socket_set_timeouts(ctrl, cfg_ctrl_timeout_ms);
  if (bind_in_range(ctrl, first_port, range_size, &ctrl_port, data_port) != 0) {
    fprintf(stderr, "zcm_proc: control bind failed\n");
    goto fail;
  }

  const char *use_host = host ? host : "127.0.0.1";
  char data_reg_ep[256] = {0};
  char ctrl_reg_ep[256] = {0};
  if (data_port > 0) snprintf(data_reg_ep, sizeof(data_reg_ep), "tcp://%s:%d", use_host, data_port);
  snprintf(ctrl_reg_ep, sizeof(ctrl_reg_ep), "tcp://%s:%d", use_host, ctrl_port);

  const char *reg_ep = (data_port > 0) ? data_reg_ep : ctrl_reg_ep;
  if (zcm_node_register_ex(node, name, reg_ep, ctrl_reg_ep, use_host, getpid()) != 0) {
    fprintf(stderr, "zcm_proc: register failed (broker not found)\n");
    goto fail;
  }

  zcm_proc_t *proc = calloc(1, sizeof(*proc));
  if (!proc) goto fail;
  proc->ctx = ctx;
  proc->node = node;
  proc->ctrl = ctrl;
  proc->data = data;
  proc->name = strdup(name);
  proc->reg_endpoint = strdup(reg_ep);
  proc->ctrl_reg_endpoint = strdup(ctrl_reg_ep);
  proc->host = strdup(use_host);
  proc->pid = getpid();
  proc->announce_interval_ms = announce_interval_ms;
  proc->announce_ok = 1;
  proc->stop = 0;
  if (!proc->name || !proc->reg_endpoint || !proc->ctrl_reg_endpoint || !proc->host) {
    free(proc->name);
    free(proc->reg_endpoint);
    free(proc->ctrl_reg_endpoint);
    free(proc->host);
    free(proc);
    goto fail;
  }

  if (pthread_create(&proc->ctrl_thread, NULL, ctrl_thread_main, proc) != 0) {
    free(proc->name);
    free(proc->reg_endpoint);
    free(proc->ctrl_reg_endpoint);
    free(proc->host);
    free(proc);
    goto fail;
  }
  if (pthread_create(&proc->announce_thread, NULL, announce_thread_main, proc) != 0) {
    proc->stop = 1;
    pthread_join(proc->ctrl_thread, NULL);
    free(proc->name);
    free(proc->reg_endpoint);
    free(proc->ctrl_reg_endpoint);
    free(proc->host);
    free(proc);
    goto fail;
  }

  *out_proc = proc;
  if (out_data) *out_data = data;
  rc = 0;

fail:
  if (rc != 0) {
    if (ctrl) zcm_socket_free(ctrl);
    if (data) zcm_socket_free(data);
    if (node) zcm_node_free(node);
    if (ctx) zcm_context_free(ctx);
  }
  free(broker);
  free(host);
  return rc;
}

void zcm_proc_free(zcm_proc_t *proc) {
  if (!proc) return;
  proc->stop = 1;
  pthread_join(proc->announce_thread, NULL);
  pthread_join(proc->ctrl_thread, NULL);
  zcm_node_unregister(proc->node, proc->name);
  if (proc->ctrl) zcm_socket_free(proc->ctrl);
  if (proc->data) zcm_socket_free(proc->data);
  if (proc->node) zcm_node_free(proc->node);
  if (proc->ctx) zcm_context_free(proc->ctx);
  free(proc->name);
  free(proc->reg_endpoint);
  free(proc->ctrl_reg_endpoint);
  free(proc->host);
  free(proc);
}

zcm_context_t *zcm_proc_context(zcm_proc_t *proc) {
  return proc ? proc->ctx : NULL;
}

zcm_node_t *zcm_proc_node(zcm_proc_t *proc) {
  return proc ? proc->node : NULL;
}
