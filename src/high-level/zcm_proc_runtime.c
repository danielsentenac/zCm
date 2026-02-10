#include "zcm/zcm_proc_runtime.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ZCM_PROC_CONFIG_SCHEMA_DEFAULT
#define ZCM_PROC_CONFIG_SCHEMA_DEFAULT "config/schema/proc-config.xsd"
#endif

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

static int parse_type_arg_kind(const char *text, zcm_proc_type_arg_kind_t *out) {
  if (!text || !*text || !out) return -1;
  if (strcasecmp(text, "text") == 0) { *out = ZCM_PROC_TYPE_ARG_TEXT; return 0; }
  if (strcasecmp(text, "double") == 0) { *out = ZCM_PROC_TYPE_ARG_DOUBLE; return 0; }
  if (strcasecmp(text, "float") == 0) { *out = ZCM_PROC_TYPE_ARG_FLOAT; return 0; }
  if (strcasecmp(text, "int") == 0) { *out = ZCM_PROC_TYPE_ARG_INT; return 0; }
  return -1;
}

static const char *type_arg_kind_name(zcm_proc_type_arg_kind_t kind) {
  switch (kind) {
    case ZCM_PROC_TYPE_ARG_TEXT: return "text";
    case ZCM_PROC_TYPE_ARG_DOUBLE: return "double";
    case ZCM_PROC_TYPE_ARG_FLOAT: return "float";
    case ZCM_PROC_TYPE_ARG_INT: return "int";
    default: return "unknown";
  }
}

static void build_type_format(zcm_proc_type_handler_cfg_t *handler) {
  if (!handler) return;
  size_t off = 0;
  int n = snprintf(handler->format, sizeof(handler->format), "%s(", handler->name);
  if (n < 0) return;
  if ((size_t)n >= sizeof(handler->format)) {
    handler->format[sizeof(handler->format) - 1] = '\0';
    return;
  }
  off = (size_t)n;

  for (size_t i = 0; i < handler->arg_count; i++) {
    n = snprintf(handler->format + off, sizeof(handler->format) - off,
                 "%s%s", (i == 0) ? "" : ",", type_arg_kind_name(handler->args[i]));
    if (n < 0) return;
    if ((size_t)n >= sizeof(handler->format) - off) {
      handler->format[sizeof(handler->format) - 1] = '\0';
      return;
    }
    off += (size_t)n;
  }

  snprintf(handler->format + off, sizeof(handler->format) - off, ")");
}

static int load_type_handlers(const char *cfg_path, zcm_proc_runtime_cfg_t *cfg) {
  char value[128] = {0};
  if (run_xmllint_xpath(cfg_path,
                        "string(count(/procConfig/process/handlers/type))",
                        value, sizeof(value)) != 0) {
    return 0;
  }

  int count = 0;
  if (parse_count_str(value, &count) != 0 || count <= 0) return 0;

  for (int i = 1; i <= count; i++) {
    if (cfg->type_handler_count >= ZCM_PROC_TYPE_HANDLER_MAX) {
      fprintf(stderr, "zcm_proc: too many type handlers in %s (max=%d)\n",
              cfg_path, ZCM_PROC_TYPE_HANDLER_MAX);
      return -1;
    }

    char xpath[256];
    char name[64] = {0};
    char reply[128] = {0};

    snprintf(xpath, sizeof(xpath), "string(/procConfig/process/handlers/type[%d]/@name)", i);
    if (run_xmllint_xpath(cfg_path, xpath, name, sizeof(name)) != 0 || !name[0]) {
      fprintf(stderr, "zcm_proc: handlers/type[%d] missing @name in %s\n", i, cfg_path);
      return -1;
    }

    snprintf(xpath, sizeof(xpath), "string(/procConfig/process/handlers/type[%d]/@reply)", i);
    if (run_xmllint_xpath(cfg_path, xpath, reply, sizeof(reply)) != 0 || !reply[0]) {
      fprintf(stderr, "zcm_proc: handlers/type[%d] missing @reply in %s\n", i, cfg_path);
      return -1;
    }

    zcm_proc_type_handler_cfg_t *handler = &cfg->type_handlers[cfg->type_handler_count++];
    memset(handler, 0, sizeof(*handler));
    snprintf(handler->name, sizeof(handler->name), "%s", name);
    snprintf(handler->reply, sizeof(handler->reply), "%s", reply);

    snprintf(xpath, sizeof(xpath),
             "string(count(/procConfig/process/handlers/type[%d]/arg))", i);
    int arg_count = 0;
    if (run_xmllint_xpath(cfg_path, xpath, value, sizeof(value)) == 0 && value[0]) {
      if (parse_count_str(value, &arg_count) != 0 || arg_count < 0) arg_count = 0;
    }

    if (arg_count > ZCM_PROC_TYPE_HANDLER_ARG_MAX) {
      fprintf(stderr, "zcm_proc: type '%s' has too many args in %s (max=%d)\n",
              handler->name, cfg_path, ZCM_PROC_TYPE_HANDLER_ARG_MAX);
      return -1;
    }

    for (int j = 1; j <= arg_count; j++) {
      char kind_text[64] = {0};
      snprintf(xpath, sizeof(xpath),
               "string(/procConfig/process/handlers/type[%d]/arg[%d]/@kind)", i, j);
      if (run_xmllint_xpath(cfg_path, xpath, kind_text, sizeof(kind_text)) != 0 || !kind_text[0]) {
        fprintf(stderr, "zcm_proc: type '%s' arg[%d] missing @kind in %s\n",
                handler->name, j, cfg_path);
        return -1;
      }
      zcm_proc_type_arg_kind_t kind;
      if (parse_type_arg_kind(kind_text, &kind) != 0) {
        fprintf(stderr, "zcm_proc: type '%s' arg[%d] invalid @kind='%s' in %s\n",
                handler->name, j, kind_text, cfg_path);
        return -1;
      }
      handler->args[handler->arg_count++] = kind;
    }

    build_type_format(handler);
  }

  return 0;
}

static int parse_port_str(const char *text, int *out) {
  if (!text || !*text || !out) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < 1 || v > 65535) return -1;
  *out = (int)v;
  return 0;
}

static int parse_interval_str(const char *text, int *out) {
  if (!text || !*text || !out) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < 1 || v > 3600000) return -1;
  *out = (int)v;
  return 0;
}

static int parse_data_socket_kind(const char *text, zcm_proc_data_socket_kind_t *out) {
  if (!text || !*text || !out) return -1;
  if (strcasecmp(text, "PUB") == 0) { *out = ZCM_PROC_DATA_SOCKET_PUB; return 0; }
  if (strcasecmp(text, "SUB") == 0) { *out = ZCM_PROC_DATA_SOCKET_SUB; return 0; }
  return -1;
}

static void trim_token_inplace(char *text) {
  trim_ws_inplace(text);
  if (!*text) return;
  if (text[0] == '"') {
    memmove(text, text + 1, strlen(text));
  }
  size_t n = strlen(text);
  if (n > 0 && text[n - 1] == '"') text[n - 1] = '\0';
}

static int load_data_sockets(const char *cfg_path, zcm_proc_runtime_cfg_t *cfg) {
  char value[128] = {0};
  if (run_xmllint_xpath(cfg_path,
                        "string(count(/procConfig/process/dataSocket))",
                        value, sizeof(value)) != 0) {
    return 0;
  }

  int count = 0;
  if (parse_count_str(value, &count) != 0 || count <= 0) return 0;

  for (int i = 1; i <= count; i++) {
    if (cfg->data_socket_count >= ZCM_PROC_DATA_SOCKET_MAX) {
      fprintf(stderr, "zcm_proc: too many dataSocket entries in %s (max=%d)\n",
              cfg_path, ZCM_PROC_DATA_SOCKET_MAX);
      return -1;
    }

    char xpath[256];
    int port = 0;
    int interval_ms = 1000;
    zcm_proc_data_socket_kind_t kind;
    char target_single[128] = {0};
    char target_multi[512] = {0};

    snprintf(xpath, sizeof(xpath), "string(/procConfig/process/dataSocket[%d]/@type)", i);
    if (run_xmllint_xpath(cfg_path, xpath, value, sizeof(value)) != 0 || !value[0]) break;
    if (parse_data_socket_kind(value, &kind) != 0) {
      fprintf(stderr, "zcm_proc: dataSocket[%d] invalid @type='%s' in %s\n",
              i, value, cfg_path);
      return -1;
    }

    if (kind == ZCM_PROC_DATA_SOCKET_PUB) {
      snprintf(xpath, sizeof(xpath), "string(/procConfig/process/dataSocket[%d]/@port)", i);
      if (run_xmllint_xpath(cfg_path, xpath, value, sizeof(value)) == 0 && value[0]) {
        if (parse_port_str(value, &port) != 0) {
          fprintf(stderr, "zcm_proc: dataSocket[%d] invalid @port='%s' in %s\n",
                  i, value, cfg_path);
          return -1;
        }
      } else {
        fprintf(stderr, "zcm_proc: dataSocket[%d] PUB requires @port in %s\n",
                i, cfg_path);
        return -1;
      }

      char payload[256] = "tick";
      snprintf(xpath, sizeof(xpath), "string(/procConfig/process/dataSocket[%d]/@payload)", i);
      if (run_xmllint_xpath(cfg_path, xpath, value, sizeof(value)) == 0 && value[0]) {
        snprintf(payload, sizeof(payload), "%s", value);
      }

      snprintf(xpath, sizeof(xpath), "string(/procConfig/process/dataSocket[%d]/@intervalMs)", i);
      if (run_xmllint_xpath(cfg_path, xpath, value, sizeof(value)) == 0 && value[0]) {
        if (parse_interval_str(value, &interval_ms) != 0) {
          fprintf(stderr, "zcm_proc: dataSocket[%d] invalid @intervalMs='%s' in %s\n",
                  i, value, cfg_path);
          return -1;
        }
      }

      zcm_proc_data_socket_cfg_t *sock = &cfg->data_sockets[cfg->data_socket_count++];
      memset(sock, 0, sizeof(*sock));
      sock->kind = ZCM_PROC_DATA_SOCKET_PUB;
      sock->port = port;
      sock->interval_ms = interval_ms;
      snprintf(sock->payload, sizeof(sock->payload), "%s", payload);
    } else {
      snprintf(xpath, sizeof(xpath), "string(/procConfig/process/dataSocket[%d]/@target)", i);
      if (run_xmllint_xpath(cfg_path, xpath, target_single, sizeof(target_single)) == 0) {
        trim_token_inplace(target_single);
      }

      snprintf(xpath, sizeof(xpath), "string(/procConfig/process/dataSocket[%d]/@targets)", i);
      if (run_xmllint_xpath(cfg_path, xpath, target_multi, sizeof(target_multi)) == 0) {
        trim_token_inplace(target_multi);
      }

      int added = 0;
      if (target_single[0]) {
        zcm_proc_data_socket_cfg_t *sock = &cfg->data_sockets[cfg->data_socket_count++];
        memset(sock, 0, sizeof(*sock));
        sock->kind = ZCM_PROC_DATA_SOCKET_SUB;
        snprintf(sock->target, sizeof(sock->target), "%s", target_single);
        added = 1;
      }

      if (target_multi[0]) {
        char list[512];
        snprintf(list, sizeof(list), "%s", target_multi);
        char *saveptr = NULL;
        for (char *tok = strtok_r(list, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
          trim_token_inplace(tok);
          if (!tok[0]) continue;
          if (cfg->data_socket_count >= ZCM_PROC_DATA_SOCKET_MAX) {
            fprintf(stderr, "zcm_proc: too many SUB targets in %s (max=%d)\n",
                    cfg_path, ZCM_PROC_DATA_SOCKET_MAX);
            return -1;
          }
          zcm_proc_data_socket_cfg_t *sock = &cfg->data_sockets[cfg->data_socket_count++];
          memset(sock, 0, sizeof(*sock));
          sock->kind = ZCM_PROC_DATA_SOCKET_SUB;
          snprintf(sock->target, sizeof(sock->target), "%s", tok);
          added = 1;
        }
      }

      if (!added) {
        fprintf(stderr, "zcm_proc: dataSocket[%d] SUB has empty @targets in %s\n", i, cfg_path);
        return -1;
      }
    }
  }

  return 0;
}

int zcm_proc_runtime_load_config(const char *cfg_path, zcm_proc_runtime_cfg_t *cfg) {
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
  strncpy(cfg->core_ping_request, "PING", sizeof(cfg->core_ping_request) - 1);
  strncpy(cfg->core_ping_reply, "PONG", sizeof(cfg->core_ping_reply) - 1);
  strncpy(cfg->core_default_reply, "OK", sizeof(cfg->core_default_reply) - 1);

  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/@name)", cfg->name, sizeof(cfg->name)) != 0 ||
      !cfg->name[0]) {
    fprintf(stderr, "zcm_proc: missing process@name in %s\n", cfg_path);
    return -1;
  }

  char handler_value[128] = {0};
  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/handlers/core/@pingRequest)",
                        handler_value, sizeof(handler_value)) == 0 && handler_value[0]) {
    snprintf(cfg->core_ping_request, sizeof(cfg->core_ping_request), "%s", handler_value);
  }
  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/handlers/core/@pingReply)",
                        handler_value, sizeof(handler_value)) == 0 && handler_value[0]) {
    snprintf(cfg->core_ping_reply, sizeof(cfg->core_ping_reply), "%s", handler_value);
  }
  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/handlers/core/@defaultReply)",
                        handler_value, sizeof(handler_value)) == 0 && handler_value[0]) {
    snprintf(cfg->core_default_reply, sizeof(cfg->core_default_reply), "%s", handler_value);
  }
  if (load_type_handlers(cfg_path, cfg) != 0) return -1;
  if (load_data_sockets(cfg_path, cfg) != 0) return -1;
  return 0;
}

int zcm_proc_runtime_bootstrap(const char *cfg_path,
                               zcm_proc_runtime_cfg_t *cfg,
                               zcm_proc_t **out_proc,
                               zcm_socket_t **out_rep) {
  if (!cfg || !out_proc || !out_rep) return -1;
  *out_proc = NULL;
  *out_rep = NULL;

  if (zcm_proc_runtime_load_config(cfg_path, cfg) != 0) return -1;

  if (setenv("ZCM_PROC_CONFIG_FILE", cfg_path, 1) != 0) {
    fprintf(stderr, "zcm_proc: failed to set ZCM_PROC_CONFIG_FILE\n");
    return -1;
  }

  if (zcm_proc_init(cfg->name, ZCM_SOCK_REP, 1, out_proc, out_rep) != 0) {
    return -1;
  }

  return 0;
}

const zcm_proc_type_handler_cfg_t *zcm_proc_runtime_find_type_handler(
    const zcm_proc_runtime_cfg_t *cfg,
    const char *type_name) {
  if (!cfg || !type_name || !*type_name) return NULL;
  for (size_t i = 0; i < cfg->type_handler_count; i++) {
    if (strcasecmp(cfg->type_handlers[i].name, type_name) == 0) {
      return &cfg->type_handlers[i];
    }
  }
  return NULL;
}

static int append_summary(char *buf, size_t buf_size, size_t *off, const char *text) {
  if (!buf || buf_size == 0 || !off || !text) return -1;
  if (*off >= buf_size) return -1;
  int n = snprintf(buf + *off, buf_size - *off, "%s", text);
  if (n < 0) return -1;
  if ((size_t)n >= buf_size - *off) {
    *off = buf_size - 1;
    buf[*off] = '\0';
    return -1;
  }
  *off += (size_t)n;
  return 0;
}

int zcm_proc_runtime_decode_type_payload(zcm_msg_t *msg,
                                         const zcm_proc_type_handler_cfg_t *handler,
                                         char *summary,
                                         size_t summary_size) {
  if (!msg || !handler || !summary || summary_size == 0) return -1;
  summary[0] = '\0';
  size_t off = 0;
  zcm_msg_rewind(msg);

  for (size_t i = 0; i < handler->arg_count; i++) {
    if (i > 0) (void)append_summary(summary, summary_size, &off, ", ");

    if (handler->args[i] == ZCM_PROC_TYPE_ARG_TEXT) {
      const char *text = NULL;
      uint32_t text_len = 0;
      if (zcm_msg_get_text(msg, &text, &text_len) != 0) return -1;
      char item[256];
      snprintf(item, sizeof(item), "text=%.*s", (int)text_len, text);
      (void)append_summary(summary, summary_size, &off, item);
      continue;
    }
    if (handler->args[i] == ZCM_PROC_TYPE_ARG_DOUBLE) {
      double v = 0.0;
      if (zcm_msg_get_double(msg, &v) != 0) return -1;
      char item[96];
      snprintf(item, sizeof(item), "double=%f", v);
      (void)append_summary(summary, summary_size, &off, item);
      continue;
    }
    if (handler->args[i] == ZCM_PROC_TYPE_ARG_FLOAT) {
      float v = 0.0f;
      if (zcm_msg_get_float(msg, &v) != 0) return -1;
      char item[96];
      snprintf(item, sizeof(item), "float=%f", v);
      (void)append_summary(summary, summary_size, &off, item);
      continue;
    }
    if (handler->args[i] == ZCM_PROC_TYPE_ARG_INT) {
      int32_t v = 0;
      if (zcm_msg_get_int(msg, &v) != 0) return -1;
      char item[96];
      snprintf(item, sizeof(item), "int=%d", v);
      (void)append_summary(summary, summary_size, &off, item);
      continue;
    }
    return -1;
  }

  if (zcm_msg_remaining(msg) != 0) return -1;
  return 0;
}

int zcm_proc_runtime_first_pub_port(const zcm_proc_runtime_cfg_t *cfg, int *out_port) {
  if (!cfg || !out_port) return -1;
  for (size_t i = 0; i < cfg->data_socket_count; i++) {
    if (cfg->data_sockets[i].kind == ZCM_PROC_DATA_SOCKET_PUB &&
        cfg->data_sockets[i].port > 0) {
      *out_port = cfg->data_sockets[i].port;
      return 0;
    }
  }
  return -1;
}

typedef struct data_socket_worker_ctx {
  zcm_proc_t *proc;
  char proc_name[128];
  zcm_proc_data_socket_cfg_t sock;
} data_socket_worker_ctx_t;

static int lookup_endpoint(zcm_proc_t *proc, const char *target, char *ep, size_t ep_size) {
  zcm_node_t *node = zcm_proc_node(proc);
  if (!node) return -1;
  if (zcm_node_lookup(node, target, ep, ep_size) != 0) {
    fprintf(stderr, "lookup failed for '%s'\n", target);
    return -1;
  }
  return 0;
}

static int resolve_target_host(zcm_proc_t *proc, const char *target, char *host, size_t host_size) {
  if (!proc || !target || !*target || !host || host_size == 0) return -1;
  zcm_node_t *node = zcm_proc_node(proc);
  if (!node) return -1;

  char data_ep[512] = {0};
  char ctrl_ep[512] = {0};
  int pid = 0;
  if (zcm_node_info(node, target, data_ep, sizeof(data_ep),
                    ctrl_ep, sizeof(ctrl_ep), host, host_size, &pid) != 0) {
    return -1;
  }
  return host[0] ? 0 : -1;
}

static int request_target_pub_port(zcm_proc_t *proc, const char *target, int *out_port) {
  if (!proc || !target || !*target || !out_port) return -1;

  char ep[256] = {0};
  if (lookup_endpoint(proc, target, ep, sizeof(ep)) != 0) return -1;

  zcm_socket_t *req = zcm_socket_new(zcm_proc_context(proc), ZCM_SOCK_REQ);
  if (!req) return -1;
  zcm_socket_set_timeouts(req, 1000);
  if (zcm_socket_connect(req, ep) != 0) {
    zcm_socket_free(req);
    return -1;
  }

  zcm_msg_t *q = zcm_msg_new();
  if (!q) {
    zcm_socket_free(req);
    return -1;
  }
  zcm_msg_set_type(q, "CORE");
  zcm_msg_put_core_text(q, "DATA_PORT");
  if (zcm_socket_send_msg(req, q) != 0) {
    zcm_msg_free(q);
    zcm_socket_free(req);
    return -1;
  }
  zcm_msg_free(q);

  zcm_msg_t *r = zcm_msg_new();
  if (!r) {
    zcm_socket_free(req);
    return -1;
  }
  if (zcm_socket_recv_msg(req, r) != 0) {
    zcm_msg_free(r);
    zcm_socket_free(req);
    return -1;
  }

  int ok = -1;
  zcm_core_value_t core;
  zcm_msg_rewind(r);
  if (zcm_msg_get_core(r, &core) == 0) {
    if (core.kind == ZCM_CORE_VALUE_INT) {
      if (core.i > 0 && core.i <= 65535) {
        *out_port = core.i;
        ok = 0;
      }
    } else if (core.kind == ZCM_CORE_VALUE_TEXT) {
      char tmp[32] = {0};
      int n = (core.text_len < sizeof(tmp) - 1) ? (int)core.text_len : (int)sizeof(tmp) - 1;
      memcpy(tmp, core.text, (size_t)n);
      tmp[n] = '\0';
      if (parse_port_str(tmp, out_port) == 0) ok = 0;
    }
  } else {
    const char *text = NULL;
    uint32_t len = 0;
    zcm_msg_rewind(r);
    if (zcm_msg_get_text(r, &text, &len) == 0) {
      char tmp[32] = {0};
      int n = (len < sizeof(tmp) - 1) ? (int)len : (int)sizeof(tmp) - 1;
      memcpy(tmp, text, (size_t)n);
      tmp[n] = '\0';
      if (parse_port_str(tmp, out_port) == 0) ok = 0;
    }
  }

  zcm_msg_free(r);
  zcm_socket_free(req);
  return ok;
}

static void *pub_worker_main(void *arg) {
  data_socket_worker_ctx_t *ctx = (data_socket_worker_ctx_t *)arg;
  if (!ctx) return NULL;

  zcm_socket_t *pub = zcm_socket_new(zcm_proc_context(ctx->proc), ZCM_SOCK_PUB);
  if (!pub) {
    free(ctx);
    return NULL;
  }

  char ep[128];
  snprintf(ep, sizeof(ep), "tcp://0.0.0.0:%d", ctx->sock.port);
  if (zcm_socket_bind(pub, ep) != 0) {
    fprintf(stderr, "zcm_proc: pub bind failed on %s\n", ep);
    zcm_socket_free(pub);
    free(ctx);
    return NULL;
  }

  printf("data pub started: proc=%s port=%d payload=%s intervalMs=%d\n",
         ctx->proc_name, ctx->sock.port, ctx->sock.payload, ctx->sock.interval_ms);

  for (;;) {
    if (zcm_socket_send_bytes(pub, ctx->sock.payload, strlen(ctx->sock.payload)) != 0) {
      fprintf(stderr, "zcm_proc: pub send failed on port %d\n", ctx->sock.port);
      usleep(200 * 1000);
      continue;
    }
    usleep((useconds_t)ctx->sock.interval_ms * 1000);
  }

  zcm_socket_free(pub);
  free(ctx);
  return NULL;
}

static void *sub_worker_main(void *arg) {
  data_socket_worker_ctx_t *ctx = (data_socket_worker_ctx_t *)arg;
  if (!ctx) return NULL;

  zcm_socket_t *sub = zcm_socket_new(zcm_proc_context(ctx->proc), ZCM_SOCK_SUB);
  if (!sub) {
    free(ctx);
    return NULL;
  }
  zcm_socket_set_timeouts(sub, 1000);

  char host[256] = {0};
  char ep[256] = {0};
  int pub_port = 0;
  for (;;) {
    if (resolve_target_host(ctx->proc, ctx->sock.target, host, sizeof(host)) != 0) {
      usleep(300 * 1000);
      continue;
    }
    if (request_target_pub_port(ctx->proc, ctx->sock.target, &pub_port) != 0) {
      usleep(300 * 1000);
      continue;
    }
    snprintf(ep, sizeof(ep), "tcp://%s:%d", host, pub_port);
    if (zcm_socket_connect(sub, ep) == 0 &&
        zcm_socket_set_subscribe(sub, "", 0) == 0) {
      break;
    }
    usleep(300 * 1000);
  }

  printf("data sub started: proc=%s target=%s endpoint=%s\n",
         ctx->proc_name, ctx->sock.target, ep);

  for (;;) {
    char buf[512] = {0};
    size_t n = 0;
    if (zcm_socket_recv_bytes(sub, buf, sizeof(buf) - 1, &n) == 0) {
      buf[n] = '\0';
      printf("data sub received: from=%s payload=%s\n", ctx->sock.target, buf);
    }
  }

  zcm_socket_free(sub);
  free(ctx);
  return NULL;
}

void zcm_proc_runtime_start_data_workers(const zcm_proc_runtime_cfg_t *cfg, zcm_proc_t *proc) {
  if (!cfg || !proc) return;
  for (size_t i = 0; i < cfg->data_socket_count; i++) {
    data_socket_worker_ctx_t *ctx = (data_socket_worker_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) continue;
    ctx->proc = proc;
    snprintf(ctx->proc_name, sizeof(ctx->proc_name), "%s", cfg->name);
    ctx->sock = cfg->data_sockets[i];

    pthread_t tid;
    void *(*entry)(void *) =
        (ctx->sock.kind == ZCM_PROC_DATA_SOCKET_PUB) ? pub_worker_main : sub_worker_main;
    if (pthread_create(&tid, NULL, entry, ctx) != 0) {
      fprintf(stderr, "zcm_proc: failed to start data socket worker\n");
      free(ctx);
      continue;
    }
    pthread_detach(tid);
  }
}
