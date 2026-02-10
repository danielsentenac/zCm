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
    for (size_t i = 0; i < count; i++) {
      printf("%s %s\n", entries[i].name, entries[i].endpoint);
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
    _exit(rc);
  }
  if (pid < 0) return 1;

  int status = 0;
  int waited = 0;
  while (waited < timeout_ms) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      if (WIFEXITED(status)) return WEXITSTATUS(status);
      return 1;
    }
    usleep(100 * 1000);
    waited += 100;
  }

  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
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
  zcm_msg_rewind(reply);
  if (zcm_msg_get_text(reply, &text, &text_len) == 0 &&
      zcm_msg_get_int(reply, &code) == 0 &&
      zcm_msg_remaining(reply) == 0) {
    printf("[REQ -> %s] received reply: msgType=%s text=%.*s code=%d\n",
           name, zcm_msg_get_type(reply), (int)text_len, text, code);
  } else {
    zcm_msg_rewind(reply);
    if (zcm_msg_get_text(reply, &text, &text_len) == 0 &&
        zcm_msg_remaining(reply) == 0) {
      printf("[REQ -> %s] received reply: msgType=%s text=%.*s\n",
             name, zcm_msg_get_type(reply), (int)text_len, text);
    } else {
      double d = 0.0;
      float f = 0.0f;
      int32_t i = 0;
      zcm_msg_rewind(reply);
      if (zcm_msg_get_double(reply, &d) == 0 && zcm_msg_remaining(reply) == 0) {
        printf("[REQ -> %s] received reply: msgType=%s double=%f\n",
               name, zcm_msg_get_type(reply), d);
      } else {
        zcm_msg_rewind(reply);
        if (zcm_msg_get_float(reply, &f) == 0 && zcm_msg_remaining(reply) == 0) {
          printf("[REQ -> %s] received reply: msgType=%s float=%f\n",
                 name, zcm_msg_get_type(reply), f);
        } else {
          zcm_msg_rewind(reply);
          if (zcm_msg_get_int(reply, &i) == 0 && zcm_msg_remaining(reply) == 0) {
            printf("[REQ -> %s] received reply: msgType=%s int=%d\n",
                   name, zcm_msg_get_type(reply), i);
          } else {
            printf("[REQ -> %s] received reply: msgType=%s\n",
                   name, zcm_msg_get_type(reply));
          }
        }
      }
    }
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
