#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"
#include "zcm/zcm_proc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ZCM_PROC_CONFIG_SCHEMA_DEFAULT
#define ZCM_PROC_CONFIG_SCHEMA_DEFAULT "config/schema/proc-config.xsd"
#endif

typedef struct runtime_cfg {
  char name[128];
  char mode[32];
  char target[128];
  int count;
  char payload[256];
  char request[256];
} runtime_cfg_t;

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

static int parse_count_str(const char *text, int *out) {
  if (!text || !*text || !out) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < -1 || v > 1000000) return -1;
  *out = (int)v;
  return 0;
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

static int load_runtime_config(const char *cfg_path, runtime_cfg_t *cfg) {
  if (!cfg_path || !cfg || !*cfg_path) return -1;
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

  memset(cfg, 0, sizeof(*cfg));
  cfg->count = -1;
  strncpy(cfg->payload, "raw-bytes-proc", sizeof(cfg->payload) - 1);
  strncpy(cfg->request, "PING", sizeof(cfg->request) - 1);

  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/@name)", cfg->name, sizeof(cfg->name)) != 0 || !cfg->name[0]) {
    fprintf(stderr, "zcm_proc: missing process@name in %s\n", cfg_path);
    return -1;
  }

  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/runtime/@mode)", cfg->mode, sizeof(cfg->mode)) != 0 || !cfg->mode[0]) {
    fprintf(stderr, "zcm_proc: missing runtime@mode in %s\n", cfg_path);
    return -1;
  }

  (void)run_xmllint_xpath(cfg_path, "string(/procConfig/process/runtime/@target)", cfg->target, sizeof(cfg->target));

  char count_buf[64] = {0};
  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/runtime/@count)", count_buf, sizeof(count_buf)) == 0 && count_buf[0]) {
    if (parse_count_str(count_buf, &cfg->count) != 0) {
      fprintf(stderr, "zcm_proc: invalid runtime@count in %s\n", cfg_path);
      return -1;
    }
  }

  (void)run_xmllint_xpath(cfg_path, "string(/procConfig/process/runtime/@payload)", cfg->payload, sizeof(cfg->payload));
  if (!cfg->payload[0]) strncpy(cfg->payload, "raw-bytes-proc", sizeof(cfg->payload) - 1);

  (void)run_xmllint_xpath(cfg_path, "string(/procConfig/process/runtime/@request)", cfg->request, sizeof(cfg->request));
  if (!cfg->request[0]) strncpy(cfg->request, "PING", sizeof(cfg->request) - 1);

  if (strcmp(cfg->mode, "daemon") == 0 || strcmp(cfg->mode, "rep") == 0) return 0;
  if (strcmp(cfg->mode, "pub-msg") == 0 || strcmp(cfg->mode, "pub-bytes") == 0) return 0;

  if (strcmp(cfg->mode, "sub-msg") == 0 || strcmp(cfg->mode, "sub-bytes") == 0) {
    if (!cfg->target[0]) {
      fprintf(stderr, "zcm_proc: runtime@target required for mode %s\n", cfg->mode);
      return -1;
    }
    return 0;
  }

  if (strcmp(cfg->mode, "req") == 0) {
    if (!cfg->target[0]) {
      fprintf(stderr, "zcm_proc: runtime@target required for mode req\n");
      return -1;
    }
    if (cfg->count < 0) cfg->count = 1;
    return 0;
  }

  fprintf(stderr, "zcm_proc: unsupported runtime mode: %s\n", cfg->mode);
  return -1;
}

static int lookup_endpoint(zcm_proc_t *proc, const char *target, char *ep, size_t ep_size) {
  zcm_node_t *node = zcm_proc_node(proc);
  if (!node) return -1;
  if (zcm_node_lookup(node, target, ep, ep_size) != 0) {
    fprintf(stderr, "lookup failed for '%s'\n", target);
    return -1;
  }
  return 0;
}

static int text_equals_nocase(const char *text, uint32_t len, const char *lit) {
  size_t n = strlen(lit);
  if (len != n) return 0;
  return strncasecmp(text, lit, n) == 0;
}

static int run_pub_msg(const char *name, int count) {
  zcm_proc_t *proc = NULL;
  zcm_socket_t *pub = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_PUB, 1, &proc, &pub) != 0) return 1;

  for (int i = 0; count < 0 || i < count; i++) {
    zcm_msg_t *msg = zcm_msg_new();
    if (!msg) {
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_set_type(msg, "ProcStatus");
    zcm_msg_put_int(msg, i);
    zcm_msg_put_text(msg, "proc_ok");

    if (zcm_socket_send_msg(pub, msg) != 0) {
      fprintf(stderr, "send failed\n");
      zcm_msg_free(msg);
      zcm_proc_free(proc);
      return 1;
    }

    printf("sent message %d\n", i);
    zcm_msg_free(msg);
    sleep(1);
  }

  zcm_proc_free(proc);
  return 0;
}

static int run_sub_msg(const char *target, const char *self_name, int count) {
  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_SUB, 0, &proc, NULL) != 0) return 1;

  char ep[256] = {0};
  if (lookup_endpoint(proc, target, ep, sizeof(ep)) != 0) {
    zcm_proc_free(proc);
    return 1;
  }

  zcm_socket_t *sub = zcm_socket_new(zcm_proc_context(proc), ZCM_SOCK_SUB);
  if (!sub) {
    zcm_proc_free(proc);
    return 1;
  }
  if (zcm_socket_connect(sub, ep) != 0 ||
      zcm_socket_set_subscribe(sub, "", 0) != 0) {
    fprintf(stderr, "connect/subscribe failed\n");
    zcm_socket_free(sub);
    zcm_proc_free(proc);
    return 1;
  }

  for (int i = 0; count < 0 || i < count; i++) {
    zcm_msg_t *msg = zcm_msg_new();
    if (!msg) {
      zcm_socket_free(sub);
      zcm_proc_free(proc);
      return 1;
    }
    if (zcm_socket_recv_msg(sub, msg) == 0) {
      zcm_core_value_t core;
      zcm_msg_rewind(msg);
      if (zcm_msg_get_core(msg, &core) == 0) {
        if (core.kind == ZCM_CORE_VALUE_TEXT) {
          printf("type=%s core.text=%.*s\n",
                 zcm_msg_get_type(msg), (int)core.text_len, core.text);
        } else if (core.kind == ZCM_CORE_VALUE_DOUBLE) {
          printf("type=%s core.double=%f\n", zcm_msg_get_type(msg), core.d);
        } else if (core.kind == ZCM_CORE_VALUE_FLOAT) {
          printf("type=%s core.float=%f\n", zcm_msg_get_type(msg), core.f);
        } else if (core.kind == ZCM_CORE_VALUE_INT) {
          printf("type=%s core.int=%d\n", zcm_msg_get_type(msg), core.i);
        } else {
          printf("message decode error: unsupported core kind\n");
        }
      } else {
        zcm_msg_rewind(msg);
        int32_t v = 0;
        const char *text = NULL;
        uint32_t text_len = 0;
        if (zcm_msg_get_int(msg, &v) == 0 &&
            zcm_msg_get_text(msg, &text, &text_len) == 0) {
          printf("type=%s v=%d text=%.*s\n",
                 zcm_msg_get_type(msg), v, (int)text_len, text);
        } else {
          printf("message decode error: %s\n", zcm_msg_last_error(msg));
        }
      }
    }
    zcm_msg_free(msg);
  }

  zcm_socket_free(sub);
  zcm_proc_free(proc);
  return 0;
}

static int run_pub_bytes(const char *name, int count, const char *payload) {
  zcm_proc_t *proc = NULL;
  zcm_socket_t *pub = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_PUB, 1, &proc, &pub) != 0) return 1;

  for (int i = 0; count < 0 || i < count; i++) {
    if (zcm_socket_send_bytes(pub, payload, strlen(payload)) != 0) {
      fprintf(stderr, "send failed\n");
      zcm_proc_free(proc);
      return 1;
    }
    printf("sent bytes %d\n", i);
    sleep(1);
  }

  zcm_proc_free(proc);
  return 0;
}

static int run_sub_bytes(const char *target, const char *self_name, int count) {
  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_SUB, 0, &proc, NULL) != 0) return 1;

  char ep[256] = {0};
  if (lookup_endpoint(proc, target, ep, sizeof(ep)) != 0) {
    zcm_proc_free(proc);
    return 1;
  }

  zcm_socket_t *sub = zcm_socket_new(zcm_proc_context(proc), ZCM_SOCK_SUB);
  if (!sub) {
    zcm_proc_free(proc);
    return 1;
  }
  if (zcm_socket_connect(sub, ep) != 0 ||
      zcm_socket_set_subscribe(sub, "", 0) != 0) {
    fprintf(stderr, "connect/subscribe failed\n");
    zcm_socket_free(sub);
    zcm_proc_free(proc);
    return 1;
  }

  for (int i = 0; count < 0 || i < count; i++) {
    char buf[256] = {0};
    size_t n = 0;
    if (zcm_socket_recv_bytes(sub, buf, sizeof(buf) - 1, &n) == 0) {
      buf[n] = '\0';
      printf("received bytes: %s\n", buf);
    }
  }

  zcm_socket_free(sub);
  zcm_proc_free(proc);
  return 0;
}

static int run_daemon(const char *name) {
  zcm_proc_t *proc = NULL;
  zcm_socket_t *rep = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_REP, 1, &proc, &rep) != 0) return 1;
  printf("zcm_proc daemon started: %s\n", name);
  printf("default request/reply: PING -> PONG\n");

  for (;;) {
    zcm_msg_t *req = zcm_msg_new();
    if (!req) {
      zcm_proc_free(proc);
      return 1;
    }
    const char *cmd = NULL;
    uint32_t cmd_len = 0;
    int32_t req_code = 200;
    int core_request = 0;
    if (zcm_socket_recv_msg(rep, req) == 0) {
      zcm_core_value_t core;
      zcm_msg_rewind(req);
      if (zcm_msg_get_core(req, &core) == 0) {
        core_request = 1;
        if (core.kind == ZCM_CORE_VALUE_TEXT) {
          cmd = core.text;
          cmd_len = core.text_len;
          printf("received query: type=%s core.text=%.*s\n",
                 zcm_msg_get_type(req), (int)cmd_len, cmd);
        } else if (core.kind == ZCM_CORE_VALUE_DOUBLE) {
          printf("received query: type=%s core.double=%f\n", zcm_msg_get_type(req), core.d);
        } else if (core.kind == ZCM_CORE_VALUE_FLOAT) {
          printf("received query: type=%s core.float=%f\n", zcm_msg_get_type(req), core.f);
        } else if (core.kind == ZCM_CORE_VALUE_INT) {
          printf("received query: type=%s core.int=%d\n", zcm_msg_get_type(req), core.i);
        }
      } else {
        zcm_msg_rewind(req);
        if (zcm_msg_get_text(req, &cmd, &cmd_len) != 0) {
          cmd = NULL;
          cmd_len = 0;
        }
        if (zcm_msg_get_int(req, &req_code) != 0) {
          req_code = 200;
        }
        printf("received query: type=%s cmd=%s\n",
               zcm_msg_get_type(req), cmd ? cmd : "<none>");
      }
    }
    zcm_msg_free(req);

    zcm_msg_t *reply = zcm_msg_new();
    if (!reply) {
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_set_type(reply, "REPLY");
    const char *reply_text = "ERR";
    if (cmd && text_equals_nocase(cmd, cmd_len, "PING")) {
      reply_text = "PONG";
    } else if (core_request) {
      reply_text = "OK";
    } else if (cmd && cmd_len > 0) {
      reply_text = "OK";
    }

    if (core_request) zcm_msg_put_core_text(reply, reply_text);
    else zcm_msg_put_text(reply, reply_text);
    zcm_msg_put_int(reply, req_code);
    if (zcm_socket_send_msg(rep, reply) != 0) {
      fprintf(stderr, "reply send failed\n");
      zcm_msg_free(reply);
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_free(reply);
  }

  zcm_proc_free(proc);
  return 0;
}

static int run_req(const char *service, const char *self_name, int count, const char *request) {
  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_REQ, 0, &proc, NULL) != 0) return 1;

  char ep[256] = {0};
  if (lookup_endpoint(proc, service, ep, sizeof(ep)) != 0) {
    zcm_proc_free(proc);
    return 1;
  }

  zcm_socket_t *req = zcm_socket_new(zcm_proc_context(proc), ZCM_SOCK_REQ);
  if (!req) {
    zcm_proc_free(proc);
    return 1;
  }
  if (zcm_socket_connect(req, ep) != 0) {
    fprintf(stderr, "connect failed\n");
    zcm_socket_free(req);
    zcm_proc_free(proc);
    return 1;
  }

  for (int i = 0; i < count; i++) {
    zcm_msg_t *msg = zcm_msg_new();
    if (!msg) {
      zcm_socket_free(req);
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_set_type(msg, "QUERY");
    zcm_msg_put_text(msg, request);
    zcm_msg_put_int(msg, 42 + i);

    if (zcm_socket_send_msg(req, msg) != 0) {
      fprintf(stderr, "send failed\n");
      zcm_msg_free(msg);
      zcm_socket_free(req);
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_free(msg);

    zcm_msg_t *reply = zcm_msg_new();
    if (!reply) {
      zcm_socket_free(req);
      zcm_proc_free(proc);
      return 1;
    }
    if (zcm_socket_recv_msg(req, reply) == 0) {
      const char *text = NULL;
      int32_t code = 0;
      if (zcm_msg_get_text(reply, &text, NULL) == 0 &&
          zcm_msg_get_int(reply, &code) == 0) {
        printf("received reply: type=%s text=%s code=%d\n",
               zcm_msg_get_type(reply), text, code);
      } else {
        printf("reply decode error: %s\n", zcm_msg_last_error(reply));
      }
    }
    zcm_msg_free(reply);
    usleep(200 * 1000);
  }

  zcm_socket_free(req);
  zcm_proc_free(proc);
  return 0;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "usage:\n"
          "  %s <proc-config.cfg>\n",
          prog);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    usage(argv[0]);
    return 1;
  }

  runtime_cfg_t cfg;
  if (load_runtime_config(argv[1], &cfg) != 0) return 1;

  if (setenv("ZCM_PROC_CONFIG_FILE", argv[1], 1) != 0) {
    fprintf(stderr, "zcm_proc: failed to set ZCM_PROC_CONFIG_FILE\n");
    return 1;
  }

  if (strcmp(cfg.mode, "daemon") == 0 || strcmp(cfg.mode, "rep") == 0) {
    return run_daemon(cfg.name);
  }
  if (strcmp(cfg.mode, "pub-msg") == 0) {
    return run_pub_msg(cfg.name, cfg.count);
  }
  if (strcmp(cfg.mode, "sub-msg") == 0) {
    return run_sub_msg(cfg.target, cfg.name, cfg.count);
  }
  if (strcmp(cfg.mode, "pub-bytes") == 0) {
    return run_pub_bytes(cfg.name, cfg.count, cfg.payload);
  }
  if (strcmp(cfg.mode, "sub-bytes") == 0) {
    return run_sub_bytes(cfg.target, cfg.name, cfg.count);
  }
  if (strcmp(cfg.mode, "req") == 0) {
    int count = cfg.count;
    if (count < 1) count = 1;
    return run_req(cfg.target, cfg.name, count, cfg.request);
  }

  fprintf(stderr, "zcm_proc: unsupported mode '%s'\n", cfg.mode);
  return 1;
}
