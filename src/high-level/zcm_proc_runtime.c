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

static const char *k_builtin_ping_request = "PING";
static const char *k_builtin_ping_reply = "PONG";
static const char *k_builtin_default_reply = "OK";

static int text_equals_nocase(const char *text, uint32_t len, const char *lit) {
  if (!text || !lit) return 0;
  size_t n = strlen(lit);
  if (len != n) return 0;
  return strncasecmp(text, lit, n) == 0;
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

static int load_domain_port_range(int *out_first_port, int *out_range_size) {
  if (!out_first_port || !out_range_size) return -1;

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
    (void)strsep(&p, " \t"); /* nameserver host */
    while (p && (*p == ' ' || *p == '\t')) p++;
    (void)strsep(&p, " \t"); /* nameserver port */
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_first = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_range = strsep(&p, " \t");

    int first_port = tok_first ? atoi(tok_first) : 0;
    int range_size = tok_range ? atoi(tok_range) : 0;
    fclose(f);

    if (first_port <= 0) first_port = 7000;
    if (range_size <= 0) range_size = 100;
    *out_first_port = first_port;
    *out_range_size = range_size;
    return 0;
  }

  fclose(f);
  return -1;
}

static int bind_pub_in_domain_range(zcm_socket_t *pub, int *out_port) {
  if (!pub || !out_port) return -1;

  int first_port = 7000;
  int range_size = 100;
  (void)load_domain_port_range(&first_port, &range_size);

  for (int i = 0; i < range_size; i++) {
    int port = first_port + i;
    if (port <= 0 || port > 65535) continue;
    char ep[128];
    snprintf(ep, sizeof(ep), "tcp://0.0.0.0:%d", port);
    if (zcm_socket_bind(pub, ep) == 0) {
      *out_port = port;
      return 0;
    }
  }

  return -1;
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
  if (strcasecmp(text, "PUSH") == 0) { *out = ZCM_PROC_DATA_SOCKET_PUSH; return 0; }
  if (strcasecmp(text, "PULL") == 0) { *out = ZCM_PROC_DATA_SOCKET_PULL; return 0; }
  return -1;
}

static const char *data_socket_kind_name(zcm_proc_data_socket_kind_t kind) {
  switch (kind) {
    case ZCM_PROC_DATA_SOCKET_PUB: return "PUB";
    case ZCM_PROC_DATA_SOCKET_SUB: return "SUB";
    case ZCM_PROC_DATA_SOCKET_PUSH: return "PUSH";
    case ZCM_PROC_DATA_SOCKET_PULL: return "PULL";
    default: return "UNKNOWN";
  }
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

static int parse_topics_csv(const char *csv,
                            char out_topics[ZCM_PROC_SUB_TOPIC_MAX][128],
                            size_t *out_count) {
  if (!out_topics || !out_count) return -1;
  *out_count = 0;
  if (!csv || !*csv) return 0;

  char list[512];
  snprintf(list, sizeof(list), "%s", csv);

  char *saveptr = NULL;
  for (char *tok = strtok_r(list, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
    trim_token_inplace(tok);
    if (!tok[0]) continue;
    if (*out_count >= ZCM_PROC_SUB_TOPIC_MAX) return -1;
    if (strlen(tok) >= sizeof(out_topics[0])) return -1;
    snprintf(out_topics[*out_count], sizeof(out_topics[0]), "%s", tok);
    (*out_count)++;
  }

  return 0;
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
    int interval_ms = 1000;
    zcm_proc_data_socket_kind_t kind;
    char target_single[128] = {0};
    char target_multi[512] = {0};
    char topics_csv[512] = {0};
    char parsed_topics[ZCM_PROC_SUB_TOPIC_MAX][128] = {{0}};
    size_t parsed_topic_count = 0;

    snprintf(xpath, sizeof(xpath), "string(/procConfig/process/dataSocket[%d]/@type)", i);
    if (run_xmllint_xpath(cfg_path, xpath, value, sizeof(value)) != 0 || !value[0]) break;
    if (parse_data_socket_kind(value, &kind) != 0) {
      fprintf(stderr, "zcm_proc: dataSocket[%d] invalid @type='%s' in %s\n",
              i, value, cfg_path);
      return -1;
    }

    if (kind == ZCM_PROC_DATA_SOCKET_PUB || kind == ZCM_PROC_DATA_SOCKET_PUSH) {
      snprintf(xpath, sizeof(xpath), "string(/procConfig/process/dataSocket[%d]/@port)", i);
      if (run_xmllint_xpath(cfg_path, xpath, value, sizeof(value)) == 0 && value[0]) {
        fprintf(stderr, "zcm_proc: dataSocket[%d] %s must not define @port in %s\n",
                i, data_socket_kind_name(kind), cfg_path);
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
      sock->kind = kind;
      sock->port = 0;
      sock->interval_ms = interval_ms;
      snprintf(sock->payload, sizeof(sock->payload), "%s", payload);
    } else {
      snprintf(xpath, sizeof(xpath), "string(/procConfig/process/dataSocket[%d]/@topics)", i);
      if (run_xmllint_xpath(cfg_path, xpath, topics_csv, sizeof(topics_csv)) == 0) {
        trim_token_inplace(topics_csv);
      }
      if (kind != ZCM_PROC_DATA_SOCKET_SUB && topics_csv[0]) {
        fprintf(stderr, "zcm_proc: dataSocket[%d] %s does not support @topics in %s\n",
                i, data_socket_kind_name(kind), cfg_path);
        return -1;
      }
      if (kind == ZCM_PROC_DATA_SOCKET_SUB && topics_csv[0]) {
        if (parse_topics_csv(topics_csv, parsed_topics, &parsed_topic_count) != 0 ||
            parsed_topic_count == 0) {
          fprintf(stderr, "zcm_proc: dataSocket[%d] SUB has invalid @topics in %s\n", i, cfg_path);
          return -1;
        }
      }

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
        sock->kind = kind;
        snprintf(sock->target, sizeof(sock->target), "%s", target_single);
        sock->topic_count = parsed_topic_count;
        for (size_t k = 0; k < parsed_topic_count; k++) {
          snprintf(sock->topics[k], sizeof(sock->topics[k]), "%s", parsed_topics[k]);
        }
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
            fprintf(stderr, "zcm_proc: too many %s targets in %s (max=%d)\n",
                    data_socket_kind_name(kind), cfg_path, ZCM_PROC_DATA_SOCKET_MAX);
            return -1;
          }
          zcm_proc_data_socket_cfg_t *sock = &cfg->data_sockets[cfg->data_socket_count++];
          memset(sock, 0, sizeof(*sock));
          sock->kind = kind;
          snprintf(sock->target, sizeof(sock->target), "%s", tok);
          sock->topic_count = parsed_topic_count;
          for (size_t k = 0; k < parsed_topic_count; k++) {
            snprintf(sock->topics[k], sizeof(sock->topics[k]), "%s", parsed_topics[k]);
          }
          added = 1;
        }
      }

      if (!added) {
        fprintf(stderr, "zcm_proc: dataSocket[%d] %s has empty @targets in %s\n",
                i, data_socket_kind_name(kind), cfg_path);
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

  if (run_xmllint_xpath(cfg_path, "string(/procConfig/process/@name)", cfg->name, sizeof(cfg->name)) != 0 ||
      !cfg->name[0]) {
    fprintf(stderr, "zcm_proc: missing process@name in %s\n", cfg_path);
    return -1;
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

const char *zcm_proc_runtime_data_role(const zcm_proc_runtime_cfg_t *cfg) {
  if (!cfg) return "NONE";
  int mask = 0;
  for (size_t i = 0; i < cfg->data_socket_count; i++) {
    if (cfg->data_sockets[i].kind == ZCM_PROC_DATA_SOCKET_PUB) mask |= 1;
    if (cfg->data_sockets[i].kind == ZCM_PROC_DATA_SOCKET_SUB) mask |= 2;
    if (cfg->data_sockets[i].kind == ZCM_PROC_DATA_SOCKET_PUSH) mask |= 4;
    if (cfg->data_sockets[i].kind == ZCM_PROC_DATA_SOCKET_PULL) mask |= 8;
  }

  switch (mask) {
    case 0: return "NONE";
    case 1: return "PUB";
    case 2: return "SUB";
    case 3: return "PUB+SUB";
    case 4: return "PUSH";
    case 5: return "PUB+PUSH";
    case 6: return "SUB+PUSH";
    case 7: return "PUB+SUB+PUSH";
    case 8: return "PULL";
    case 9: return "PUB+PULL";
    case 10: return "SUB+PULL";
    case 11: return "PUB+SUB+PULL";
    case 12: return "PUSH+PULL";
    case 13: return "PUB+PUSH+PULL";
    case 14: return "SUB+PUSH+PULL";
    default: return "PUB+SUB+PUSH+PULL";
  }
}

const char *zcm_proc_runtime_builtin_ping_request(void) {
  return k_builtin_ping_request;
}

const char *zcm_proc_runtime_builtin_ping_reply(void) {
  return k_builtin_ping_reply;
}

const char *zcm_proc_runtime_builtin_default_reply(void) {
  return k_builtin_default_reply;
}

const char *zcm_proc_runtime_builtin_reply_for_command(const char *cmd, uint32_t cmd_len) {
  if (text_equals_nocase(cmd, cmd_len, k_builtin_ping_request)) {
    return k_builtin_ping_reply;
  }
  return k_builtin_default_reply;
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

int zcm_proc_runtime_first_push_port(const zcm_proc_runtime_cfg_t *cfg, int *out_port) {
  if (!cfg || !out_port) return -1;
  for (size_t i = 0; i < cfg->data_socket_count; i++) {
    if (cfg->data_sockets[i].kind == ZCM_PROC_DATA_SOCKET_PUSH &&
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
  zcm_socket_t *bound_tx_socket;
  zcm_proc_runtime_sub_payload_cb_t on_sub_payload;
  void *user;
} data_socket_worker_ctx_t;

static int data_socket_is_sender(zcm_proc_data_socket_kind_t kind) {
  return (kind == ZCM_PROC_DATA_SOCKET_PUB ||
          kind == ZCM_PROC_DATA_SOCKET_PUSH);
}

static int data_socket_is_receiver(zcm_proc_data_socket_kind_t kind) {
  return (kind == ZCM_PROC_DATA_SOCKET_SUB ||
          kind == ZCM_PROC_DATA_SOCKET_PULL);
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

static int parse_port_from_reply(zcm_msg_t *reply, int *out_port) {
  if (!reply || !out_port) return -1;

  const char *text = NULL;
  uint32_t len = 0;
  int32_t code = 200;

  zcm_msg_rewind(reply);
  if (zcm_msg_get_text(reply, &text, &len) == 0 &&
      zcm_msg_get_int(reply, &code) == 0 &&
      zcm_msg_remaining(reply) == 0) {
    if (code != 200) return -1;
    char tmp[32] = {0};
    int n = (len < sizeof(tmp) - 1) ? (int)len : (int)sizeof(tmp) - 1;
    memcpy(tmp, text, (size_t)n);
    tmp[n] = '\0';
    return parse_port_str(tmp, out_port);
  }

  zcm_msg_rewind(reply);
  if (zcm_msg_get_text(reply, &text, &len) == 0 &&
      zcm_msg_remaining(reply) == 0) {
    char tmp[32] = {0};
    int n = (len < sizeof(tmp) - 1) ? (int)len : (int)sizeof(tmp) - 1;
    memcpy(tmp, text, (size_t)n);
    tmp[n] = '\0';
    return parse_port_str(tmp, out_port);
  }

  zcm_msg_rewind(reply);
  int32_t port_i = 0;
  if (zcm_msg_get_int(reply, &port_i) == 0 &&
      zcm_msg_remaining(reply) == 0 &&
      port_i > 0 && port_i <= 65535) {
    *out_port = port_i;
    return 0;
  }

  return -1;
}

static int query_target_port_once(zcm_proc_t *proc, const char *endpoint,
                                  const char *cmd, int include_code,
                                  int *out_port) {
  if (!proc || !endpoint || !*endpoint || !cmd || !*cmd || !out_port) return -1;

  zcm_socket_t *req = zcm_socket_new(zcm_proc_context(proc), ZCM_SOCK_REQ);
  if (!req) return -1;
  zcm_socket_set_timeouts(req, 1000);
  if (zcm_socket_connect(req, endpoint) != 0) {
    zcm_socket_free(req);
    return -1;
  }

  zcm_msg_t *q = zcm_msg_new();
  if (!q) {
    zcm_socket_free(req);
    return -1;
  }
  zcm_msg_set_type(q, "ZCM_CMD");
  if (zcm_msg_put_text(q, cmd) != 0) {
    zcm_msg_free(q);
    zcm_socket_free(req);
    return -1;
  }
  if (include_code && zcm_msg_put_int(q, 200) != 0) {
    zcm_msg_free(q);
    zcm_socket_free(req);
    return -1;
  }
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

  int rc = parse_port_from_reply(r, out_port);
  zcm_msg_free(r);
  zcm_socket_free(req);
  return rc;
}

static int request_target_data_port(zcm_proc_t *proc, const char *target,
                                    const char *cmd, const char *legacy_cmd,
                                    int *out_port) {
  if (!proc || !target || !*target || !cmd || !*cmd || !out_port) return -1;

  char ep[256] = {0};
  if (lookup_endpoint(proc, target, ep, sizeof(ep)) != 0) return -1;

  const char *commands[2] = {cmd, legacy_cmd};
  for (size_t i = 0; i < 2; i++) {
    const char *c = commands[i];
    if (!c || !*c) continue;
    if (i == 1 && strcmp(c, cmd) == 0) continue;
    if (query_target_port_once(proc, ep, c, 1, out_port) == 0 ||
        query_target_port_once(proc, ep, c, 0, out_port) == 0) {
      return 0;
    }
  }
  return -1;
}

static void *tx_worker_main(void *arg) {
  data_socket_worker_ctx_t *ctx = (data_socket_worker_ctx_t *)arg;
  if (!ctx) return NULL;

  zcm_socket_t *tx = ctx->bound_tx_socket;
  if (!tx) {
    free(ctx);
    return NULL;
  }

  const char *kind_name = data_socket_kind_name(ctx->sock.kind);
  printf("[%s %s] started: port=%d payload=\"%s\" intervalMs=%d\n",
         kind_name, ctx->proc_name, ctx->sock.port, ctx->sock.payload, ctx->sock.interval_ms);
  fflush(stdout);

  for (;;) {
    if (zcm_socket_send_bytes(tx, ctx->sock.payload, strlen(ctx->sock.payload)) != 0) {
      fprintf(stderr, "zcm_proc: %s send failed on port %d\n", kind_name, ctx->sock.port);
      usleep(200 * 1000);
      continue;
    }
    usleep((useconds_t)ctx->sock.interval_ms * 1000);
  }

  zcm_socket_free(tx);
  free(ctx);
  return NULL;
}

static void *rx_worker_main(void *arg) {
  data_socket_worker_ctx_t *ctx = (data_socket_worker_ctx_t *)arg;
  if (!ctx) return NULL;

  zcm_socket_type_t sock_type =
      (ctx->sock.kind == ZCM_PROC_DATA_SOCKET_SUB) ? ZCM_SOCK_SUB : ZCM_SOCK_PULL;
  zcm_socket_t *rx = zcm_socket_new(zcm_proc_context(ctx->proc), sock_type);
  if (!rx) {
    free(ctx);
    return NULL;
  }
  zcm_socket_set_timeouts(rx, 1000);

  const char *kind_name = data_socket_kind_name(ctx->sock.kind);
  const char *peer_label =
      (ctx->sock.kind == ZCM_PROC_DATA_SOCKET_SUB) ? "publisher" : "pusher";
  const char *port_cmd =
      (ctx->sock.kind == ZCM_PROC_DATA_SOCKET_SUB) ? "DATA_PORT_PUB" : "DATA_PORT_PUSH";
  const char *legacy_cmd =
      (ctx->sock.kind == ZCM_PROC_DATA_SOCKET_SUB) ? "DATA_PORT" : NULL;

  char host[256] = {0};
  char ep[256] = {0};
  int data_port = 0;
  for (;;) {
    if (resolve_target_host(ctx->proc, ctx->sock.target, host, sizeof(host)) != 0) {
      usleep(300 * 1000);
      continue;
    }
    if (request_target_data_port(ctx->proc, ctx->sock.target,
                                 port_cmd, legacy_cmd, &data_port) != 0) {
      usleep(300 * 1000);
      continue;
    }
    snprintf(ep, sizeof(ep), "tcp://%s:%d", host, data_port);

    int connect_ok = (zcm_socket_connect(rx, ep) == 0);
    if (!connect_ok) {
      usleep(300 * 1000);
      continue;
    }

    if (ctx->sock.kind == ZCM_PROC_DATA_SOCKET_SUB) {
      int subscribe_ok = 1;
      if (ctx->sock.topic_count == 0) {
        if (zcm_socket_set_subscribe(rx, "", 0) != 0) subscribe_ok = 0;
      } else {
        for (size_t i = 0; i < ctx->sock.topic_count; i++) {
          if (zcm_socket_set_subscribe(rx, ctx->sock.topics[i], strlen(ctx->sock.topics[i])) != 0) {
            subscribe_ok = 0;
            break;
          }
        }
      }
      if (!subscribe_ok) {
        usleep(300 * 1000);
        continue;
      }
    }

    if (connect_ok) {
      break;
    }
  }

  printf("[%s %s] connected to %s=%s endpoint=%s\n",
         kind_name, ctx->proc_name, peer_label, ctx->sock.target, ep);
  if (ctx->sock.kind == ZCM_PROC_DATA_SOCKET_SUB && ctx->sock.topic_count > 0) {
    printf("[SUB %s] topics:", ctx->proc_name);
    for (size_t i = 0; i < ctx->sock.topic_count; i++) {
      printf(" %s%s", ctx->sock.topics[i], (i + 1 < ctx->sock.topic_count) ? "," : "");
    }
    printf("\n");
  }
  fflush(stdout);

  for (;;) {
    char buf[512] = {0};
    size_t n = 0;
    if (zcm_socket_recv_bytes(rx, buf, sizeof(buf) - 1, &n) == 0) {
      buf[n] = '\0';
      if (ctx->on_sub_payload) {
        ctx->on_sub_payload(ctx->proc_name, ctx->sock.target, buf, n, ctx->user);
      }
      printf("[%s %s] received payload from %s: \"%s\" (%zu bytes)\n",
             kind_name, ctx->proc_name, ctx->sock.target, buf, n);
      fflush(stdout);
    }
  }

  zcm_socket_free(rx);
  free(ctx);
  return NULL;
}

void zcm_proc_runtime_start_data_workers(zcm_proc_runtime_cfg_t *cfg,
                                         zcm_proc_t *proc,
                                         zcm_proc_runtime_sub_payload_cb_t on_sub_payload,
                                         void *user) {
  if (!cfg || !proc) return;
  for (size_t i = 0; i < cfg->data_socket_count; i++) {
    data_socket_worker_ctx_t *ctx = (data_socket_worker_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) continue;
    ctx->proc = proc;
    snprintf(ctx->proc_name, sizeof(ctx->proc_name), "%s", cfg->name);
    ctx->sock = cfg->data_sockets[i];
    ctx->bound_tx_socket = NULL;
    ctx->on_sub_payload = on_sub_payload;
    ctx->user = user;

    if (!data_socket_is_sender(ctx->sock.kind) &&
        !data_socket_is_receiver(ctx->sock.kind)) {
      fprintf(stderr, "zcm_proc: unsupported data socket kind=%d\n", (int)ctx->sock.kind);
      free(ctx);
      continue;
    }

    if (data_socket_is_sender(ctx->sock.kind)) {
      zcm_socket_type_t tx_type =
          (ctx->sock.kind == ZCM_PROC_DATA_SOCKET_PUB) ? ZCM_SOCK_PUB : ZCM_SOCK_PUSH;
      ctx->bound_tx_socket = zcm_socket_new(zcm_proc_context(proc), tx_type);
      if (!ctx->bound_tx_socket) {
        fprintf(stderr, "zcm_proc: failed to create %s socket worker\n",
                data_socket_kind_name(ctx->sock.kind));
        free(ctx);
        continue;
      }
      if (bind_pub_in_domain_range(ctx->bound_tx_socket, &ctx->sock.port) != 0) {
        fprintf(stderr, "zcm_proc: failed to allocate %s dataSocket port\n",
                data_socket_kind_name(ctx->sock.kind));
        zcm_socket_free(ctx->bound_tx_socket);
        free(ctx);
        continue;
      }
      cfg->data_sockets[i].port = ctx->sock.port;
    }

    pthread_t tid;
    void *(*entry)(void *) =
        data_socket_is_sender(ctx->sock.kind) ? tx_worker_main : rx_worker_main;
    if (pthread_create(&tid, NULL, entry, ctx) != 0) {
      fprintf(stderr, "zcm_proc: failed to start data socket worker\n");
      if (ctx->bound_tx_socket) zcm_socket_free(ctx->bound_tx_socket);
      free(ctx);
      continue;
    }
    pthread_detach(tid);
  }
}
