#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
  zcm_socket_set_timeouts(req, 800);
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
    if (len == token_len && strncmp(p, token, len) == 0) return 1;
    if (!next) break;
    p = next + 1;
  }
  return 0;
}

static int role_has_pub(const char *role) {
  return role_contains_token(role, "PUB");
}

static int role_has_push(const char *role) {
  return role_contains_token(role, "PUSH");
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
                            int *out_pub_port, int *out_push_port) {
  if (!ctx || !node || !name || !endpoint || !out_role || out_role_size == 0 ||
      !out_pub_port || !out_push_port) {
    return;
  }

  snprintf(out_role, out_role_size, "UNKNOWN");
  *out_pub_port = -1;
  *out_push_port = -1;

  if (strcmp(name, "zcmbroker") == 0) {
    snprintf(out_role, out_role_size, "BROKER");
    return;
  }

  char text[128] = {0};
  int code = 0;
  if (query_proc_command_with_ctrl_fallback(ctx, node, name, endpoint,
                                            "DATA_ROLE", text, sizeof(text), &code) == 0 &&
      code == 200 && role_is_valid(text)) {
    snprintf(out_role, out_role_size, "%s", text);
  }

  if (role_has_pub(out_role) || strcmp(out_role, "UNKNOWN") == 0) {
    text[0] = '\0';
    code = 0;
    if (query_proc_command_with_ctrl_fallback(ctx, node, name, endpoint,
                                              "DATA_PORT_PUB", text, sizeof(text), &code) != 0) {
      (void)query_proc_command_with_ctrl_fallback(ctx, node, name, endpoint,
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
    if (query_proc_command_with_ctrl_fallback(ctx, node, name, endpoint,
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
}

typedef struct names_row_info {
  char role[32];
  int pub_port;
  int push_port;
} names_row_info_t;

static void print_repeat_char(char ch, size_t n) {
  for (size_t i = 0; i < n; i++) putchar(ch);
}

static void print_names_broker_offline(const char *endpoint) {
  const char *name = "zcmbroker";
  const char *ep = (endpoint && *endpoint) ? endpoint : "-";
  const char *role = "BROKER_OFFLINE";
  const char *pub_port = "-";
  const char *push_port = "-";

  size_t w_name = strlen("NAME");
  size_t w_endpoint = strlen("ENDPOINT");
  size_t w_role = strlen("ROLE");
  size_t w_pub_port = strlen("PUB_PORT");
  size_t w_push_port = strlen("PUSH_PORT");

  if (strlen(name) > w_name) w_name = strlen(name);
  if (strlen(ep) > w_endpoint) w_endpoint = strlen(ep);
  if (strlen(role) > w_role) w_role = strlen(role);
  if (strlen(pub_port) > w_pub_port) w_pub_port = strlen(pub_port);
  if (strlen(push_port) > w_push_port) w_push_port = strlen(push_port);

  printf("%-*s  %-*s  %-*s  %-*s  %-*s\n",
         (int)w_name, "NAME",
         (int)w_endpoint, "ENDPOINT",
         (int)w_role, "ROLE",
         (int)w_pub_port, "PUB_PORT",
         (int)w_push_port, "PUSH_PORT");
  print_repeat_char('-', w_name);
  printf("  ");
  print_repeat_char('-', w_endpoint);
  printf("  ");
  print_repeat_char('-', w_role);
  printf("  ");
  print_repeat_char('-', w_pub_port);
  printf("  ");
  print_repeat_char('-', w_push_port);
  printf("\n");

  printf("%-*s  %-*s  %-*s  %-*s  %-*s\n",
         (int)w_name, name,
         (int)w_endpoint, ep,
         (int)w_role, role,
         (int)w_pub_port, pub_port,
         (int)w_push_port, push_port);
}

static int do_names(const char *endpoint) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;
  zcm_node_t *node = zcm_node_new(ctx, endpoint);
  if (!node) {
    zcm_context_free(ctx);
    return 1;
  }

  zcm_node_entry_t *entries = NULL;
  size_t count = 0;
  if (zcm_node_list(node, &entries, &count) == 0) {
    names_row_info_t *rows = NULL;
    if (count > 0) {
      rows = (names_row_info_t *)calloc(count, sizeof(*rows));
      if (!rows) {
        zcm_node_list_free(entries, count);
        zcm_node_free(node);
        zcm_context_free(ctx);
        return 1;
      }
    }

    size_t w_name = strlen("NAME");
    size_t w_endpoint = strlen("ENDPOINT");
    size_t w_role = strlen("ROLE");
    size_t w_pub_port = strlen("PUB_PORT");
    size_t w_push_port = strlen("PUSH_PORT");

    for (size_t i = 0; i < count; i++) {
      probe_node_role(ctx, node, entries[i].name, entries[i].endpoint,
                      rows[i].role, sizeof(rows[i].role),
                      &rows[i].pub_port, &rows[i].push_port);

      size_t name_len = strlen(entries[i].name);
      if (name_len > w_name) w_name = name_len;
      size_t ep_len = strlen(entries[i].endpoint);
      if (ep_len > w_endpoint) w_endpoint = ep_len;
      size_t role_len = strlen(rows[i].role);
      if (role_len > w_role) w_role = role_len;

      char port_text[16];
      if (rows[i].pub_port > 0) snprintf(port_text, sizeof(port_text), "%d", rows[i].pub_port);
      else snprintf(port_text, sizeof(port_text), "-");
      size_t port_len = strlen(port_text);
      if (port_len > w_pub_port) w_pub_port = port_len;

      if (rows[i].push_port > 0) snprintf(port_text, sizeof(port_text), "%d", rows[i].push_port);
      else snprintf(port_text, sizeof(port_text), "-");
      port_len = strlen(port_text);
      if (port_len > w_push_port) w_push_port = port_len;
    }

    printf("%-*s  %-*s  %-*s  %-*s  %-*s\n",
           (int)w_name, "NAME",
           (int)w_endpoint, "ENDPOINT",
           (int)w_role, "ROLE",
           (int)w_pub_port, "PUB_PORT",
           (int)w_push_port, "PUSH_PORT");
    print_repeat_char('-', w_name);
    printf("  ");
    print_repeat_char('-', w_endpoint);
    printf("  ");
    print_repeat_char('-', w_role);
    printf("  ");
    print_repeat_char('-', w_pub_port);
    printf("  ");
    print_repeat_char('-', w_push_port);
    printf("\n");

    for (size_t i = 0; i < count; i++) {
      char pub_port_text[16];
      char push_port_text[16];
      if (rows[i].pub_port > 0) snprintf(pub_port_text, sizeof(pub_port_text), "%d", rows[i].pub_port);
      else snprintf(pub_port_text, sizeof(pub_port_text), "-");
      if (rows[i].push_port > 0) snprintf(push_port_text, sizeof(push_port_text), "%d", rows[i].push_port);
      else snprintf(push_port_text, sizeof(push_port_text), "-");

      printf("%-*s  %-*s  %-*s  %-*s  %-*s\n",
             (int)w_name, entries[i].name,
             (int)w_endpoint, entries[i].endpoint,
             (int)w_role, rows[i].role,
             (int)w_pub_port, pub_port_text,
             (int)w_push_port, push_port_text);
    }

    if (rows) {
      free(rows);
      rows = NULL;
    }
    zcm_node_list_free(entries, count);
    rc = 0;
  }

  zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int do_names_with_timeout(const char *endpoint, int timeout_ms) {
  pid_t pid = fork();
  if (pid == 0) {
    int rc = do_names(endpoint);
    fflush(NULL);
    _exit(rc);
  }
  if (pid < 0) return 1;

  int status = 0;
  int waited = 0;
  while (waited < timeout_ms) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      if (WIFEXITED(status)) {
        int rc = WEXITSTATUS(status);
        if (rc != 0) {
          print_names_broker_offline(endpoint);
          fprintf(stderr, "zcm: broker not reachable\n");
        }
        return rc;
      }
      return 1;
    }
    usleep(100 * 1000);
    waited += 100;
  }

  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  print_names_broker_offline(endpoint);
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
  zcm_socket_t *req = NULL;
  zcm_msg_t *msg = NULL;
  zcm_msg_t *reply = NULL;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  char ep[256] = {0};
  if (zcm_node_lookup(node, name, ep, sizeof(ep)) != 0) {
    fprintf(stderr, "zcm: lookup failed for %s\n", name);
    goto out;
  }

  req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) goto out;
  if (zcm_socket_connect(req, ep) != 0) {
    fprintf(stderr, "zcm: connect failed\n");
    goto out;
  }
  zcm_socket_set_timeouts(req, 1000);

  msg = zcm_msg_new();
  if (!msg) goto out;
  zcm_msg_set_type(msg, type);
  for (size_t i = 0; i < value_count; i++) {
    if (set_payload_value(msg, values[i].kind, values[i].value) != 0) {
      fprintf(stderr, "zcm: invalid payload value at position %zu\n", i + 1);
      goto out;
    }
  }

  if (zcm_socket_send_msg(req, msg) != 0) {
    fprintf(stderr, "zcm: send failed\n");
    goto out;
  }

  reply = zcm_msg_new();
  if (!reply) goto out;
  if (zcm_socket_recv_msg(req, reply) != 0) {
    fprintf(stderr, "zcm: no reply from %s\n", name);
    goto out;
  }

  const char *text = NULL;
  uint32_t text_len = 0;
  int32_t code = 0;
  const char *reply_type = zcm_msg_get_type(reply);
  if (!reply_type) reply_type = "";

  zcm_msg_rewind(reply);
  if (zcm_msg_get_text(reply, &text, &text_len) == 0 &&
      zcm_msg_get_int(reply, &code) == 0 &&
      zcm_msg_remaining(reply) == 0) {
    printf("[REQ -> %s] received reply: msgType=%s text=%.*s code=%d\n",
           name, reply_type, (int)text_len, text, code);
  } else {
    printf("[REQ -> %s] received reply: msgType=%s", name, reply_type);
    (void)print_reply_payload_generic(reply);
  }

  rc = 0;

out:
  if (reply) zcm_msg_free(reply);
  if (msg) zcm_msg_free(msg);
  if (req) zcm_socket_free(req);
  if (node) zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int do_kill(const char *endpoint, const char *name) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;
  zcm_socket_t *req = NULL;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  char ctrl_ep[512] = {0};
  char host[256] = {0};
  int pid = 0;
  if (zcm_node_info(node, name, NULL, 0, ctrl_ep, sizeof(ctrl_ep), host, sizeof(host), &pid) != 0) {
    fprintf(stderr, "zcm: kill failed (no info for %s)\n", name);
    goto out;
  }
  if (ctrl_ep[0] == '\0') {
    fprintf(stderr, "zcm: kill failed (no control endpoint for %s)\n", name);
    goto out;
  }

  req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) goto out;
  if (zcm_socket_connect(req, ctrl_ep) != 0) {
    fprintf(stderr, "zcm: kill failed (connect control endpoint)\n");
    goto out;
  }
  zcm_socket_set_timeouts(req, 1000);
  if (zcm_socket_send_bytes(req, "SHUTDOWN", 8) != 0) {
    fprintf(stderr, "zcm: kill failed (send shutdown)\n");
    goto out;
  }

  char reply[32] = {0};
  size_t n = 0;
  if (zcm_socket_recv_bytes(req, reply, sizeof(reply) - 1, &n) != 0 || n == 0) {
    fprintf(stderr, "zcm: kill failed (no reply)\n");
    goto out;
  }
  reply[n] = '\0';
  if (strcmp(reply, "OK") != 0) {
    fprintf(stderr, "zcm: kill failed (reply=%s)\n", reply);
    goto out;
  }

  rc = 0;

out:
  if (req) zcm_socket_free(req);
  if (node) zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int do_ping(const char *endpoint, const char *name) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;
  zcm_socket_t *req = NULL;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  char ctrl_ep[512] = {0};
  char host[256] = {0};
  int pid = 0;
  if (zcm_node_info(node, name, NULL, 0, ctrl_ep, sizeof(ctrl_ep), host, sizeof(host), &pid) != 0) {
    fprintf(stderr, "zcm: ping failed (no info for %s)\n", name);
    goto out;
  }
  if (ctrl_ep[0] == '\0') {
    fprintf(stderr, "zcm: ping failed (no control endpoint for %s)\n", name);
    goto out;
  }

  req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) goto out;
  if (zcm_socket_connect(req, ctrl_ep) != 0) {
    fprintf(stderr, "zcm: ping failed (connect control endpoint)\n");
    goto out;
  }
  zcm_socket_set_timeouts(req, 1000);
  if (zcm_socket_send_bytes(req, "PING", 4) != 0) {
    fprintf(stderr, "zcm: ping failed (send ping)\n");
    goto out;
  }

  char reply[32] = {0};
  size_t n = 0;
  if (zcm_socket_recv_bytes(req, reply, sizeof(reply) - 1, &n) != 0 || n == 0) {
    fprintf(stderr, "zcm: ping failed (no reply)\n");
    goto out;
  }
  reply[n] = '\0';
  if (strcmp(reply, "PONG") != 0) {
    fprintf(stderr, "zcm: ping failed (reply=%s)\n", reply);
    goto out;
  }

  printf("PONG %s %s %d\n", name, host, pid);
  rc = 0;

out:
  if (req) zcm_socket_free(req);
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
    rc = do_names_with_timeout(endpoint, 2000);
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
    rc = do_names_with_timeout(endpoint, 2000);
  }

  free(endpoint);
  return rc;
}
