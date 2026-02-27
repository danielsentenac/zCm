#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <zmq.h>

static void usage(const char *prog) {
  fprintf(stderr,
          "usage:\n"
          "  %s names\n"
          "  %s send NAME -type TYPE "
          "(-t TEXT | -d DOUBLE | -f FLOAT | -i INTEGER |\n"
          "                       -c CHAR | -s SHORT | -l LONG | -b BYTES | -a ARRAY_SPEC)+\n"
          "    ARRAY_SPEC: char:v1,v2 | short:v1,v2 | int:v1,v2 | float:v1,v2 | double:v1,v2\n"
          "  %s kill NAME\n"
          "  %s ping NAME\n"
          "  %s broker [ping|stop|list]\n",
          prog, prog, prog, prog, prog);
}

static char *load_endpoint_from_config(void) {
  const char *override = getenv("ZCMBROKER");
  if (!override || !*override) override = getenv("ZCMBROKER_ENDPOINT");
  if (override && *override) {
    char *endpoint = strdup(override);
    if (!endpoint) return NULL;
    return endpoint;
  }

  const char *domain = getenv("ZCMDOMAIN");
  if (!domain || !*domain) return NULL;

  const char *env = getenv("ZCMDOMAIN_DATABASE");
  if (!env || !*env) env = getenv("ZCMMGR");

  char file_name[512];
  if (env && *env) {
    snprintf(file_name, sizeof(file_name), "%s/ZCmDomains", env);
  } else {
    const char *root = getenv("ZCMROOT");
    if (!root || !*root) return NULL;
    snprintf(file_name, sizeof(file_name), "%s/mgr/ZCmDomains", root);
  }

  FILE *f = fopen(file_name, "r");
  if (!f) return NULL;

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

    if (!tok_host || !tok_port || !*tok_host || !*tok_port) {
      fclose(f);
      return NULL;
    }

    char *endpoint = malloc(512);
    if (!endpoint) {
      fclose(f);
      return NULL;
    }
    snprintf(endpoint, 512, "tcp://%s:%s", tok_host, tok_port);
    fclose(f);
    return endpoint;
  }

  fclose(f);
  return NULL;
}

static int parse_port_reply(const char *text, int *out_port) {
  if (!text || !out_port) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < 1 || v > 65535) return -1;
  *out_port = (int)v;
  return 0;
}

static int parse_int_reply(const char *text, int *out_value) {
  if (!text || !out_value) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < INT_MIN || v > INT_MAX) return -1;
  *out_value = (int)v;
  return 0;
}

static int names_query_timeout_ms(void) {
  const int default_timeout_ms = 1000;
  const char *env = getenv("ZCM_NAMES_QUERY_TIMEOUT_MS");
  if (!env || !*env) return default_timeout_ms;
  char *end = NULL;
  long v = strtol(env, &end, 10);
  if (!end || *end != '\0') return default_timeout_ms;
  if (v < 10 || v > 5000) return default_timeout_ms;
  return (int)v;
}

static int names_query_attempts(void) {
  const char *env = getenv("ZCM_NAMES_QUERY_ATTEMPTS");
  if (!env || !*env) return 3;
  char *end = NULL;
  long v = strtol(env, &end, 10);
  if (!end || *end != '\0') return 3;
  if (v < 1 || v > 10) return 3;
  return (int)v;
}

static int endpoint_is_queryable(const char *endpoint) {
  if (!endpoint || !*endpoint) return 0;
  if (strncmp(endpoint, "tcp://", 6) == 0) return 1;
  if (strncmp(endpoint, "ipc://", 6) == 0) return 1;
  if (strncmp(endpoint, "inproc://", 9) == 0) return 1;
  return 0;
}

static void extract_host_from_endpoint(const char *endpoint, char *out_host, size_t out_host_size) {
  if (!out_host || out_host_size == 0) return;
  snprintf(out_host, out_host_size, "-");
  if (!endpoint || !*endpoint) return;

  const char *p = strstr(endpoint, "://");
  p = p ? (p + 3) : endpoint;
  if (!*p) return;

  if (*p == '[') {
    const char *end = strchr(p + 1, ']');
    if (!end || end <= p + 1) return;
    size_t n = (size_t)(end - (p + 1));
    if (n >= out_host_size) n = out_host_size - 1;
    memcpy(out_host, p + 1, n);
    out_host[n] = '\0';
    return;
  }

  const char *last_colon = strrchr(p, ':');
  size_t n = last_colon ? (size_t)(last_colon - p) : strlen(p);
  if (n == 0) return;
  if (n >= out_host_size) n = out_host_size - 1;
  memcpy(out_host, p, n);
  out_host[n] = '\0';
}

static void resolve_hostname_if_ip(char *host, size_t host_size) {
  if (!host || host_size == 0 || !host[0] || strcmp(host, "-") == 0) return;

  struct sockaddr_storage ss;
  socklen_t ss_len = 0;
  memset(&ss, 0, sizeof(ss));

  struct in_addr a4;
  if (inet_pton(AF_INET, host, &a4) == 1) {
    struct sockaddr_in *sa = (struct sockaddr_in *)&ss;
    sa->sin_family = AF_INET;
    sa->sin_addr = a4;
    ss_len = (socklen_t)sizeof(*sa);
  } else {
    struct in6_addr a6;
    if (inet_pton(AF_INET6, host, &a6) == 1) {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&ss;
      sa6->sin6_family = AF_INET6;
      sa6->sin6_addr = a6;
      ss_len = (socklen_t)sizeof(*sa6);
    } else {
      return;
    }
  }

  char resolved[NI_MAXHOST] = {0};
  if (getnameinfo((struct sockaddr *)&ss, ss_len,
                  resolved, sizeof(resolved),
                  NULL, 0, NI_NAMEREQD) == 0 && resolved[0]) {
    size_t n = strlen(resolved);
    if (n > 0 && resolved[n - 1] == '.') resolved[n - 1] = '\0';
    snprintf(host, host_size, "%s", resolved);
  }
}

static int query_proc_command_once(zcm_context_t *ctx, const char *endpoint,
                                   const char *cmd, int include_code_field,
                                   char *out_text, size_t out_text_size,
                                   int *out_code) {
  if (!ctx || !endpoint || !cmd || !out_text || out_text_size == 0) return -1;
  out_text[0] = '\0';
  if (out_code) *out_code = 0;

  int rc = -1;
  zcm_socket_t *req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) return -1;
  zcm_socket_set_timeouts(req, names_query_timeout_ms());
  if (zcm_socket_connect(req, endpoint) != 0) goto out;

  zcm_msg_t *q = zcm_msg_new();
  if (!q) goto out;
  zcm_msg_set_type(q, "ZCM_CMD");
  if (zcm_msg_put_text(q, cmd) != 0) {
    zcm_msg_free(q);
    goto out;
  }
  if (include_code_field && zcm_msg_put_int(q, 200) != 0) {
    zcm_msg_free(q);
    goto out;
  }
  if (zcm_socket_send_msg(req, q) != 0) {
    zcm_msg_free(q);
    goto out;
  }
  zcm_msg_free(q);

  zcm_msg_t *reply = zcm_msg_new();
  if (!reply) goto out;
  if (zcm_socket_recv_msg(req, reply) != 0) {
    zcm_msg_free(reply);
    goto out;
  }

  const char *text = NULL;
  uint32_t text_len = 0;
  int32_t code = 200;

  zcm_msg_rewind(reply);
  if (zcm_msg_get_text(reply, &text, &text_len) == 0 &&
      zcm_msg_get_int(reply, &code) == 0 &&
      zcm_msg_remaining(reply) == 0) {
    size_t n = text_len;
    if (n >= out_text_size) n = out_text_size - 1;
    memcpy(out_text, text, n);
    out_text[n] = '\0';
    if (out_code) *out_code = (int)code;
    rc = 0;
  } else {
    zcm_msg_rewind(reply);
    if (zcm_msg_get_text(reply, &text, &text_len) == 0 &&
        zcm_msg_remaining(reply) == 0) {
      size_t n = text_len;
      if (n >= out_text_size) n = out_text_size - 1;
      memcpy(out_text, text, n);
      out_text[n] = '\0';
      if (out_code) *out_code = 200;
      rc = 0;
    }
  }

  zcm_msg_free(reply);

out:
  zcm_socket_free(req);
  return rc;
}

static int query_proc_command(zcm_context_t *ctx, const char *endpoint,
                              const char *cmd, char *out_text,
                              size_t out_text_size, int *out_code) {
  if (query_proc_command_once(ctx, endpoint, cmd, 1, out_text, out_text_size, out_code) == 0) {
    return 0;
  }
  return query_proc_command_once(ctx, endpoint, cmd, 0, out_text, out_text_size, out_code);
}

static int query_proc_command_with_ctrl_fallback(zcm_context_t *ctx, zcm_node_t *node,
                                                 const char *name, const char *endpoint,
                                                 const char *cmd, char *out_text,
                                                 size_t out_text_size, int *out_code) {
  if (!ctx || !node || !name || !endpoint || !cmd || !out_text || out_text_size == 0) return -1;
  if (query_proc_command(ctx, endpoint, cmd, out_text, out_text_size, out_code) == 0) return 0;

  char ctrl_ep[512] = {0};
  if (zcm_node_info(node, name,
                    NULL, 0,
                    ctrl_ep, sizeof(ctrl_ep),
                    NULL, 0,
                    NULL) != 0) {
    return -1;
  }
  if (!ctrl_ep[0] || strcmp(ctrl_ep, endpoint) == 0) return -1;
  return query_proc_command(ctx, ctrl_ep, cmd, out_text, out_text_size, out_code);
}

static int role_contains_token(const char *role, const char *token) {
  if (!role || !token || !*token) return 0;
  size_t token_len = strlen(token);
  const char *p = role;
  while (p && *p) {
    const char *next = strchr(p, '+');
    size_t len = next ? (size_t)(next - p) : strlen(p);
    if (len >= token_len && strncmp(p, token, token_len) == 0) {
      if (len == token_len || p[token_len] == ':') return 1;
    }
    if (!next) break;
    p = next + 1;
  }
  return 0;
}

static int role_has_pub(const char *role) {
  return role_contains_token(role, "PUB");
}

static int role_has_sub(const char *role) {
  return role_contains_token(role, "SUB");
}

static int role_has_push(const char *role) {
  return role_contains_token(role, "PUSH");
}

static int role_has_pull(const char *role) {
  return role_contains_token(role, "PULL");
}

static void role_add_token(char *role, size_t role_size, const char *token) {
  if (!role || role_size == 0 || !token || !*token) return;
  if (strcmp(role, "UNKNOWN") == 0 || strcmp(role, "NONE") == 0 || role[0] == '\0') {
    snprintf(role, role_size, "%s", token);
    return;
  }
  if (role_contains_token(role, token)) return;

  size_t cur = strlen(role);
  size_t add = strlen(token) + 1; /* + token plus '+' */
  if (cur + add >= role_size) return;
  role[cur] = '+';
  role[cur + 1] = '\0';
  strncat(role, token, role_size - strlen(role) - 1);
}

static int role_is_valid(const char *role) {
  if (!role) return 0;
  if (strcmp(role, "NONE") == 0) return 1;

  const char *p = role;
  while (p && *p) {
    const char *next = strchr(p, '+');
    size_t len = next ? (size_t)(next - p) : strlen(p);
    if (len == 0) return 0;

    int ok = ((len == 3 && strncmp(p, "PUB", 3) == 0) ||
              (len == 3 && strncmp(p, "SUB", 3) == 0) ||
              (len == 4 && strncmp(p, "PUSH", 4) == 0) ||
              (len == 4 && strncmp(p, "PULL", 4) == 0));
    if (!ok) return 0;
    if (!next) break;
    p = next + 1;
  }

  return 1;
}

static int endpoint_is_sub_scheme(const char *endpoint) {
  if (!endpoint) return 0;
  return strncmp(endpoint, "sub://", 6) == 0;
}

static int reply_means_no_pub(const char *text, int code) {
  if (code != 404 || !text || !*text) return 0;
  return (strstr(text, "no PUB") != NULL ||
          strstr(text, "no pub") != NULL ||
          strstr(text, "NO PUB") != NULL);
}

static int reply_means_no_push(const char *text, int code) {
  if (code != 404 || !text || !*text) return 0;
  return (strstr(text, "no PUSH") != NULL ||
          strstr(text, "no push") != NULL ||
          strstr(text, "NO PUSH") != NULL);
}

static void probe_node_role(zcm_context_t *ctx, zcm_node_t *node,
                            const char *name, const char *endpoint,
                            char *out_role, size_t out_role_size,
                            int *out_pub_port, int *out_push_port,
                            int *out_pub_bytes, int *out_sub_bytes,
                            int *out_push_bytes, int *out_pull_bytes,
                            char *out_host, size_t out_host_size) {
  if (!ctx || !node || !name || !endpoint || !out_role || out_role_size == 0 ||
      !out_pub_port || !out_push_port ||
      !out_pub_bytes || !out_sub_bytes || !out_push_bytes || !out_pull_bytes ||
      !out_host || out_host_size == 0) {
    return;
  }

  snprintf(out_role, out_role_size, "UNKNOWN");
  extract_host_from_endpoint(endpoint, out_host, out_host_size);
  resolve_hostname_if_ip(out_host, out_host_size);
  *out_pub_port = -1;
  *out_push_port = -1;
  *out_pub_bytes = -1;
  *out_sub_bytes = -1;
  *out_push_bytes = -1;
  *out_pull_bytes = -1;

  if (strcmp(name, "zcmbroker") == 0) {
    snprintf(out_role, out_role_size, "BROKER");
    return;
  }

  char probe_endpoint[512] = {0};
  snprintf(probe_endpoint, sizeof(probe_endpoint), "%s", endpoint);

  /* Prefer broker metadata first. Some nodes only report metrics and do not
   * expose DATA_* control commands. */
  char info_ep[512] = {0};
  char info_ctrl_ep[512] = {0};
  char info_host[256] = {0};
  int info_pid = 0;
  if (zcm_node_info(node, name,
                    info_ep, sizeof(info_ep),
                    info_ctrl_ep, sizeof(info_ctrl_ep),
                    info_host, sizeof(info_host),
                    &info_pid) == 0) {
    if (info_host[0]) {
      snprintf(out_host, out_host_size, "%s", info_host);
      resolve_hostname_if_ip(out_host, out_host_size);
    } else if (info_ep[0]) {
      extract_host_from_endpoint(info_ep, out_host, out_host_size);
      resolve_hostname_if_ip(out_host, out_host_size);
    }
    if (info_ctrl_ep[0]) {
      snprintf(probe_endpoint, sizeof(probe_endpoint), "%s", info_ctrl_ep);
    }
  }

  {
    const char *ep_for_role = info_ep[0] ? info_ep : endpoint;
    if (endpoint_is_sub_scheme(ep_for_role)) {
      snprintf(out_role, out_role_size, "SUB");
      if (endpoint_is_queryable(probe_endpoint)) {
        char text[128] = {0};
        int code = 0;
        if (query_proc_command_with_ctrl_fallback(ctx, node, name, probe_endpoint,
                                                  "DATA_PAYLOAD_BYTES_SUB",
                                                  text, sizeof(text), &code) == 0 &&
            code == 200) {
          int bytes = -1;
          if (parse_int_reply(text, &bytes) == 0) {
            *out_sub_bytes = bytes;
          }
        }
      }
      return;
    }
  }

  char text[128] = {0};
  int code = 0;
  if (query_proc_command_with_ctrl_fallback(ctx, node, name, probe_endpoint,
                                            "DATA_ROLE", text, sizeof(text), &code) == 0 &&
      code == 200 && role_is_valid(text)) {
    snprintf(out_role, out_role_size, "%s", text);
  }

  if (role_has_pub(out_role) || strcmp(out_role, "UNKNOWN") == 0) {
    text[0] = '\0';
    code = 0;
    if (query_proc_command_with_ctrl_fallback(ctx, node, name, probe_endpoint,
                                              "DATA_PORT_PUB", text, sizeof(text), &code) != 0) {
      (void)query_proc_command_with_ctrl_fallback(ctx, node, name, probe_endpoint,
                                                  "DATA_PORT", text, sizeof(text), &code);
    }
    if (text[0] != '\0' || code != 0) {
      if (code == 200) {
        int port = 0;
        if (parse_port_reply(text, &port) == 0) {
          *out_pub_port = port;
          role_add_token(out_role, out_role_size, "PUB");
        }
      } else if (strcmp(out_role, "UNKNOWN") == 0 && reply_means_no_pub(text, code)) {
        role_add_token(out_role, out_role_size, "SUB");
      }
    }
  }

  if (role_has_push(out_role) || strcmp(out_role, "UNKNOWN") == 0) {
    text[0] = '\0';
    code = 0;
    if (query_proc_command_with_ctrl_fallback(ctx, node, name, probe_endpoint,
                                              "DATA_PORT_PUSH", text, sizeof(text), &code) == 0) {
      if (code == 200) {
        int port = 0;
        if (parse_port_reply(text, &port) == 0) {
          *out_push_port = port;
          role_add_token(out_role, out_role_size, "PUSH");
        }
      } else if (strcmp(out_role, "UNKNOWN") == 0 && reply_means_no_push(text, code)) {
        role_add_token(out_role, out_role_size, "PULL");
      }
    }
  }

  if (role_has_pub(out_role) || strcmp(out_role, "UNKNOWN") == 0) {
    text[0] = '\0';
    code = 0;
    if (query_proc_command_with_ctrl_fallback(ctx, node, name, probe_endpoint,
                                              "DATA_PAYLOAD_BYTES_PUB",
                                              text, sizeof(text), &code) == 0 &&
        code == 200) {
      int bytes = -1;
      if (parse_int_reply(text, &bytes) == 0) {
        *out_pub_bytes = bytes;
        role_add_token(out_role, out_role_size, "PUB");
      }
    }
  }

  if (role_has_sub(out_role) || strcmp(out_role, "UNKNOWN") == 0) {
    text[0] = '\0';
    code = 0;
    if (query_proc_command_with_ctrl_fallback(ctx, node, name, probe_endpoint,
                                              "DATA_PAYLOAD_BYTES_SUB",
                                              text, sizeof(text), &code) == 0 &&
        code == 200) {
      int bytes = -1;
      if (parse_int_reply(text, &bytes) == 0) {
        *out_sub_bytes = bytes;
        role_add_token(out_role, out_role_size, "SUB");
      }
    }
  }

  if (role_has_push(out_role) || strcmp(out_role, "UNKNOWN") == 0) {
    text[0] = '\0';
    code = 0;
    if (query_proc_command_with_ctrl_fallback(ctx, node, name, probe_endpoint,
                                              "DATA_PAYLOAD_BYTES_PUSH",
                                              text, sizeof(text), &code) == 0 &&
        code == 200) {
      int bytes = -1;
      if (parse_int_reply(text, &bytes) == 0) {
        *out_push_bytes = bytes;
        role_add_token(out_role, out_role_size, "PUSH");
      }
    }
  }

  if (role_has_pull(out_role) || strcmp(out_role, "UNKNOWN") == 0) {
    text[0] = '\0';
    code = 0;
    if (query_proc_command_with_ctrl_fallback(ctx, node, name, probe_endpoint,
                                              "DATA_PAYLOAD_BYTES_PULL",
                                              text, sizeof(text), &code) == 0 &&
        code == 200) {
      int bytes = -1;
      if (parse_int_reply(text, &bytes) == 0) {
        *out_pull_bytes = bytes;
        role_add_token(out_role, out_role_size, "PULL");
      }
    }
  }
}

static void probe_node_bytes_by_role(zcm_context_t *ctx, const char *endpoint,
                                     const char *role,
                                     int *out_pub_bytes, int *out_sub_bytes,
                                     int *out_push_bytes, int *out_pull_bytes) {
  if (!out_pub_bytes || !out_sub_bytes || !out_push_bytes || !out_pull_bytes) return;
  *out_pub_bytes = -1;
  *out_sub_bytes = -1;
  *out_push_bytes = -1;
  *out_pull_bytes = -1;

  if (!ctx || !endpoint || !*endpoint || !role || !*role) return;
  if (strcmp(role, "UNKNOWN") == 0 || strcmp(role, "NONE") == 0 || strcmp(role, "BROKER") == 0) return;

  char text[128] = {0};
  int code = 0;
  int bytes = -1;

  if (role_has_pub(role) &&
      query_proc_command(ctx, endpoint,
                         "DATA_PAYLOAD_BYTES_PUB",
                         text, sizeof(text), &code) == 0 &&
      code == 200 &&
      parse_int_reply(text, &bytes) == 0) {
    *out_pub_bytes = bytes;
  }

  if (role_has_sub(role) &&
      query_proc_command(ctx, endpoint,
                         "DATA_PAYLOAD_BYTES_SUB",
                         text, sizeof(text), &code) == 0 &&
      code == 200 &&
      parse_int_reply(text, &bytes) == 0) {
    *out_sub_bytes = bytes;
  }

  if (role_has_push(role) &&
      query_proc_command(ctx, endpoint,
                         "DATA_PAYLOAD_BYTES_PUSH",
                         text, sizeof(text), &code) == 0 &&
      code == 200 &&
      parse_int_reply(text, &bytes) == 0) {
    *out_push_bytes = bytes;
  }

  if (role_has_pull(role) &&
      query_proc_command(ctx, endpoint,
                         "DATA_PAYLOAD_BYTES_PULL",
                         text, sizeof(text), &code) == 0 &&
      code == 200 &&
      parse_int_reply(text, &bytes) == 0) {
    *out_pull_bytes = bytes;
  }
}

typedef struct names_row_info {
  char host[256];
  char role[512];
  char ctrl_endpoint[512];
  char endpoint_display[512];
  char role_display[512];
  char sub_targets_csv[1024];
  char sub_target_bytes_csv[1024];
  int pub_port;
  int push_port;
  int pub_bytes;
  int sub_bytes;
  int push_bytes;
  int pull_bytes;
} names_row_info_t;

static char *trim_ascii_ws_inplace(char *s);

static int parse_int_or_dash(const char *text, int *out_value) {
  if (!text || !out_value) return -1;
  if (strcmp(text, "-") == 0) {
    *out_value = -1;
    return 0;
  }
  return parse_int_reply(text, out_value);
}

static int query_node_metrics_snapshot(zcm_context_t *ctx,
                                       zcm_node_t *node,
                                       const char *name,
                                       const char *endpoint,
                                       names_row_info_t *row) {
  if (!ctx || !endpoint || !*endpoint || !row) return -1;
  if (!endpoint_is_queryable(endpoint)) return -1;

  char reply[4096] = {0};
  int code = 0;
  int ok = 0;
  int attempts = names_query_attempts();
  for (int i = 0; i < attempts; i++) {
    int rc = -1;
    if (node && name && *name) {
      rc = query_proc_command_with_ctrl_fallback(ctx, node, name, endpoint,
                                                 "DATA_METRICS",
                                                 reply, sizeof(reply), &code);
    } else {
      rc = query_proc_command(ctx, endpoint, "DATA_METRICS",
                              reply, sizeof(reply), &code);
    }
    if (rc == 0 && code == 200 && reply[0]) {
      ok = 1;
      break;
    }
    if (i + 1 < attempts) usleep(20 * 1000);
  }
  if (!ok) {
    return -1;
  }

  char copy[4096] = {0};
  snprintf(copy, sizeof(copy), "%s", reply);

  char role[512] = {0};
  char sub_targets[1024] = {0};
  char sub_target_bytes[1024] = {0};
  int pub_port = row->pub_port;
  int push_port = row->push_port;
  int pub_bytes = row->pub_bytes;
  int sub_bytes = row->sub_bytes;
  int push_bytes = row->push_bytes;
  int pull_bytes = row->pull_bytes;

  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, ";", &saveptr);
       tok;
       tok = strtok_r(NULL, ";", &saveptr)) {
    char *item = trim_ascii_ws_inplace(tok);
    if (!item || !item[0]) continue;
    char *eq = strchr(item, '=');
    if (!eq || eq == item) continue;
    *eq = '\0';
    char *key = trim_ascii_ws_inplace(item);
    char *value = trim_ascii_ws_inplace(eq + 1);
    if (!key || !key[0] || !value || !value[0]) continue;

    if (strcmp(key, "ROLE") == 0) {
      if (role_is_valid(value) ||
          strcmp(value, "UNKNOWN") == 0 ||
          strcmp(value, "NONE") == 0 ||
          strcmp(value, "BROKER") == 0) {
        snprintf(role, sizeof(role), "%s", value);
      }
      continue;
    }
    if (strcmp(key, "PUB_PORT") == 0) {
      (void)parse_int_or_dash(value, &pub_port);
      continue;
    }
    if (strcmp(key, "PUSH_PORT") == 0) {
      (void)parse_int_or_dash(value, &push_port);
      continue;
    }
    if (strcmp(key, "PUB_BYTES") == 0) {
      (void)parse_int_or_dash(value, &pub_bytes);
      continue;
    }
    if (strcmp(key, "SUB_BYTES") == 0) {
      (void)parse_int_or_dash(value, &sub_bytes);
      continue;
    }
    if (strcmp(key, "PUSH_BYTES") == 0) {
      (void)parse_int_or_dash(value, &push_bytes);
      continue;
    }
    if (strcmp(key, "PULL_BYTES") == 0) {
      (void)parse_int_or_dash(value, &pull_bytes);
      continue;
    }
    if (strcmp(key, "SUB_TARGETS") == 0) {
      if (strcmp(value, "-") != 0) snprintf(sub_targets, sizeof(sub_targets), "%s", value);
      continue;
    }
    if (strcmp(key, "SUB_TARGET_BYTES") == 0) {
      if (strcmp(value, "-") != 0) snprintf(sub_target_bytes, sizeof(sub_target_bytes), "%s", value);
      continue;
    }
  }

  if (role[0]) snprintf(row->role, sizeof(row->role), "%s", role);
  row->pub_port = pub_port;
  row->push_port = push_port;
  row->pub_bytes = pub_bytes;
  row->sub_bytes = sub_bytes;
  row->push_bytes = push_bytes;
  row->pull_bytes = pull_bytes;
  if (sub_targets[0]) snprintf(row->sub_targets_csv, sizeof(row->sub_targets_csv), "%s", sub_targets);
  if (sub_target_bytes[0]) {
    snprintf(row->sub_target_bytes_csv, sizeof(row->sub_target_bytes_csv), "%s", sub_target_bytes);
  }
  return 0;
}

static int endpoint_sub_to_tcp(const char *endpoint, char *out, size_t out_size) {
  if (!endpoint || !out || out_size == 0) return -1;
  if (!endpoint_is_sub_scheme(endpoint)) return -1;
  if (snprintf(out, out_size, "tcp://%s", endpoint + 6) >= (int)out_size) return -1;
  return 0;
}

static int endpoint_normalize_for_matching(const char *endpoint, char *out, size_t out_size) {
  if (!endpoint || !*endpoint || !out || out_size == 0) return -1;
  if (endpoint_is_sub_scheme(endpoint)) {
    return endpoint_sub_to_tcp(endpoint, out, out_size);
  }
  if (snprintf(out, out_size, "%s", endpoint) >= (int)out_size) return -1;
  return 0;
}

static int endpoint_parse_port(const char *endpoint, int *out_port) {
  if (!endpoint || !out_port) return -1;

  const char *p = strstr(endpoint, "://");
  p = p ? (p + 3) : endpoint;
  if (!*p) return -1;

  const char *port_text = NULL;
  if (*p == '[') {
    const char *end = strchr(p + 1, ']');
    if (!end || end[1] != ':') return -1;
    port_text = end + 2;
  } else {
    const char *last_colon = strrchr(p, ':');
    if (!last_colon || !last_colon[1]) return -1;
    port_text = last_colon + 1;
  }

  char *endptr = NULL;
  long v = strtol(port_text, &endptr, 10);
  if (!endptr || *endptr != '\0') return -1;
  if (v < 1 || v > 65535) return -1;
  *out_port = (int)v;
  return 0;
}

static int endpoint_parse_host(const char *endpoint, char *out_host, size_t out_host_size) {
  if (!endpoint || !out_host || out_host_size == 0) return -1;
  out_host[0] = '\0';

  const char *p = strstr(endpoint, "://");
  p = p ? (p + 3) : endpoint;
  if (!*p) return -1;

  if (*p == '[') {
    const char *end = strchr(p + 1, ']');
    if (!end || end <= p + 1) return -1;
    size_t n = (size_t)(end - (p + 1));
    if (n >= out_host_size) n = out_host_size - 1;
    memcpy(out_host, p + 1, n);
    out_host[n] = '\0';
    return (out_host[0] ? 0 : -1);
  }

  const char *last_colon = strrchr(p, ':');
  size_t n = last_colon ? (size_t)(last_colon - p) : strlen(p);
  if (n == 0) return -1;
  if (n >= out_host_size) n = out_host_size - 1;
  memcpy(out_host, p, n);
  out_host[n] = '\0';
  return 0;
}

static int host_is_connectable(const char *host) {
  if (!host || !*host) return 0;
  if (strcmp(host, "*") == 0 ||
      strcmp(host, "0.0.0.0") == 0 ||
      strcmp(host, "::") == 0 ||
      strcmp(host, "0:0:0:0:0:0:0:0") == 0 ||
      strcmp(host, "-") == 0) return 0;
  return 1;
}

static int build_tcp_endpoint_text(const char *host, int port,
                                   char *out_endpoint, size_t out_endpoint_size) {
  if (!host || !*host || !out_endpoint || out_endpoint_size == 0) return -1;
  if (port < 1 || port > 65535) return -1;
  if (strchr(host, ':') != NULL && host[0] != '[') {
    snprintf(out_endpoint, out_endpoint_size, "tcp://[%s]:%d", host, port);
  } else {
    snprintf(out_endpoint, out_endpoint_size, "tcp://%s:%d", host, port);
  }
  return 0;
}

static void endpoint_resolve_hostname_if_ip(char *endpoint, size_t endpoint_size) {
  if (!endpoint || endpoint_size == 0 || !endpoint[0]) return;
  if (strncmp(endpoint, "tcp://", 6) != 0) return;

  char host[256] = {0};
  int port = -1;
  if (endpoint_parse_host(endpoint, host, sizeof(host)) != 0) return;
  if (endpoint_parse_port(endpoint, &port) != 0) return;

  char resolved[256] = {0};
  snprintf(resolved, sizeof(resolved), "%s", host);
  resolve_hostname_if_ip(resolved, sizeof(resolved));
  if (!resolved[0] || strcmp(resolved, host) == 0) return;

  if (build_tcp_endpoint_text(resolved, port, endpoint, endpoint_size) != 0) return;
}

static int infer_ctrl_endpoint_from_data(const char *data_endpoint, const char *host_hint,
                                         char *out_ctrl_endpoint, size_t out_ctrl_endpoint_size) {
  if (!data_endpoint || !*data_endpoint ||
      !out_ctrl_endpoint || out_ctrl_endpoint_size == 0) return -1;
  out_ctrl_endpoint[0] = '\0';

  char normalized_ep[512] = {0};
  if (endpoint_normalize_for_matching(data_endpoint,
                                      normalized_ep, sizeof(normalized_ep)) != 0) {
    return -1;
  }

  char data_host[256] = {0};
  int data_port = -1;
  if (endpoint_parse_host(normalized_ep, data_host, sizeof(data_host)) != 0) return -1;
  if (endpoint_parse_port(normalized_ep, &data_port) != 0) return -1;
  if (data_port >= 65535) return -1;

  const char *ctrl_host = data_host;
  if (host_is_connectable(host_hint)) ctrl_host = host_hint;
  if (!host_is_connectable(ctrl_host)) return -1;

  return build_tcp_endpoint_text(ctrl_host, data_port + 1,
                                 out_ctrl_endpoint, out_ctrl_endpoint_size);
}

static int csv_contains_token(const char *csv, const char *token) {
  if (!csv || !token || !*token) return 0;
  size_t token_len = strlen(token);
  const char *p = csv;
  while (p && *p) {
    const char *next = strchr(p, ',');
    size_t len = next ? (size_t)(next - p) : strlen(p);
    if (len == token_len && strncmp(p, token, len) == 0) return 1;
    if (!next) break;
    p = next + 1;
  }
  return 0;
}

static char *trim_ascii_ws_inplace(char *s) {
  char *e;
  if (!s) return s;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
  *e = '\0';
  return s;
}

static void csv_append_token(char *csv, size_t csv_size, const char *token) {
  if (!csv || csv_size == 0 || !token || !*token) return;
  if (csv_contains_token(csv, token)) return;

  size_t cur = strlen(csv);
  if (cur > 0) {
    if (cur + 1 >= csv_size) return;
    csv[cur++] = ',';
    csv[cur] = '\0';
  }
  strncat(csv, token, csv_size - strlen(csv) - 1);
}

static void build_sub_role_targets_csv(const char *targets_csv,
                                       char *out_csv, size_t out_csv_size) {
  if (!out_csv || out_csv_size == 0) return;
  out_csv[0] = '\0';
  if (!targets_csv || !targets_csv[0]) return;

  char copy[384] = {0};
  snprintf(copy, sizeof(copy), "%s", targets_csv);

  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, ",", &saveptr);
       tok;
       tok = strtok_r(NULL, ",", &saveptr)) {
    tok = trim_ascii_ws_inplace(tok);
    if (!tok || !tok[0]) continue;
    if (strncmp(tok, "SUB:", 4) == 0) {
      tok += 4;
      tok = trim_ascii_ws_inplace(tok);
      if (!tok || !tok[0]) continue;
    }

    char decorated[256] = {0};
    snprintf(decorated, sizeof(decorated), "SUB:%s", tok);
    csv_append_token(out_csv, out_csv_size, decorated);
  }
}

static void role_append_token(char *role, size_t role_size, const char *token) {
  if (!role || role_size == 0 || !token || !*token) return;
  size_t cur = strlen(role);
  if (cur > 0) {
    if (cur + 1 >= role_size) return;
    role[cur++] = '+';
    role[cur] = '\0';
  }
  strncat(role, token, role_size - strlen(role) - 1);
}

static void build_role_with_sub_targets(const char *base_role, const char *targets_csv,
                                        char *out_role, size_t out_role_size) {
  if (!out_role || out_role_size == 0) return;
  out_role[0] = '\0';

  const char *role = (base_role && base_role[0]) ? base_role : "SUB";
  if (!targets_csv || !*targets_csv) {
    snprintf(out_role, out_role_size, "%s", role);
    return;
  }

  char decorated_targets_csv[384] = {0};
  build_sub_role_targets_csv(targets_csv,
                             decorated_targets_csv, sizeof(decorated_targets_csv));
  if (!decorated_targets_csv[0]) {
    snprintf(out_role, out_role_size, "%s", role);
    return;
  }

  char role_copy[128] = {0};
  snprintf(role_copy, sizeof(role_copy), "%s", role);

  int replaced_sub = 0;
  char *saveptr = NULL;
  for (char *tok = strtok_r(role_copy, "+", &saveptr);
       tok;
       tok = strtok_r(NULL, "+", &saveptr)) {
    if (strcmp(tok, "SUB") == 0) {
      role_append_token(out_role, out_role_size, decorated_targets_csv);
      replaced_sub = 1;
    } else {
      role_append_token(out_role, out_role_size, tok);
    }
  }

  if (!replaced_sub &&
      (strcmp(role, "UNKNOWN") == 0 || strcmp(role, "NONE") == 0)) {
    snprintf(out_role, out_role_size, "%s", decorated_targets_csv);
    return;
  }

  if (out_role[0] == '\0') snprintf(out_role, out_role_size, "%s", role);
}

static int row_is_publisher_like(const names_row_info_t *row) {
  if (!row) return 0;
  if (role_has_pub(row->role)) return 1;
  if (row->pub_port > 0) return 1;
  if (row->pub_bytes >= 0) return 1;
  return 0;
}

static int row_is_subscriber_candidate(const zcm_node_entry_t *entry, const names_row_info_t *row) {
  if (!entry || !row) return 0;
  if (endpoint_is_sub_scheme(entry->endpoint)) return 1;
  if (role_has_sub(row->role)) return 1;

  /* If role inference has no positive sender signal, allow endpoint-match
   * decoration to classify an unknown row as SUB. */
  if ((strcmp(row->role, "EXTERNAL") == 0 || strcmp(row->role, "UNKNOWN") == 0) &&
      !role_has_pub(row->role) && !role_has_push(row->role) && !role_has_pull(row->role) &&
      row->pub_port <= 0 && row->push_port <= 0 &&
      row->pub_bytes < 0 && row->push_bytes < 0 && row->pull_bytes < 0) {
    return 1;
  }

  return 0;
}

static void names_rows_init_display_from_broker(zcm_node_entry_t *entries,
                                                 names_row_info_t *rows,
                                                 size_t count) {
  if (!entries || !rows) return;

  for (size_t i = 0; i < count; i++) {
    char normalized_ep[512] = {0};
    if (endpoint_normalize_for_matching(entries[i].endpoint,
                                        normalized_ep, sizeof(normalized_ep)) == 0) {
      snprintf(rows[i].endpoint_display, sizeof(rows[i].endpoint_display), "%s", normalized_ep);
    } else if (entries[i].endpoint && entries[i].endpoint[0]) {
      snprintf(rows[i].endpoint_display, sizeof(rows[i].endpoint_display), "%s", entries[i].endpoint);
    } else {
      snprintf(rows[i].endpoint_display, sizeof(rows[i].endpoint_display), "-");
    }
    if (rows[i].role[0]) snprintf(rows[i].role_display, sizeof(rows[i].role_display), "%s", rows[i].role);
    else snprintf(rows[i].role_display, sizeof(rows[i].role_display), "UNKNOWN");
  }
}

static void names_rows_apply_subscriber_resolution(zcm_node_entry_t *entries,
                                                   names_row_info_t *rows,
                                                   size_t count) {
  if (!entries || !rows) return;

  names_rows_init_display_from_broker(entries, rows, count);

  for (size_t i = 0; i < count; i++) {
    if (!row_is_subscriber_candidate(&entries[i], &rows[i])) continue;

    const char *sub_match_source =
      (rows[i].ctrl_endpoint[0] ? rows[i].ctrl_endpoint : entries[i].endpoint);
    char resolved_endpoint[512] = {0};
    if (endpoint_normalize_for_matching(sub_match_source,
                                        resolved_endpoint, sizeof(resolved_endpoint)) != 0) {
      continue;
    }

    int resolved_port = -1;
    (void)endpoint_parse_port(resolved_endpoint, &resolved_port);

    char targets_csv[384] = {0};
    for (size_t j = 0; j < count; j++) {
      if (j == i) continue;
      if (!entries[j].name || !entries[j].endpoint) continue;
      if (!row_is_publisher_like(&rows[j])) continue;

      char normalized_j[512] = {0};
      if (endpoint_normalize_for_matching(entries[j].endpoint,
                                          normalized_j, sizeof(normalized_j)) != 0) {
        continue;
      }
      if (strcmp(normalized_j, resolved_endpoint) != 0) continue;

      char target_with_port[256] = {0};
      if (resolved_port > 0) {
        snprintf(target_with_port, sizeof(target_with_port), "%s:%d",
                 entries[j].name, resolved_port);
      } else {
        snprintf(target_with_port, sizeof(target_with_port), "%s",
                 entries[j].name);
      }
      csv_append_token(targets_csv, sizeof(targets_csv), target_with_port);
    }

    /* Legacy SUB registrations may advertise local sub://host:port, while
     * publishers are registered on remote tcp://host:port. If exact endpoint
     * matching fails, fall back to unique publisher port matching. */
    if (!targets_csv[0] && resolved_port > 0) {
      char by_port_target[256] = {0};
      int by_port_matches = 0;
      for (size_t j = 0; j < count; j++) {
        if (j == i) continue;
        if (!entries[j].name || !entries[j].name[0]) continue;
        if (!entries[j].endpoint || !entries[j].endpoint[0]) continue;
        if (!row_is_publisher_like(&rows[j])) continue;

        int pub_port = rows[j].pub_port;
        if (pub_port <= 0) {
          (void)endpoint_parse_port(entries[j].endpoint, &pub_port);
        }
        if (pub_port != resolved_port) continue;

        char candidate[256] = {0};
        snprintf(candidate, sizeof(candidate), "%s:%d", entries[j].name, resolved_port);
        if (by_port_matches == 0) {
          snprintf(by_port_target, sizeof(by_port_target), "%s", candidate);
          by_port_matches = 1;
        } else if (strcmp(by_port_target, candidate) != 0) {
          by_port_matches = 2;
          break;
        }
      }
      if (by_port_matches == 1 && by_port_target[0]) {
        csv_append_token(targets_csv, sizeof(targets_csv), by_port_target);
      }
    }

    if (targets_csv[0]) {
      build_role_with_sub_targets(rows[i].role, targets_csv,
                                  rows[i].role_display, sizeof(rows[i].role_display));
    }
  }
}

static int parse_sub_targets_reply(const char *reply_text,
                                   char *out_targets_csv,
                                   size_t out_targets_csv_size) {
  if (!reply_text || !out_targets_csv || out_targets_csv_size == 0) return -1;
  out_targets_csv[0] = '\0';

  char copy[512] = {0};
  snprintf(copy, sizeof(copy), "%s", reply_text);

  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, ",;", &saveptr);
       tok;
       tok = strtok_r(NULL, ",;", &saveptr)) {
    tok = trim_ascii_ws_inplace(tok);
    if (!tok || !tok[0]) continue;
    if (strncmp(tok, "SUB:", 4) == 0) {
      tok += 4;
      tok = trim_ascii_ws_inplace(tok);
      if (!tok || !tok[0]) continue;
    }
    csv_append_token(out_targets_csv, out_targets_csv_size, tok);
  }

  return (out_targets_csv[0] ? 0 : -1);
}

static int parse_sub_target_bytes_reply(const char *reply_text,
                                        char *out_target_bytes_csv,
                                        size_t out_target_bytes_csv_size) {
  if (!reply_text || !out_target_bytes_csv || out_target_bytes_csv_size == 0) return -1;
  out_target_bytes_csv[0] = '\0';

  char copy[1024] = {0};
  snprintf(copy, sizeof(copy), "%s", reply_text);

  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, ",;", &saveptr);
       tok;
       tok = strtok_r(NULL, ",;", &saveptr)) {
    tok = trim_ascii_ws_inplace(tok);
    if (!tok || !tok[0]) continue;
    if (strncmp(tok, "SUB:", 4) == 0) {
      tok += 4;
      tok = trim_ascii_ws_inplace(tok);
      if (!tok || !tok[0]) continue;
    }

    char *eq = strchr(tok, '=');
    if (!eq || eq == tok || !eq[1]) continue;
    *eq = '\0';

    char *key = trim_ascii_ws_inplace(tok);
    char *value = trim_ascii_ws_inplace(eq + 1);
    int bytes = -1;
    if (!key || !key[0]) continue;
    if (parse_int_reply(value, &bytes) != 0 || bytes < 0) continue;

    char item[320] = {0};
    snprintf(item, sizeof(item), "%s=%d", key, bytes);
    csv_append_token(out_target_bytes_csv, out_target_bytes_csv_size, item);
  }

  return (out_target_bytes_csv[0] ? 0 : -1);
}

static void names_rows_apply_subscriber_targets_query(zcm_context_t *ctx,
                                                      zcm_node_t *node,
                                                      zcm_node_entry_t *entries,
                                                      names_row_info_t *rows,
                                                      size_t count) {
  if (!ctx || !node || !entries || !rows) return;

  for (size_t i = 0; i < count; i++) {
    if (!row_is_subscriber_candidate(&entries[i], &rows[i])) continue;
    if (!entries[i].name || !entries[i].name[0]) continue;

    const char *query_ep = NULL;
    if (endpoint_is_queryable(rows[i].endpoint_display)) {
      query_ep = rows[i].endpoint_display;
    } else if (entries[i].endpoint && endpoint_is_queryable(entries[i].endpoint)) {
      query_ep = entries[i].endpoint;
    }
    if (!query_ep || !query_ep[0]) continue;

    char reply_text[512] = {0};
    int code = 0;
    int ok = 0;
    int attempts = names_query_attempts();
    for (int attempt = 0; attempt < attempts; attempt++) {
      if (query_proc_command_with_ctrl_fallback(ctx, node, entries[i].name, query_ep,
                                                "DATA_SUB_TARGETS",
                                                reply_text, sizeof(reply_text), &code) == 0 &&
          code == 200 && reply_text[0]) {
        ok = 1;
        break;
      }
      if (attempt + 1 < attempts) usleep(20 * 1000);
    }
    if (!ok) {
      continue;
    }

    char targets_csv[384] = {0};
    if (parse_sub_targets_reply(reply_text,
                                targets_csv, sizeof(targets_csv)) != 0) {
      continue;
    }

    snprintf(rows[i].sub_targets_csv, sizeof(rows[i].sub_targets_csv), "%s", targets_csv);
    build_role_with_sub_targets(rows[i].role, targets_csv,
                                rows[i].role_display, sizeof(rows[i].role_display));
  }
}

static void names_rows_apply_subscriber_target_bytes_query(zcm_context_t *ctx,
                                                           zcm_node_t *node,
                                                           zcm_node_entry_t *entries,
                                                           names_row_info_t *rows,
                                                           size_t count) {
  if (!ctx || !node || !entries || !rows) return;

  for (size_t i = 0; i < count; i++) {
    if (!row_is_subscriber_candidate(&entries[i], &rows[i])) continue;
    if (!entries[i].name || !entries[i].name[0]) continue;
    if (rows[i].sub_target_bytes_csv[0]) continue;

    const char *query_ep = NULL;
    if (endpoint_is_queryable(rows[i].endpoint_display)) {
      query_ep = rows[i].endpoint_display;
    } else if (entries[i].endpoint && endpoint_is_queryable(entries[i].endpoint)) {
      query_ep = entries[i].endpoint;
    }
    if (!query_ep || !query_ep[0]) continue;

    char reply_text[1024] = {0};
    int code = 0;
    int ok = 0;
    int attempts = names_query_attempts();
    for (int attempt = 0; attempt < attempts; attempt++) {
      if (query_proc_command_with_ctrl_fallback(ctx, node, entries[i].name, query_ep,
                                                "DATA_PAYLOAD_BYTES_SUB_TARGETS",
                                                reply_text, sizeof(reply_text), &code) == 0 &&
          code == 200 && reply_text[0]) {
        ok = 1;
        break;
      }
      if (attempt + 1 < attempts) usleep(20 * 1000);
    }
    if (!ok) {
      continue;
    }

    (void)parse_sub_target_bytes_reply(reply_text,
                                       rows[i].sub_target_bytes_csv,
                                       sizeof(rows[i].sub_target_bytes_csv));
  }
}

static void names_rows_apply_subscriber_info_overrides(zcm_node_t *node,
                                                       zcm_node_entry_t *entries,
                                                       names_row_info_t *rows,
                                                       size_t count) {
  if (!node || !entries || !rows) return;

  for (size_t i = 0; i < count; i++) {
    if (!row_is_subscriber_candidate(&entries[i], &rows[i])) continue;
    if (!entries[i].name || !entries[i].name[0]) continue;

    char info_ep[512] = {0};
    char info_ctrl_ep[512] = {0};
    char info_host[256] = {0};
    int info_pid = 0;
    if (zcm_node_info(node, entries[i].name,
                      info_ep, sizeof(info_ep),
                      info_ctrl_ep, sizeof(info_ctrl_ep),
                      info_host, sizeof(info_host),
                      &info_pid) != 0) {
      continue;
    }
    (void)info_pid;

    if (info_ep[0]) {
      snprintf(rows[i].ctrl_endpoint, sizeof(rows[i].ctrl_endpoint), "%s", info_ep);
    }

    if (info_host[0]) {
      snprintf(rows[i].host, sizeof(rows[i].host), "%s", info_host);
      resolve_hostname_if_ip(rows[i].host, sizeof(rows[i].host));
    }

    char inferred_ctrl_ep[512] = {0};
    const char *ep_for_infer = info_ep[0] ? info_ep : entries[i].endpoint;
    int have_inferred_ctrl = (infer_ctrl_endpoint_from_data(ep_for_infer, info_host,
                                                            inferred_ctrl_ep, sizeof(inferred_ctrl_ep)) == 0);

    int use_info_ctrl = (info_ctrl_ep[0] && endpoint_is_queryable(info_ctrl_ep));
    if (use_info_ctrl && info_ep[0]) {
      if (strcmp(info_ctrl_ep, info_ep) == 0) {
        use_info_ctrl = 0;
      } else {
        int ctrl_port = -1;
        int data_port = -1;
        if (endpoint_parse_port(info_ctrl_ep, &ctrl_port) == 0 &&
            endpoint_parse_port(info_ep, &data_port) == 0 &&
            ctrl_port == data_port) {
          use_info_ctrl = 0;
        }
      }
    }

    const char *display_ep = NULL;
    if (use_info_ctrl) {
      display_ep = info_ctrl_ep;
    } else if (have_inferred_ctrl) {
      display_ep = inferred_ctrl_ep;
    } else if (info_ep[0] && endpoint_is_queryable(info_ep) &&
               !endpoint_is_sub_scheme(info_ep)) {
      display_ep = info_ep;
    }
    if (display_ep) {
      char normalized_ep[512] = {0};
      if (endpoint_normalize_for_matching(display_ep,
                                          normalized_ep, sizeof(normalized_ep)) == 0) {
        snprintf(rows[i].endpoint_display, sizeof(rows[i].endpoint_display), "%s", normalized_ep);
      } else {
        snprintf(rows[i].endpoint_display, sizeof(rows[i].endpoint_display), "%s", display_ep);
      }
    }
  }
}

static size_t role_collect_sub_variants(const char *role_display,
                                        char out_variants[][512],
                                        size_t out_variants_cap) {
  if (!role_display || !role_display[0] ||
      !out_variants || out_variants_cap == 0) return 0;
  if (!strchr(role_display, ',')) return 0;

  char copy[512] = {0};
  snprintf(copy, sizeof(copy), "%s", role_display);

  size_t count = 0;
  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, ",", &saveptr);
       tok;
       tok = strtok_r(NULL, ",", &saveptr)) {
    tok = trim_ascii_ws_inplace(tok);
    if (!tok || !tok[0]) continue;
    if (strncmp(tok, "SUB:", 4) != 0 || !tok[4]) return 0;
    if (count >= out_variants_cap) return 0;
    snprintf(out_variants[count], 512, "%s", tok);
    count++;
  }

  if (count < 2) return 0;
  return count;
}

static int parse_sub_role_variant(const char *variant,
                                  char *out_pub_name, size_t out_pub_name_size,
                                  int *out_pub_port) {
  if (!variant || !out_pub_name || out_pub_name_size == 0 || !out_pub_port) return -1;
  out_pub_name[0] = '\0';
  *out_pub_port = -1;

  if (strncmp(variant, "SUB:", 4) != 0 || !variant[4]) return -1;
  const char *payload = variant + 4;
  const char *last_colon = strrchr(payload, ':');
  if (last_colon && last_colon[1]) {
    char *endptr = NULL;
    long v = strtol(last_colon + 1, &endptr, 10);
    if (endptr && *endptr == '\0' && v >= 1 && v <= 65535) {
      size_t name_len = (size_t)(last_colon - payload);
      if (name_len >= out_pub_name_size) name_len = out_pub_name_size - 1;
      memcpy(out_pub_name, payload, name_len);
      out_pub_name[name_len] = '\0';
      *out_pub_port = (int)v;
      return (out_pub_name[0] ? 0 : -1);
    }
  }

  snprintf(out_pub_name, out_pub_name_size, "%s", payload);
  return 0;
}

static int find_pub_bytes_for_sub_variant(zcm_node_entry_t *entries,
                                          names_row_info_t *rows,
                                          size_t count,
                                          const char *variant) {
  char pub_name[256] = {0};
  int pub_port = -1;
  if (!entries || !rows || !variant) return -1;
  if (parse_sub_role_variant(variant, pub_name, sizeof(pub_name), &pub_port) != 0) return -1;

  for (size_t j = 0; j < count; j++) {
    if (!entries[j].name) continue;
    if (strcmp(entries[j].name, pub_name) != 0) continue;
    if (pub_port > 0 && rows[j].pub_port > 0 && rows[j].pub_port != pub_port) continue;
    if (rows[j].pub_bytes >= 0) return rows[j].pub_bytes;
  }
  return -1;
}

static int find_sub_target_bytes_for_variant(const char *sub_target_bytes_csv,
                                             const char *variant) {
  char pub_name[256] = {0};
  int pub_port = -1;
  int fallback = -1;
  if (!sub_target_bytes_csv || !sub_target_bytes_csv[0] || !variant) return -1;
  if (parse_sub_role_variant(variant, pub_name, sizeof(pub_name), &pub_port) != 0) return -1;

  char copy[1024] = {0};
  snprintf(copy, sizeof(copy), "%s", sub_target_bytes_csv);

  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, ",;", &saveptr);
       tok;
       tok = strtok_r(NULL, ",;", &saveptr)) {
    tok = trim_ascii_ws_inplace(tok);
    if (!tok || !tok[0]) continue;

    char *eq = strchr(tok, '=');
    if (!eq || eq == tok || !eq[1]) continue;
    *eq = '\0';

    char *key = trim_ascii_ws_inplace(tok);
    char *value = trim_ascii_ws_inplace(eq + 1);
    int bytes = -1;
    if (!key || !key[0]) continue;
    if (parse_int_reply(value, &bytes) != 0 || bytes < 0) continue;

    char decorated[320] = {0};
    char key_name[256] = {0};
    int key_port = -1;
    snprintf(decorated, sizeof(decorated), "SUB:%s", key);
    if (parse_sub_role_variant(decorated, key_name, sizeof(key_name), &key_port) != 0) continue;
    if (strcmp(key_name, pub_name) != 0) continue;

    if (pub_port > 0 && key_port == pub_port) return bytes;
    if (pub_port <= 0 || key_port <= 0) fallback = bytes;
  }
  return fallback;
}

static int role_extract_first_sub_variant(const char *role_display,
                                          char *out_variant, size_t out_variant_size) {
  if (!role_display || !out_variant || out_variant_size == 0) return -1;
  out_variant[0] = '\0';

  const char *p = strstr(role_display, "SUB:");
  if (!p) return -1;
  const char *end = p;
  while (*end && *end != ',' && *end != '+') end++;
  size_t n = (size_t)(end - p);
  if (n == 0) return -1;
  if (n >= out_variant_size) n = out_variant_size - 1;
  memcpy(out_variant, p, n);
  out_variant[n] = '\0';
  return 0;
}

static void names_rows_apply_subscriber_endpoint_corrections(zcm_node_entry_t *entries,
                                                             names_row_info_t *rows,
                                                             size_t count) {
  if (!entries || !rows) return;

  for (size_t i = 0; i < count; i++) {
    if (!row_is_subscriber_candidate(&entries[i], &rows[i])) continue;
    if (!rows[i].role_display[0]) continue;
    if (strchr(rows[i].role_display, ',')) continue;

    char sub_variant[256] = {0};
    char pub_name[256] = {0};
    int pub_port = -1;
    if (role_extract_first_sub_variant(rows[i].role_display,
                                       sub_variant, sizeof(sub_variant)) != 0) continue;
    if (parse_sub_role_variant(sub_variant,
                               pub_name, sizeof(pub_name), &pub_port) != 0) continue;
    if (pub_port <= 0 || pub_port >= 65535) continue;

    int ep_port = -1;
    char ep_host[256] = {0};
    (void)endpoint_parse_port(rows[i].endpoint_display, &ep_port);
    (void)endpoint_parse_host(rows[i].endpoint_display, ep_host, sizeof(ep_host));

    char pub_host[256] = {0};
    for (size_t j = 0; j < count; j++) {
      if (!entries[j].name || strcmp(entries[j].name, pub_name) != 0) continue;
      if (rows[j].host[0] && strcmp(rows[j].host, "-") != 0) {
        snprintf(pub_host, sizeof(pub_host), "%s", rows[j].host);
      } else {
        (void)endpoint_parse_host(rows[j].endpoint_display, pub_host, sizeof(pub_host));
      }
      break;
    }

    int should_rewrite = 0;
    if (ep_port == pub_port) should_rewrite = 1;
    if (ep_port == (pub_port + 1) &&
        rows[i].host[0] && strcmp(rows[i].host, "-") != 0 &&
        ep_host[0] && strcasecmp(ep_host, rows[i].host) != 0) {
      should_rewrite = 1;
    }
    if (!should_rewrite) continue;
    if (!host_is_connectable(rows[i].host)) continue;

    char corrected_ep[512] = {0};
    if (build_tcp_endpoint_text(rows[i].host, pub_port + 1,
                                corrected_ep, sizeof(corrected_ep)) != 0) {
      continue;
    }
    snprintf(rows[i].endpoint_display, sizeof(rows[i].endpoint_display), "%s", corrected_ep);
  }
}

static void names_rows_apply_broker_endpoint_dns(zcm_node_entry_t *entries,
                                                 names_row_info_t *rows,
                                                 size_t count) {
  if (!entries || !rows) return;
  for (size_t i = 0; i < count; i++) {
    if (!entries[i].name) continue;
    if (strcmp(entries[i].name, "zcmbroker") != 0) continue;
    endpoint_resolve_hostname_if_ip(rows[i].endpoint_display,
                                    sizeof(rows[i].endpoint_display));
  }
}

static void print_repeat_char(char ch, size_t n);
static void format_int_or_dash(int value, char *out, size_t out_size);

static void names_print_table(zcm_node_entry_t *entries,
                              names_row_info_t *rows,
                              size_t count) {
  if (!entries || !rows) return;

  size_t w_name = strlen("NAME");
  size_t w_endpoint = strlen("REQ/REP ENDPOINT");
  size_t w_role = strlen("ROLE");
  size_t w_pub_port = strlen("PUB_PORT");
  size_t w_push_port = strlen("PUSH_PORT");
  size_t w_pub_bytes = strlen("PUB_BYTES");
  size_t w_sub_bytes = strlen("SUB_BYTES");
  size_t w_push_bytes = strlen("PUSH_BYTES");
  size_t w_pull_bytes = strlen("PULL_BYTES");

  for (size_t i = 0; i < count; i++) {
    size_t name_len = strlen(entries[i].name);
    if (name_len > w_name) w_name = name_len;
    size_t ep_len = strlen(rows[i].endpoint_display);
    if (ep_len > w_endpoint) w_endpoint = ep_len;

    char role_variants[32][512];
    size_t role_variant_count = role_collect_sub_variants(
      rows[i].role_display,
      role_variants,
      sizeof(role_variants) / sizeof(role_variants[0]));
    if (role_variant_count > 0) {
      for (size_t r = 0; r < role_variant_count; r++) {
        size_t role_len = strlen(role_variants[r]);
        if (role_len > w_role) w_role = role_len;
        char num_text[16];
        int sub_bytes_variant = find_sub_target_bytes_for_variant(rows[i].sub_target_bytes_csv,
                                                                  role_variants[r]);
        if (sub_bytes_variant < 0) {
          sub_bytes_variant = find_pub_bytes_for_sub_variant(entries, rows, count, role_variants[r]);
        }
        format_int_or_dash((sub_bytes_variant >= 0) ? sub_bytes_variant : rows[i].sub_bytes,
                           num_text, sizeof(num_text));
        if (strlen(num_text) > w_sub_bytes) w_sub_bytes = strlen(num_text);
      }
    } else {
      size_t role_len = strlen(rows[i].role_display);
      if (role_len > w_role) w_role = role_len;
    }

    char num_text[16];
    format_int_or_dash((rows[i].pub_port > 0) ? rows[i].pub_port : -1,
                       num_text, sizeof(num_text));
    if (strlen(num_text) > w_pub_port) w_pub_port = strlen(num_text);
    format_int_or_dash((rows[i].push_port > 0) ? rows[i].push_port : -1,
                       num_text, sizeof(num_text));
    if (strlen(num_text) > w_push_port) w_push_port = strlen(num_text);
    format_int_or_dash(rows[i].pub_bytes, num_text, sizeof(num_text));
    if (strlen(num_text) > w_pub_bytes) w_pub_bytes = strlen(num_text);
    format_int_or_dash(rows[i].sub_bytes, num_text, sizeof(num_text));
    if (strlen(num_text) > w_sub_bytes) w_sub_bytes = strlen(num_text);
    format_int_or_dash(rows[i].push_bytes, num_text, sizeof(num_text));
    if (strlen(num_text) > w_push_bytes) w_push_bytes = strlen(num_text);
    format_int_or_dash(rows[i].pull_bytes, num_text, sizeof(num_text));
    if (strlen(num_text) > w_pull_bytes) w_pull_bytes = strlen(num_text);
  }

  printf("%-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s\n",
         (int)w_name, "NAME",
         (int)w_endpoint, "REQ/REP ENDPOINT",
         (int)w_role, "ROLE",
         (int)w_pub_port, "PUB_PORT",
         (int)w_push_port, "PUSH_PORT",
         (int)w_pub_bytes, "PUB_BYTES",
         (int)w_sub_bytes, "SUB_BYTES",
         (int)w_push_bytes, "PUSH_BYTES",
         (int)w_pull_bytes, "PULL_BYTES");
  print_repeat_char('-', w_name);
  printf("  ");
  print_repeat_char('-', w_endpoint);
  printf("  ");
  print_repeat_char('-', w_role);
  printf("  ");
  print_repeat_char('-', w_pub_port);
  printf("  ");
  print_repeat_char('-', w_push_port);
  printf("  ");
  print_repeat_char('-', w_pub_bytes);
  printf("  ");
  print_repeat_char('-', w_sub_bytes);
  printf("  ");
  print_repeat_char('-', w_push_bytes);
  printf("  ");
  print_repeat_char('-', w_pull_bytes);
  printf("\n");

  for (size_t i = 0; i < count; i++) {
    char pub_port_text[16];
    char push_port_text[16];
    char pub_bytes_text[16];
    char sub_bytes_text[16];
    char push_bytes_text[16];
    char pull_bytes_text[16];
    format_int_or_dash((rows[i].pub_port > 0) ? rows[i].pub_port : -1,
                       pub_port_text, sizeof(pub_port_text));
    format_int_or_dash((rows[i].push_port > 0) ? rows[i].push_port : -1,
                       push_port_text, sizeof(push_port_text));
    format_int_or_dash(rows[i].pub_bytes, pub_bytes_text, sizeof(pub_bytes_text));
    format_int_or_dash(rows[i].sub_bytes, sub_bytes_text, sizeof(sub_bytes_text));
    format_int_or_dash(rows[i].push_bytes, push_bytes_text, sizeof(push_bytes_text));
    format_int_or_dash(rows[i].pull_bytes, pull_bytes_text, sizeof(pull_bytes_text));

    char role_variants[32][512];
    size_t role_variant_count = role_collect_sub_variants(
      rows[i].role_display,
      role_variants,
      sizeof(role_variants) / sizeof(role_variants[0]));
    if (role_variant_count > 0) {
      for (size_t r = 0; r < role_variant_count; r++) {
        char variant_sub_bytes_text[16];
        int sub_bytes_variant = find_sub_target_bytes_for_variant(rows[i].sub_target_bytes_csv,
                                                                  role_variants[r]);
        if (sub_bytes_variant < 0) {
          sub_bytes_variant = find_pub_bytes_for_sub_variant(entries, rows, count, role_variants[r]);
        }
        format_int_or_dash((sub_bytes_variant >= 0) ? sub_bytes_variant : rows[i].sub_bytes,
                           variant_sub_bytes_text, sizeof(variant_sub_bytes_text));

        const char *name_text = (r == 0) ? entries[i].name : "";
        const char *endpoint_text = (r == 0) ? rows[i].endpoint_display : "";
        printf("%-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s\n",
               (int)w_name, name_text,
               (int)w_endpoint, endpoint_text,
               (int)w_role, role_variants[r],
               (int)w_pub_port, pub_port_text,
               (int)w_push_port, push_port_text,
               (int)w_pub_bytes, pub_bytes_text,
               (int)w_sub_bytes, variant_sub_bytes_text,
               (int)w_push_bytes, push_bytes_text,
               (int)w_pull_bytes, pull_bytes_text);
      }
    } else {
      printf("%-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s\n",
             (int)w_name, entries[i].name,
             (int)w_endpoint, rows[i].endpoint_display,
             (int)w_role, rows[i].role_display,
             (int)w_pub_port, pub_port_text,
             (int)w_push_port, push_port_text,
             (int)w_pub_bytes, pub_bytes_text,
             (int)w_sub_bytes, sub_bytes_text,
             (int)w_push_bytes, push_bytes_text,
             (int)w_pull_bytes, pull_bytes_text);
    }
  }
}

static void print_repeat_char(char ch, size_t n) {
  for (size_t i = 0; i < n; i++) putchar(ch);
}

static void format_int_or_dash(int value, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  if (value >= 0) snprintf(out, out_size, "%d", value);
  else snprintf(out, out_size, "-");
}

static int recv_text_frame(void *sock, char *out, size_t out_size) {
  if (!sock || !out || out_size == 0) return -1;
  int n = zmq_recv(sock, out, out_size - 1, 0);
  if (n < 0) return -1;
  out[n] = '\0';
  return 0;
}

static int do_names_broker_ex(const char *endpoint) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;
  void *req = NULL;
  zcm_node_entry_t *entries = NULL;
  names_row_info_t *rows = NULL;
  size_t count = 0;

  if (!ctx || !endpoint || !*endpoint) goto out;
  node = zcm_node_new(ctx, endpoint);
  req = zmq_socket(zcm_context_zmq(ctx), ZMQ_REQ);
  if (!req) goto out;

  int timeout_ms = 5000;
  int linger = 0;
  int immediate = 0;
  zmq_setsockopt(req, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
  zmq_setsockopt(req, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
  zmq_setsockopt(req, ZMQ_LINGER, &linger, sizeof(linger));
  zmq_setsockopt(req, ZMQ_IMMEDIATE, &immediate, sizeof(immediate));

  if (zmq_connect(req, endpoint) != 0) goto out;
  if (zmq_send(req, "LIST_EX", 7, 0) < 0) goto out;

  char status[16] = {0};
  if (recv_text_frame(req, status, sizeof(status)) != 0) goto out;
  if (strcmp(status, "OK") != 0) goto out;

  int raw_count = 0;
  int n = zmq_recv(req, &raw_count, sizeof(raw_count), 0);
  if (n != (int)sizeof(raw_count) || raw_count < 0 || raw_count > 100000) goto out;
  count = (size_t)raw_count;

  if (count > 0) {
    entries = (zcm_node_entry_t *)calloc(count, sizeof(*entries));
    rows = (names_row_info_t *)calloc(count, sizeof(*rows));
    if (!entries || !rows) goto out;
  }

  for (size_t i = 0; i < count; i++) {
    char name[256] = {0};
    char ep[512] = {0};
    char host[256] = {0};
    char role[512] = {0};
    char pub_port[32] = {0};
    char push_port[32] = {0};
    char pub_bytes[32] = {0};
    char sub_bytes[32] = {0};
    char push_bytes[32] = {0};
    char pull_bytes[32] = {0};
    if (recv_text_frame(req, name, sizeof(name)) != 0) goto out;
    if (recv_text_frame(req, ep, sizeof(ep)) != 0) goto out;
    if (recv_text_frame(req, host, sizeof(host)) != 0) goto out;
    if (recv_text_frame(req, role, sizeof(role)) != 0) goto out;
    if (recv_text_frame(req, pub_port, sizeof(pub_port)) != 0) goto out;
    if (recv_text_frame(req, push_port, sizeof(push_port)) != 0) goto out;
    if (recv_text_frame(req, pub_bytes, sizeof(pub_bytes)) != 0) goto out;
    if (recv_text_frame(req, sub_bytes, sizeof(sub_bytes)) != 0) goto out;
    if (recv_text_frame(req, push_bytes, sizeof(push_bytes)) != 0) goto out;
    if (recv_text_frame(req, pull_bytes, sizeof(pull_bytes)) != 0) goto out;

    entries[i].name = strdup(name);
    entries[i].endpoint = strdup(ep);
    if (!entries[i].name || !entries[i].endpoint) goto out;

    if (host[0]) {
      snprintf(rows[i].host, sizeof(rows[i].host), "%s", host);
    } else {
      extract_host_from_endpoint(ep, rows[i].host, sizeof(rows[i].host));
    }
    resolve_hostname_if_ip(rows[i].host, sizeof(rows[i].host));

    if (role[0]) snprintf(rows[i].role, sizeof(rows[i].role), "%s", role);
    else snprintf(rows[i].role, sizeof(rows[i].role), "UNKNOWN");
    rows[i].ctrl_endpoint[0] = '\0';

    rows[i].pub_port = -1;
    rows[i].push_port = -1;
    rows[i].pub_bytes = -1;
    rows[i].sub_bytes = -1;
    rows[i].push_bytes = -1;
    rows[i].pull_bytes = -1;
    (void)parse_int_reply(pub_port, &rows[i].pub_port);
    (void)parse_int_reply(push_port, &rows[i].push_port);
    /* Bytes are runtime values: always sourced from per-node DATA_METRICS,
     * never from broker LIST_EX cache. */
  }

  names_rows_init_display_from_broker(entries, rows, count);
  for (size_t i = 0; i < count; i++) {
    if (!entries[i].name || !entries[i].endpoint) continue;
    (void)query_node_metrics_snapshot(ctx, node, entries[i].name,
                                      entries[i].endpoint, &rows[i]);
  }
  if (node) {
    names_rows_apply_subscriber_info_overrides(node, entries, rows, count);
    names_rows_apply_subscriber_resolution(entries, rows, count);
    names_rows_apply_subscriber_endpoint_corrections(entries, rows, count);
    names_rows_apply_subscriber_targets_query(ctx, node, entries, rows, count);
    names_rows_apply_subscriber_target_bytes_query(ctx, node, entries, rows, count);
  } else {
    names_rows_apply_subscriber_resolution(entries, rows, count);
  }
  for (size_t i = 0; i < count; i++) {
    if (rows[i].sub_targets_csv[0]) {
      build_role_with_sub_targets(rows[i].role, rows[i].sub_targets_csv,
                                  rows[i].role_display, sizeof(rows[i].role_display));
    }
  }
  names_rows_apply_broker_endpoint_dns(entries, rows, count);
  names_print_table(entries, rows, count);

  rc = 0;

out:
  if (entries) zcm_node_list_free(entries, count);
  free(rows);
  if (req) zmq_close(req);
  if (node) zcm_node_free(node);
  if (ctx) zcm_context_free(ctx);
  return rc;
}

static int do_names(const char *endpoint) {
  return do_names_broker_ex(endpoint);
}

static int do_names_with_timeout(const char *endpoint, int timeout_ms, int report_error) {
  (void)timeout_ms;
  int rc = do_names(endpoint);
  if (rc != 0 && report_error) {
    fprintf(stderr, "zcm: broker not reachable\n");
  }
  return rc;
}

static int do_names_with_retry(const char *endpoint, int timeout_ms, int attempts) {
  if (attempts <= 0) attempts = 1;
  for (int i = 0; i < attempts; i++) {
    int rc = do_names_with_timeout(endpoint, timeout_ms, 0);
    if (rc == 0) return 0;
    if (i + 1 < attempts) usleep(200 * 1000);
  }
  fprintf(stderr, "zcm: broker not reachable\n");
  return 1;
}

typedef enum {
  SEND_VALUE_NONE = 0,
  SEND_VALUE_TEXT = 1,
  SEND_VALUE_DOUBLE = 2,
  SEND_VALUE_FLOAT = 3,
  SEND_VALUE_INT = 4,
  SEND_VALUE_CHAR = 5,
  SEND_VALUE_SHORT = 6,
  SEND_VALUE_LONG = 7,
  SEND_VALUE_BYTES = 8,
  SEND_VALUE_ARRAY = 9
} send_value_kind_t;

typedef struct send_value {
  send_value_kind_t kind;
  const char *value;
} send_value_t;

#define SEND_VALUE_MAX 64

static int parse_int32_str(const char *text, int32_t *out) {
  if (!text || !out) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < INT32_MIN || v > INT32_MAX) return -1;
  *out = (int32_t)v;
  return 0;
}

static int parse_int16_str(const char *text, int16_t *out) {
  if (!text || !out) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < INT16_MIN || v > INT16_MAX) return -1;
  *out = (int16_t)v;
  return 0;
}

static int parse_int64_str(const char *text, int64_t *out) {
  if (!text || !out) return -1;
  char *end = NULL;
  errno = 0;
  long long v = strtoll(text, &end, 10);
  if (!end || *end != '\0' || errno == ERANGE) return -1;
  *out = (int64_t)v;
  return 0;
}

static int parse_char_str(const char *text, char *out) {
  if (!text || !out) return -1;
  size_t n = strlen(text);
  if (n == 1) {
    *out = text[0];
    return 0;
  }

  int32_t iv = 0;
  if (parse_int32_str(text, &iv) != 0) return -1;
  if (iv < -128 || iv > 127) return -1;
  *out = (char)iv;
  return 0;
}

static int parse_float_str(const char *text, float *out) {
  if (!text || !out) return -1;
  char *end = NULL;
  float v = strtof(text, &end);
  if (!end || *end != '\0') return -1;
  *out = v;
  return 0;
}

static int parse_double_str(const char *text, double *out) {
  if (!text || !out) return -1;
  char *end = NULL;
  double v = strtod(text, &end);
  if (!end || *end != '\0') return -1;
  *out = v;
  return 0;
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

static int parse_array_type(const char *text, zcm_msg_array_type_t *out) {
  if (!text || !out) return -1;
  if (strcasecmp(text, "char") == 0) {
    *out = ZCM_MSG_ARRAY_CHAR;
    return 0;
  }
  if (strcasecmp(text, "short") == 0) {
    *out = ZCM_MSG_ARRAY_SHORT;
    return 0;
  }
  if (strcasecmp(text, "int") == 0) {
    *out = ZCM_MSG_ARRAY_INT;
    return 0;
  }
  if (strcasecmp(text, "float") == 0) {
    *out = ZCM_MSG_ARRAY_FLOAT;
    return 0;
  }
  if (strcasecmp(text, "double") == 0) {
    *out = ZCM_MSG_ARRAY_DOUBLE;
    return 0;
  }
  return -1;
}

static size_t array_elem_size(zcm_msg_array_type_t type) {
  switch (type) {
    case ZCM_MSG_ARRAY_CHAR: return 1;
    case ZCM_MSG_ARRAY_SHORT: return 2;
    case ZCM_MSG_ARRAY_INT: return 4;
    case ZCM_MSG_ARRAY_FLOAT: return 4;
    case ZCM_MSG_ARRAY_DOUBLE: return 8;
    default: return 0;
  }
}

static int grow_array_buf(void **buf, size_t elem_size, size_t *cap, size_t need) {
  if (!buf || !cap || elem_size == 0) return -1;
  if (need <= *cap) return 0;
  size_t new_cap = (*cap == 0) ? 8 : *cap;
  while (new_cap < need) new_cap *= 2;
  void *next = realloc(*buf, new_cap * elem_size);
  if (!next) return -1;
  *buf = next;
  *cap = new_cap;
  return 0;
}

static int set_payload_array(zcm_msg_t *msg, const char *spec) {
  if (!msg || !spec) return -1;

  const char *sep = strchr(spec, ':');
  if (!sep) return -1;

  char type_text[32] = {0};
  size_t type_len = (size_t)(sep - spec);
  if (type_len == 0 || type_len >= sizeof(type_text)) return -1;
  memcpy(type_text, spec, type_len);
  type_text[type_len] = '\0';
  trim_ws_inplace(type_text);

  zcm_msg_array_type_t array_type;
  if (parse_array_type(type_text, &array_type) != 0) return -1;

  const char *items_text = sep + 1;
  while (*items_text && isspace((unsigned char)*items_text)) items_text++;
  if (*items_text == '\0') {
    return zcm_msg_put_array(msg, array_type, 0, NULL);
  }

  char *list = strdup(items_text);
  if (!list) return -1;

  size_t cap = 0;
  size_t count = 0;
  void *items = NULL;
  size_t elem_size = array_elem_size(array_type);
  if (elem_size == 0) {
    free(list);
    return -1;
  }

  int rc = -1;
  char *saveptr = NULL;
  for (char *tok = strtok_r(list, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
    trim_ws_inplace(tok);
    if (tok[0] == '\0') goto out;
    if (grow_array_buf(&items, elem_size, &cap, count + 1) != 0) goto out;

    switch (array_type) {
      case ZCM_MSG_ARRAY_CHAR: {
        char v = 0;
        if (parse_char_str(tok, &v) != 0) goto out;
        ((char *)items)[count] = v;
        break;
      }
      case ZCM_MSG_ARRAY_SHORT: {
        int16_t v = 0;
        if (parse_int16_str(tok, &v) != 0) goto out;
        ((int16_t *)items)[count] = v;
        break;
      }
      case ZCM_MSG_ARRAY_INT: {
        int32_t v = 0;
        if (parse_int32_str(tok, &v) != 0) goto out;
        ((int32_t *)items)[count] = v;
        break;
      }
      case ZCM_MSG_ARRAY_FLOAT: {
        float v = 0.0f;
        if (parse_float_str(tok, &v) != 0) goto out;
        ((float *)items)[count] = v;
        break;
      }
      case ZCM_MSG_ARRAY_DOUBLE: {
        double v = 0.0;
        if (parse_double_str(tok, &v) != 0) goto out;
        ((double *)items)[count] = v;
        break;
      }
      default:
        goto out;
    }

    count++;
  }

  if (count == 0) {
    rc = zcm_msg_put_array(msg, array_type, 0, NULL);
  } else {
    rc = zcm_msg_put_array(msg, array_type, (uint32_t)count, items);
  }

out:
  free(items);
  free(list);
  return rc;
}

static int set_payload_value(zcm_msg_t *msg, send_value_kind_t kind, const char *value) {
  if (!msg || !value) return -1;
  switch (kind) {
    case SEND_VALUE_TEXT:
      return zcm_msg_put_text(msg, value);
    case SEND_VALUE_DOUBLE: {
      double v = 0.0;
      if (parse_double_str(value, &v) != 0) return -1;
      return zcm_msg_put_double(msg, v);
    }
    case SEND_VALUE_FLOAT: {
      float v = 0.0f;
      if (parse_float_str(value, &v) != 0) return -1;
      return zcm_msg_put_float(msg, v);
    }
    case SEND_VALUE_INT: {
      int32_t v = 0;
      if (parse_int32_str(value, &v) != 0) return -1;
      return zcm_msg_put_int(msg, v);
    }
    case SEND_VALUE_CHAR: {
      char v = 0;
      if (parse_char_str(value, &v) != 0) return -1;
      return zcm_msg_put_char(msg, v);
    }
    case SEND_VALUE_SHORT: {
      int16_t v = 0;
      if (parse_int16_str(value, &v) != 0) return -1;
      return zcm_msg_put_short(msg, v);
    }
    case SEND_VALUE_LONG: {
      int64_t v = 0;
      if (parse_int64_str(value, &v) != 0) return -1;
      return zcm_msg_put_long(msg, v);
    }
    case SEND_VALUE_BYTES:
      return zcm_msg_put_bytes(msg, value, (uint32_t)strlen(value));
    case SEND_VALUE_ARRAY:
      return set_payload_array(msg, value);
    default:
      return -1;
  }
}

static uint16_t read_u16_le(const uint8_t *p) {
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p) {
  return (uint64_t)p[0] |
         ((uint64_t)p[1] << 8) |
         ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) |
         ((uint64_t)p[4] << 32) |
         ((uint64_t)p[5] << 40) |
         ((uint64_t)p[6] << 48) |
         ((uint64_t)p[7] << 56);
}

static void print_hex_bytes_limited(const uint8_t *p, size_t len, size_t max_show) {
  size_t n = (len < max_show) ? len : max_show;
  for (size_t i = 0; i < n; i++) {
    printf("%02X", p[i]);
    if (i + 1 < n) printf(" ");
  }
  if (len > max_show) printf(" ...");
}

static const char *array_type_name_from_u8(uint8_t t) {
  switch (t) {
    case ZCM_MSG_ARRAY_CHAR: return "char";
    case ZCM_MSG_ARRAY_SHORT: return "short";
    case ZCM_MSG_ARRAY_INT: return "int";
    case ZCM_MSG_ARRAY_FLOAT: return "float";
    case ZCM_MSG_ARRAY_DOUBLE: return "double";
    default: return "unknown";
  }
}

static int print_reply_payload_generic(const zcm_msg_t *reply) {
  size_t len = 0;
  const uint8_t *buf = (const uint8_t *)zcm_msg_data(reply, &len);
  if (!buf || len == 0) {
    printf(" payload={}\n");
    return 0;
  }

  printf(" payload={");
  size_t off = 0;
  int first_item = 1;
  while (off < len) {
    if (!first_item) printf(", ");
    first_item = 0;
    uint8_t item = buf[off++];

    if (item == ZCM_MSG_ITEM_CHAR) {
      if (off + 1 > len) goto malformed;
      char v = (char)buf[off++];
      printf("char='%c'(%d)", isprint((unsigned char)v) ? v : '?', (int)(unsigned char)v);
      continue;
    }
    if (item == ZCM_MSG_ITEM_SHORT) {
      if (off + 2 > len) goto malformed;
      int16_t v = (int16_t)read_u16_le(buf + off);
      off += 2;
      printf("short=%d", (int)v);
      continue;
    }
    if (item == ZCM_MSG_ITEM_INT) {
      if (off + 4 > len) goto malformed;
      int32_t v = (int32_t)read_u32_le(buf + off);
      off += 4;
      printf("int=%d", v);
      continue;
    }
    if (item == ZCM_MSG_ITEM_LONG) {
      if (off + 8 > len) goto malformed;
      int64_t v = (int64_t)read_u64_le(buf + off);
      off += 8;
      printf("long=%lld", (long long)v);
      continue;
    }
    if (item == ZCM_MSG_ITEM_FLOAT) {
      if (off + 4 > len) goto malformed;
      uint32_t bits = read_u32_le(buf + off);
      off += 4;
      float v = 0.0f;
      memcpy(&v, &bits, sizeof(v));
      printf("float=%f", v);
      continue;
    }
    if (item == ZCM_MSG_ITEM_DOUBLE) {
      if (off + 8 > len) goto malformed;
      uint64_t bits = read_u64_le(buf + off);
      off += 8;
      double v = 0.0;
      memcpy(&v, &bits, sizeof(v));
      printf("double=%f", v);
      continue;
    }
    if (item == ZCM_MSG_ITEM_TEXT) {
      if (off + 4 > len) goto malformed;
      uint32_t n = read_u32_le(buf + off);
      off += 4;
      if (off + n > len) goto malformed;
      printf("text=%.*s", (int)n, (const char *)(buf + off));
      off += n;
      continue;
    }
    if (item == ZCM_MSG_ITEM_BYTES) {
      if (off + 4 > len) goto malformed;
      uint32_t n = read_u32_le(buf + off);
      off += 4;
      if (off + n > len) goto malformed;
      printf("bytes[%u]=", n);
      print_hex_bytes_limited(buf + off, n, 16);
      off += n;
      continue;
    }
    if (item == ZCM_MSG_ITEM_ARRAY) {
      if (off + 1 + 4 > len) goto malformed;
      uint8_t arr_t = buf[off++];
      uint32_t elems = read_u32_le(buf + off);
      off += 4;
      size_t elem_size = 0;
      switch (arr_t) {
        case ZCM_MSG_ARRAY_CHAR: elem_size = 1; break;
        case ZCM_MSG_ARRAY_SHORT: elem_size = 2; break;
        case ZCM_MSG_ARRAY_INT: elem_size = 4; break;
        case ZCM_MSG_ARRAY_FLOAT: elem_size = 4; break;
        case ZCM_MSG_ARRAY_DOUBLE: elem_size = 8; break;
        default: goto malformed;
      }
      size_t total = elem_size * (size_t)elems;
      if (off + total > len) goto malformed;
      printf("array<%s>[%u]=", array_type_name_from_u8(arr_t), elems);
      size_t show = elems < 8 ? elems : 8;
      printf("[");
      for (size_t i = 0; i < show; i++) {
        if (i) printf(",");
        const uint8_t *q = buf + off + i * elem_size;
        if (arr_t == ZCM_MSG_ARRAY_CHAR) {
          char v = (char)q[0];
          printf("'%c'", isprint((unsigned char)v) ? v : '?');
        } else if (arr_t == ZCM_MSG_ARRAY_SHORT) {
          printf("%d", (int)(int16_t)read_u16_le(q));
        } else if (arr_t == ZCM_MSG_ARRAY_INT) {
          printf("%d", (int32_t)read_u32_le(q));
        } else if (arr_t == ZCM_MSG_ARRAY_FLOAT) {
          uint32_t bits = read_u32_le(q);
          float v = 0.0f;
          memcpy(&v, &bits, sizeof(v));
          printf("%f", v);
        } else if (arr_t == ZCM_MSG_ARRAY_DOUBLE) {
          uint64_t bits = read_u64_le(q);
          double v = 0.0;
          memcpy(&v, &bits, sizeof(v));
          printf("%f", v);
        }
      }
      if (elems > show) printf(",...");
      printf("]");
      off += total;
      continue;
    }

    goto malformed;
  }

  printf("}\n");
  return 0;

malformed:
  printf(" <malformed>}\n");
  return -1;
}

static int do_send(const char *endpoint, const char *name, const char *type,
                   const send_value_t *values, size_t value_count) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;
  char ep[256] = {0};
  char ctrl_ep[512] = {0};
  const char *targets[2] = {NULL, NULL};
  int target_count = 0;
  int attempt;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  if (zcm_node_lookup(node, name, ep, sizeof(ep)) != 0) {
    fprintf(stderr, "zcm: lookup failed for %s\n", name);
    goto out;
  }

  (void)zcm_node_info(node, name, NULL, 0, ctrl_ep, sizeof(ctrl_ep), NULL, 0, NULL);
  if (ctrl_ep[0] && strcmp(ctrl_ep, ep) != 0) {
    targets[target_count++] = ctrl_ep;
    targets[target_count++] = ep;
  } else {
    targets[target_count++] = ep;
  }

  for (attempt = 0; attempt < target_count; attempt++) {
    zcm_socket_t *req = NULL;
    zcm_msg_t *msg = NULL;
    zcm_msg_t *reply = NULL;
    const char *reply_type = NULL;
    const char *text = NULL;
    uint32_t text_len = 0;
    int32_t code = 0;

    req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
    if (!req) goto attempt_out;
    if (zcm_socket_connect(req, targets[attempt]) != 0) goto attempt_out;
    zcm_socket_set_timeouts(req, 1000);

    msg = zcm_msg_new();
    if (!msg) goto attempt_out;
    zcm_msg_set_type(msg, type);
    for (size_t i = 0; i < value_count; i++) {
      if (set_payload_value(msg, values[i].kind, values[i].value) != 0) {
        fprintf(stderr, "zcm: invalid payload value at position %zu\n", i + 1);
        goto attempt_out;
      }
    }

    if (zcm_socket_send_msg(req, msg) != 0) goto attempt_out;

    reply = zcm_msg_new();
    if (!reply) goto attempt_out;
    if (zcm_socket_recv_msg(req, reply) != 0) goto attempt_out;

    reply_type = zcm_msg_get_type(reply);
    if (!reply_type) reply_type = "";
    zcm_msg_rewind(reply);
    if (zcm_msg_get_text(reply, &text, &text_len) == 0 &&
        zcm_msg_get_int(reply, &code) == 0 &&
        zcm_msg_remaining(reply) == 0) {
      int is_ctrl_target = (ctrl_ep[0] && strcmp(targets[attempt], ctrl_ep) == 0);
      if (is_ctrl_target && target_count > 1 &&
          code == 404 &&
          text_len == strlen("UNKNOWN_CMD") &&
          strncmp(text, "UNKNOWN_CMD", text_len) == 0) {
        /* Control endpoint does not implement generic QUERY payloads:
         * retry once on the data endpoint. */
        goto attempt_out;
      }
      printf("[REQ -> %s] received reply: msgType=%s text=%.*s code=%d\n",
             name, reply_type, (int)text_len, text, code);
    } else {
      printf("[REQ -> %s] received reply: msgType=%s", name, reply_type);
      (void)print_reply_payload_generic(reply);
    }
    rc = 0;

attempt_out:
    if (reply) zcm_msg_free(reply);
    if (msg) zcm_msg_free(msg);
    if (req) zcm_socket_free(req);
    if (rc == 0) break;
  }

out:
  if (rc != 0) fprintf(stderr, "zcm: no reply from %s\n", name);
  if (node) zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int send_kill_msg(zcm_context_t *ctx, const char *target_ep) {
  if (!ctx || !target_ep || !*target_ep) return -1;
  if (!endpoint_is_queryable(target_ep)) return -1;
  int rc = -1;
  zcm_socket_t *req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  zcm_msg_t *msg = NULL;
  zcm_msg_t *reply = NULL;
  if (!req) return -1;
  if (zcm_socket_connect(req, target_ep) != 0) goto out;
  zcm_socket_set_timeouts(req, 1000);

  msg = zcm_msg_new();
  if (!msg) goto out;
  zcm_msg_set_type(msg, "ZCM_CMD");
  if (zcm_msg_put_text(msg, "KILL") != 0) goto out;
  if (zcm_msg_put_int(msg, 200) != 0) goto out;
  if (zcm_socket_send_msg(req, msg) != 0) goto out;

  reply = zcm_msg_new();
  if (!reply) goto out;
  if (zcm_socket_recv_msg(req, reply) != 0) goto out;

  {
    const char *text = NULL;
    uint32_t text_len = 0;
    int32_t code = 0;
    zcm_msg_rewind(reply);
    if (zcm_msg_get_text(reply, &text, &text_len) == 0 &&
        zcm_msg_get_int(reply, &code) == 0 &&
        zcm_msg_remaining(reply) == 0) {
      if (code == 200 &&
          text_len == strlen("OK") &&
          strncmp(text, "OK", text_len) == 0) {
        rc = 0;
      }
    }
  }

out:
  if (reply) zcm_msg_free(reply);
  if (msg) zcm_msg_free(msg);
  if (req) zcm_socket_free(req);
  return rc;
}

static int send_shutdown_bytes(zcm_context_t *ctx, const char *target_ep) {
  if (!ctx || !target_ep || !*target_ep) return -1;
  if (!endpoint_is_queryable(target_ep)) return -1;
  int rc = -1;
  zcm_socket_t *req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) return -1;
  if (zcm_socket_connect(req, target_ep) != 0) goto out;
  zcm_socket_set_timeouts(req, 1000);
  if (zcm_socket_send_bytes(req, "SHUTDOWN", 8) != 0) goto out;

  {
    char reply[32] = {0};
    size_t n = 0;
    if (zcm_socket_recv_bytes(req, reply, sizeof(reply) - 1, &n) != 0 || n == 0) goto out;
    reply[n] = '\0';
    if (strcmp(reply, "OK") == 0) rc = 0;
  }

out:
  if (req) zcm_socket_free(req);
  return rc;
}

static int send_ping_msg(zcm_context_t *ctx, const char *target_ep) {
  if (!ctx || !target_ep || !*target_ep) return -1;
  if (!endpoint_is_queryable(target_ep)) return -1;
  int rc = -1;
  zcm_socket_t *req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  zcm_msg_t *msg = NULL;
  zcm_msg_t *reply = NULL;
  if (!req) return -1;
  if (zcm_socket_connect(req, target_ep) != 0) goto out;
  zcm_socket_set_timeouts(req, 1000);

  msg = zcm_msg_new();
  if (!msg) goto out;
  zcm_msg_set_type(msg, "ZCM_CMD");
  if (zcm_msg_put_text(msg, "PING") != 0) goto out;
  if (zcm_msg_put_int(msg, 200) != 0) goto out;
  if (zcm_socket_send_msg(req, msg) != 0) goto out;

  reply = zcm_msg_new();
  if (!reply) goto out;
  if (zcm_socket_recv_msg(req, reply) != 0) goto out;

  {
    const char *text = NULL;
    uint32_t text_len = 0;
    int32_t code = 0;
    zcm_msg_rewind(reply);
    if (zcm_msg_get_text(reply, &text, &text_len) == 0 &&
        zcm_msg_get_int(reply, &code) == 0 &&
        zcm_msg_remaining(reply) == 0) {
      if (code == 200 &&
          text_len == strlen("PONG") &&
          strncmp(text, "PONG", text_len) == 0) {
        rc = 0;
      }
    }
  }

out:
  if (reply) zcm_msg_free(reply);
  if (msg) zcm_msg_free(msg);
  if (req) zcm_socket_free(req);
  return rc;
}

static int send_ping_bytes(zcm_context_t *ctx, const char *target_ep) {
  if (!ctx || !target_ep || !*target_ep) return -1;
  if (!endpoint_is_queryable(target_ep)) return -1;
  int rc = -1;
  zcm_socket_t *req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) return -1;
  if (zcm_socket_connect(req, target_ep) != 0) goto out;
  zcm_socket_set_timeouts(req, 1000);
  if (zcm_socket_send_bytes(req, "PING", 4) != 0) goto out;

  {
    char reply[32] = {0};
    size_t n = 0;
    if (zcm_socket_recv_bytes(req, reply, sizeof(reply) - 1, &n) != 0 || n == 0) goto out;
    reply[n] = '\0';
    if (strcmp(reply, "PONG") == 0) rc = 0;
  }

out:
  if (req) zcm_socket_free(req);
  return rc;
}

static int do_kill(const char *endpoint, const char *name) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  char data_ep[512] = {0};
  char ctrl_ep[512] = {0};
  char host[256] = {0};
  int pid = 0;
  if (zcm_node_info(node, name,
                    data_ep, sizeof(data_ep),
                    ctrl_ep, sizeof(ctrl_ep),
                    host, sizeof(host), &pid) != 0) {
    fprintf(stderr, "zcm: kill failed (no info for %s)\n", name);
    goto out;
  }

  {
    const char *targets[2] = {NULL, NULL};
    int target_count = 0;
    if (ctrl_ep[0]) targets[target_count++] = ctrl_ep;
    if (data_ep[0] && (!ctrl_ep[0] || strcmp(data_ep, ctrl_ep) != 0)) {
      targets[target_count++] = data_ep;
    }
    if (target_count == 0) {
      fprintf(stderr, "zcm: kill failed (no reachable endpoint for %s)\n", name);
      goto out;
    }

    for (int i = 0; i < target_count; i++) {
      if (send_kill_msg(ctx, targets[i]) == 0) {
        rc = 0;
        goto out;
      }
      if (i == 0 && send_shutdown_bytes(ctx, targets[i]) == 0) {
        rc = 0;
        goto out;
      }
    }
  }
  fprintf(stderr, "zcm: kill failed (node did not acknowledge KILL)\n");

out:
  if (node) zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int do_ping(const char *endpoint, const char *name) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  char data_ep[512] = {0};
  char ctrl_ep[512] = {0};
  char host[256] = {0};
  int pid = 0;
  if (zcm_node_info(node, name,
                    data_ep, sizeof(data_ep),
                    ctrl_ep, sizeof(ctrl_ep),
                    host, sizeof(host), &pid) != 0) {
    fprintf(stderr, "zcm: ping failed (no info for %s)\n", name);
    goto out;
  }

  {
    const char *targets[2] = {NULL, NULL};
    int target_count = 0;
    if (ctrl_ep[0]) targets[target_count++] = ctrl_ep;
    if (data_ep[0] && (!ctrl_ep[0] || strcmp(data_ep, ctrl_ep) != 0)) {
      targets[target_count++] = data_ep;
    }
    if (target_count == 0) {
      fprintf(stderr, "zcm: ping failed (no reachable endpoint for %s)\n", name);
      goto out;
    }

    for (int i = 0; i < target_count; i++) {
      if (send_ping_msg(ctx, targets[i]) == 0 ||
          send_ping_bytes(ctx, targets[i]) == 0) {
        printf("PONG %s %s %d\n", name, host, pid);
        rc = 0;
        goto out;
      }
    }
  }

  fprintf(stderr, "zcm: ping failed (node did not acknowledge PING)\n");

out:
  if (node) zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int do_broker_cmd(const char *endpoint, const char *cmd, const char *expected_reply) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  void *req = NULL;

  if (!ctx) return 1;
  req = zmq_socket(zcm_context_zmq(ctx), ZMQ_REQ);
  if (!req) goto out;

  int timeout_ms = 1000;
  int linger = 0;
  int immediate = 1;
  zmq_setsockopt(req, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
  zmq_setsockopt(req, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
  zmq_setsockopt(req, ZMQ_LINGER, &linger, sizeof(linger));
  zmq_setsockopt(req, ZMQ_IMMEDIATE, &immediate, sizeof(immediate));

  if (zmq_connect(req, endpoint) != 0) {
    fprintf(stderr, "zcm: broker not reachable\n");
    goto out;
  }

  if (zmq_send(req, cmd, strlen(cmd), 0) < 0) {
    fprintf(stderr, "zcm: broker command failed (send %s)\n", cmd);
    goto out;
  }

  char reply[64] = {0};
  int n = zmq_recv(req, reply, sizeof(reply) - 1, 0);
  if (n <= 0) {
    fprintf(stderr, "zcm: broker command failed (no reply for %s)\n", cmd);
    goto out;
  }
  reply[n] = '\0';

  if (expected_reply && strcmp(reply, expected_reply) != 0) {
    fprintf(stderr, "zcm: broker command failed (reply=%s)\n", reply);
    goto out;
  }

  printf("%s\n", reply);
  rc = 0;

out:
  if (req) zmq_close(req);
  zcm_context_free(ctx);
  return rc;
}

static int parse_send_args(int argc, char **argv,
                           const char **name,
                           const char **type,
                           send_value_t *values,
                           size_t *value_count) {
  if (argc < 3) return -1;
  *name = argv[2];
  *type = NULL;
  *value_count = 0;

  for (int i = 3; i < argc; i++) {
    if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
      *type = argv[++i];
    } else if ((strcmp(argv[i], "-t") == 0 ||
                strcmp(argv[i], "-d") == 0 ||
                strcmp(argv[i], "-f") == 0 ||
                strcmp(argv[i], "-i") == 0 ||
                strcmp(argv[i], "-c") == 0 ||
                strcmp(argv[i], "-s") == 0 ||
                strcmp(argv[i], "-l") == 0 ||
                strcmp(argv[i], "-b") == 0 ||
                strcmp(argv[i], "-a") == 0) && i + 1 < argc) {
      if (*value_count >= SEND_VALUE_MAX) return -1;
      send_value_t *slot = &values[*value_count];
      if (strcmp(argv[i], "-t") == 0) slot->kind = SEND_VALUE_TEXT;
      else if (strcmp(argv[i], "-d") == 0) slot->kind = SEND_VALUE_DOUBLE;
      else if (strcmp(argv[i], "-f") == 0) slot->kind = SEND_VALUE_FLOAT;
      else if (strcmp(argv[i], "-i") == 0) slot->kind = SEND_VALUE_INT;
      else if (strcmp(argv[i], "-c") == 0) slot->kind = SEND_VALUE_CHAR;
      else if (strcmp(argv[i], "-s") == 0) slot->kind = SEND_VALUE_SHORT;
      else if (strcmp(argv[i], "-l") == 0) slot->kind = SEND_VALUE_LONG;
      else if (strcmp(argv[i], "-b") == 0) slot->kind = SEND_VALUE_BYTES;
      else slot->kind = SEND_VALUE_ARRAY;
      slot->value = argv[++i];
      (*value_count)++;
    } else {
      return -1;
    }
  }

  return (*name && *type && **type != '\0' && *value_count > 0) ? 0 : -1;
}

int main(int argc, char **argv) {
  const char *cmd = NULL;
  const char *sub = NULL;
  const char *name = NULL;
  const char *type = NULL;
  send_value_t values[SEND_VALUE_MAX];
  size_t value_count = 0;

  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  cmd = argv[1];

  if (strcmp(cmd, "names") == 0) {
    if (argc != 2) {
      usage(argv[0]);
      return 1;
    }
  } else if (strcmp(cmd, "send") == 0) {
    if (parse_send_args(argc, argv, &name, &type, values, &value_count) != 0) {
      usage(argv[0]);
      return 1;
    }
  } else if (strcmp(cmd, "kill") == 0) {
    if (argc != 3) {
      usage(argv[0]);
      return 1;
    }
    name = argv[2];
  } else if (strcmp(cmd, "ping") == 0) {
    if (argc != 3) {
      usage(argv[0]);
      return 1;
    }
    name = argv[2];
  } else if (strcmp(cmd, "broker") == 0) {
    sub = (argc >= 3) ? argv[2] : NULL;
    if (sub && (strcmp(sub, "ping") == 0 || strcmp(sub, "stop") == 0 || strcmp(sub, "list") == 0)) {
      if (argc != 3) {
        usage(argv[0]);
        return 1;
      }
    } else {
      usage(argv[0]);
      return 1;
    }
  } else {
    usage(argv[0]);
    return 1;
  }

  char *endpoint = load_endpoint_from_config();
  if (!endpoint) {
    fprintf(stderr, "zcm: missing ZCMDOMAIN or ZCmDomains entry\n");
    return 1;
  }

  int rc = 1;
  if (strcmp(cmd, "names") == 0) {
    rc = do_names_with_retry(endpoint, 7000, 3);
  } else if (strcmp(cmd, "send") == 0) {
    rc = do_send(endpoint, name, type, values, value_count);
  } else if (strcmp(cmd, "kill") == 0) {
    rc = do_kill(endpoint, name);
  } else if (strcmp(cmd, "ping") == 0) {
    rc = do_ping(endpoint, name);
  } else if (strcmp(sub, "ping") == 0) {
    rc = do_broker_cmd(endpoint, "PING", "PONG");
  } else if (strcmp(sub, "stop") == 0) {
    rc = do_broker_cmd(endpoint, "STOP", "OK");
  } else {
    rc = do_names_with_retry(endpoint, 7000, 3);
  }

  free(endpoint);
  return rc;
}
