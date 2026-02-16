#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>

#include <zmq.h>

struct zcm_broker_entry {
  char *name;
  char *endpoint;
  char *ctrl_endpoint;
  char *host;
  int pid;
  char role[32];
  int pub_port;
  int push_port;
  int pub_bytes;
  int sub_bytes;
  int push_bytes;
  int pull_bytes;
  struct zcm_broker_entry *next;
};

struct zcm_broker {
  zcm_context_t *ctx;
  char *endpoint;
  pthread_t thread;
  int running;
  struct zcm_broker_entry *head;
};

static int entry_remove(struct zcm_broker *b, const char *name);

static void entry_reset_metrics(struct zcm_broker_entry *e) {
  if (!e) return;
  snprintf(e->role, sizeof(e->role), "UNKNOWN");
  e->pub_port = -1;
  e->push_port = -1;
  e->pub_bytes = -1;
  e->sub_bytes = -1;
  e->push_bytes = -1;
  e->pull_bytes = -1;
}

static int endpoint_has_scheme(const char *endpoint, const char *scheme) {
  if (!endpoint || !scheme) return 0;
  size_t n = strlen(scheme);
  if (n == 0) return 0;
  return strncmp(endpoint, scheme, n) == 0;
}

static int endpoint_tcp_parse_host_port(const char *endpoint,
                                        char *out_host, size_t out_host_size,
                                        int *out_port) {
  if (!endpoint || !out_host || out_host_size == 0 || !out_port) return -1;
  out_host[0] = '\0';
  *out_port = 0;
  if (strncmp(endpoint, "tcp://", 6) != 0) return -1;

  const char *addr = endpoint + 6;
  if (!*addr) return -1;

  const char *host_begin = addr;
  const char *host_end = NULL;
  const char *port_text = NULL;
  if (*addr == '[') {
    host_begin = addr + 1;
    host_end = strchr(host_begin, ']');
    if (!host_end || host_end <= host_begin || host_end[1] != ':') return -1;
    port_text = host_end + 2;
  } else {
    host_end = strrchr(addr, ':');
    if (!host_end || host_end <= host_begin) return -1;
    port_text = host_end + 1;
  }
  if (!port_text || !*port_text) return -1;

  size_t host_len = (size_t)(host_end - host_begin);
  if (host_len == 0) return -1;
  if (host_len >= out_host_size) host_len = out_host_size - 1;
  memcpy(out_host, host_begin, host_len);
  out_host[host_len] = '\0';

  char *end = NULL;
  long p = strtol(port_text, &end, 10);
  if (!end || *end != '\0' || p < 1 || p > 65535) return -1;
  *out_port = (int)p;
  return 0;
}

static int endpoint_host_needs_resolution(const char *host) {
  if (!host || !*host) return 1;
  if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "::") == 0 ||
      strcmp(host, "0:0:0:0:0:0:0:0") == 0 || strcmp(host, "*") == 0) {
    return 1;
  }

  if (strncasecmp(host, "eth", 3) == 0 ||
      strncasecmp(host, "en", 2) == 0 ||
      strncasecmp(host, "wl", 2) == 0 ||
      strncasecmp(host, "lo", 2) == 0) {
    return 1;
  }

  return 0;
}

static int build_tcp_endpoint_text(const char *host, int port,
                                   char *out, size_t out_size) {
  if (!host || !*host || !out || out_size == 0) return -1;
  if (port < 1 || port > 65535) return -1;

  const int looks_ipv6 = strchr(host, ':') != NULL;
  if (looks_ipv6 && host[0] != '[') {
    snprintf(out, out_size, "tcp://[%s]:%d", host, port);
  } else {
    snprintf(out, out_size, "tcp://%s:%d", host, port);
  }
  return 0;
}

static void entry_effective_endpoint(const struct zcm_broker_entry *e,
                                     char *out_endpoint,
                                     size_t out_size) {
  char ep_host[256] = {0};
  int ep_port = 0;

  if (!out_endpoint || out_size == 0) return;
  out_endpoint[0] = '\0';
  if (!e || !e->endpoint || !*e->endpoint) return;

  snprintf(out_endpoint, out_size, "%s", e->endpoint);

  if (endpoint_tcp_parse_host_port(e->endpoint, ep_host, sizeof(ep_host), &ep_port) != 0) return;
  if (!endpoint_host_needs_resolution(ep_host)) return;
  if (!e->host || !*e->host) return;
  if (endpoint_host_needs_resolution(e->host)) return;

  (void)build_tcp_endpoint_text(e->host, ep_port, out_endpoint, out_size);
}

static int parse_int_text(const char *text, int *out_value) {
  if (!text || !out_value) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < INT_MIN || v > INT_MAX) return -1;
  *out_value = (int)v;
  return 0;
}

static int endpoint_is_queryable(const char *endpoint) {
  if (!endpoint || !*endpoint) return 0;
  if (strncmp(endpoint, "tcp://", 6) == 0) return 1;
  if (strncmp(endpoint, "ipc://", 6) == 0) return 1;
  if (strncmp(endpoint, "inproc://", 9) == 0) return 1;
  return 0;
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

static int role_is_valid(const char *role) {
  if (!role || !*role) return 0;
  if (strcmp(role, "NONE") == 0) return 1;

  const char *p = role;
  while (p && *p) {
    const char *next = strchr(p, '+');
    size_t len = next ? (size_t)(next - p) : strlen(p);
    if (len == 0) return 0;
    if (!((len == 3 && strncmp(p, "PUB", 3) == 0) ||
          (len == 3 && strncmp(p, "SUB", 3) == 0) ||
          (len == 4 && strncmp(p, "PUSH", 4) == 0) ||
          (len == 4 && strncmp(p, "PULL", 4) == 0))) {
      return 0;
    }
    if (!next) break;
    p = next + 1;
  }
  return 1;
}

static void entry_free(struct zcm_broker_entry *e) {
  if (!e) return;
  free(e->name);
  free(e->endpoint);
  free(e->ctrl_endpoint);
  free(e->host);
  free(e);
}

static int host_is_local(const char *host) {
  if (!host || !*host) return 0;
  if (strcmp(host, "127.0.0.1") == 0 ||
      strcmp(host, "::1") == 0 ||
      strcasecmp(host, "localhost") == 0) {
    return 1;
  }

  char local_host[256] = {0};
  if (gethostname(local_host, sizeof(local_host) - 1) != 0) return 0;
  local_host[sizeof(local_host) - 1] = '\0';
  if (strcasecmp(host, local_host) == 0) return 1;

  char host_short[256] = {0};
  char local_short[256] = {0};
  snprintf(host_short, sizeof(host_short), "%s", host);
  snprintf(local_short, sizeof(local_short), "%s", local_host);
  char *dot = strchr(host_short, '.');
  if (dot) *dot = '\0';
  dot = strchr(local_short, '.');
  if (dot) *dot = '\0';
  return strcasecmp(host_short, local_short) == 0;
}

static int pid_is_alive_local(int pid) {
  if (pid <= 0) return 0;
  if (kill((pid_t)pid, 0) == 0) return 1;
  return errno == EPERM;
}

static int entry_is_stale_local(const struct zcm_broker_entry *e) {
  if (!e || e->pid <= 0) return 0;
  if (!host_is_local(e->host)) return 0;
  return !pid_is_alive_local(e->pid);
}

static int entry_prune_stale_local(struct zcm_broker *b) {
  int removed = 0;
  struct zcm_broker_entry *prev = NULL;
  struct zcm_broker_entry *e = b->head;

  while (e) {
    if (entry_is_stale_local(e)) {
      struct zcm_broker_entry *dead = e;
      if (prev) prev->next = e->next;
      else b->head = e->next;
      e = e->next;
      entry_free(dead);
      removed++;
      continue;
    }
    prev = e;
    e = e->next;
  }

  return removed;
}

static struct zcm_broker_entry *entry_find(struct zcm_broker *b, const char *name) {
  for (struct zcm_broker_entry *e = b->head; e; e = e->next) {
    if (strcmp(e->name, name) == 0) return e;
  }
  return NULL;
}

/*
 * Register a basic name -> endpoint entry.
 * Return codes:
 *   0  success (new or idempotent same endpoint)
 *   1  duplicate name with conflicting endpoint/owner
 *  -1  allocation/internal error
 */
static int entry_set(struct zcm_broker *b, const char *name, const char *endpoint) {
  struct zcm_broker_entry *e = entry_find(b, name);
  if (e) {
    if (e->endpoint && strcmp(e->endpoint, endpoint) == 0) return 0;
    return 1;
  }

  e = (struct zcm_broker_entry *)calloc(1, sizeof(*e));
  if (!e) return -1;
  e->name = strdup(name);
  if (!e->name) {
    free(e);
    return -1;
  }
  e->endpoint = strdup(endpoint);
  if (!e->endpoint) {
    free(e->name);
    free(e);
    return -1;
  }
  e->pid = 0;
  entry_reset_metrics(e);
  if (endpoint_has_scheme(endpoint, "sub://")) {
    snprintf(e->role, sizeof(e->role), "SUB");
  }
  e->next = b->head;
  b->head = e;
  return 0;
}

/*
 * Register an extended entry.
 * Return codes:
 *   0  success (new or re-register by same owner)
 *   1  duplicate name with different owner
 *  -1  allocation/internal error
 */
static int entry_set_ex(struct zcm_broker *b, const char *name, const char *endpoint,
                        const char *ctrl_endpoint, const char *host, int pid) {
  struct zcm_broker_entry *e = entry_find(b, name);
  if (e && entry_is_stale_local(e)) {
    (void)entry_remove(b, name);
    e = NULL;
  }
  if (e) {
    int same_owner = 0;
    if (e->pid > 0 && pid > 0 && e->pid == pid &&
        e->host && host && strcmp(e->host, host) == 0) {
      same_owner = 1;
    }
    if (!same_owner) return 1;
  }

  char *new_endpoint = endpoint ? strdup(endpoint) : NULL;
  char *new_ctrl = ctrl_endpoint ? strdup(ctrl_endpoint) : NULL;
  char *new_host = host ? strdup(host) : NULL;
  if ((endpoint && !new_endpoint) || (ctrl_endpoint && !new_ctrl) || (host && !new_host)) {
    free(new_endpoint);
    free(new_ctrl);
    free(new_host);
    return -1;
  }

  if (!e) {
    e = (struct zcm_broker_entry *)calloc(1, sizeof(*e));
    if (!e) {
      free(new_endpoint);
      free(new_ctrl);
      free(new_host);
      return -1;
    }
    e->name = strdup(name);
    if (!e->name) {
      free(new_endpoint);
      free(new_ctrl);
      free(new_host);
      free(e);
      return -1;
    }
    entry_reset_metrics(e);
    e->next = b->head;
    b->head = e;
  }

  free(e->endpoint);
  free(e->ctrl_endpoint);
  free(e->host);
  e->endpoint = new_endpoint;
  e->ctrl_endpoint = new_ctrl;
  e->host = new_host;
  e->pid = pid;
  if (endpoint_has_scheme(endpoint, "sub://")) {
    snprintf(e->role, sizeof(e->role), "SUB");
  }
  return 0;
}

static int entry_remove(struct zcm_broker *b, const char *name) {
  struct zcm_broker_entry *prev = NULL;
  struct zcm_broker_entry *e = b->head;
  while (e) {
    if (strcmp(e->name, name) == 0) {
      if (prev) prev->next = e->next;
      else b->head = e->next;
      entry_free(e);
      return 0;
    }
    prev = e;
    e = e->next;
  }
  return -1;
}

static int query_proc_command_once(zcm_context_t *ctx, const char *endpoint,
                                   const char *cmd, int include_code_field,
                                   char *out_text, size_t out_text_size,
                                   int *out_code) {
  if (!ctx || !endpoint || !*endpoint || !cmd || !*cmd || !out_text || out_text_size == 0) {
    return -1;
  }
  if (!endpoint_is_queryable(endpoint)) return -1;
  out_text[0] = '\0';
  if (out_code) *out_code = 0;

  int rc = -1;
  zcm_socket_t *req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) return -1;
  zcm_socket_set_timeouts(req, 250);
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

static int query_proc_ping_ok(zcm_context_t *ctx, const char *endpoint) {
  char text[32] = {0};
  int code = 0;
  if (!ctx || !endpoint || !*endpoint) return 0;
  if (query_proc_command_once(ctx, endpoint, "PING", 1, text, sizeof(text), &code) != 0) return 0;
  if (code != 200) return 0;
  return strcmp(text, "PONG") == 0;
}

static int query_proc_command_with_ctrl_fallback(zcm_context_t *ctx,
                                                 const char *endpoint,
                                                 const char *ctrl_endpoint,
                                                 const char *cmd,
                                                 char *out_text,
                                                 size_t out_text_size,
                                                 int *out_code) {
  if (!ctx || !cmd || !*cmd || !out_text || out_text_size == 0) {
    return -1;
  }
  if (ctrl_endpoint && *ctrl_endpoint) {
    if (query_proc_command(ctx, ctrl_endpoint, cmd, out_text, out_text_size, out_code) == 0) {
      return 0;
    }
  }
  if (!endpoint || !*endpoint) return -1;
  return query_proc_command(ctx, endpoint, cmd, out_text, out_text_size, out_code);
}

static int entry_prune_stale_remote_ctrl(struct zcm_broker *b) {
  int removed = 0;
  struct zcm_broker_entry *prev = NULL;
  struct zcm_broker_entry *e = NULL;

  if (!b) return 0;
  e = b->head;
  while (e) {
    int should_check = 0;
    char probe_host[256] = {0};
    int probe_port = 0;

    if (strcmp(e->name, "zcmbroker") != 0 &&
        e->pid > 0 &&
        e->ctrl_endpoint && e->ctrl_endpoint[0] &&
        endpoint_is_queryable(e->ctrl_endpoint)) {
      should_check = 1;
    }

    if (should_check) {
      if (e->host && e->host[0]) {
        snprintf(probe_host, sizeof(probe_host), "%s", e->host);
      } else if (endpoint_tcp_parse_host_port(e->ctrl_endpoint, probe_host, sizeof(probe_host), &probe_port) == 0) {
        (void)probe_port;
      } else if (e->endpoint &&
                 endpoint_tcp_parse_host_port(e->endpoint, probe_host, sizeof(probe_host), &probe_port) == 0) {
        (void)probe_port;
      }

      if (probe_host[0] && !host_is_local(probe_host)) {
        if (!query_proc_ping_ok(b->ctx, e->ctrl_endpoint)) {
          struct zcm_broker_entry *dead = e;
          if (prev) prev->next = e->next;
          else b->head = e->next;
          e = e->next;
          entry_free(dead);
          removed++;
          continue;
        }
      }
    }

    prev = e;
    e = e->next;
  }

  return removed;
}

static void entry_refresh_metrics(struct zcm_broker *b, struct zcm_broker_entry *e) {
  if (!b || !e || !e->name) return;

  if (strcmp(e->name, "zcmbroker") == 0) {
    snprintf(e->role, sizeof(e->role), "BROKER");
    e->pub_port = -1;
    e->push_port = -1;
    e->pub_bytes = -1;
    e->sub_bytes = -1;
    e->push_bytes = -1;
    e->pull_bytes = -1;
    return;
  }

  if ((!e->role[0] || strcmp(e->role, "UNKNOWN") == 0) && e->endpoint) {
    if (endpoint_has_scheme(e->endpoint, "sub://")) {
      snprintf(e->role, sizeof(e->role), "SUB");
    } else if ((!e->ctrl_endpoint || !e->ctrl_endpoint[0]) &&
               (!e->host || !e->host[0]) &&
               e->pid <= 0) {
      snprintf(e->role, sizeof(e->role), "EXTERNAL");
    }
  }

  if ((!e->ctrl_endpoint || !e->ctrl_endpoint[0]) &&
      (!e->endpoint || !*e->endpoint)) {
    return;
  }

  /*
   * SUB-only external consumers (for example servlet subscribers) can register
   * an endpoint that points to their publisher data socket. That endpoint is
   * not a control REP endpoint and must not be probed with DATA_* commands.
   */
  if ((!e->ctrl_endpoint || !e->ctrl_endpoint[0]) &&
      (strcmp(e->role, "SUB") == 0 || endpoint_has_scheme(e->endpoint, "sub://"))) {
    return;
  }

  /*
   * Avoid synchronous cross-host probing on the broker request thread.
   * Remote nodes should report metrics via METRICS; probing them here can stall
   * LIST_EX when a remote process disappears without unregistering.
   */
  {
    char probe_host[256] = {0};
    int probe_port = 0;
    if (e->host && e->host[0]) {
      snprintf(probe_host, sizeof(probe_host), "%s", e->host);
    } else if (e->endpoint &&
               endpoint_tcp_parse_host_port(e->endpoint, probe_host, sizeof(probe_host), &probe_port) == 0) {
      (void)probe_port;
    }
    if (probe_host[0] && !host_is_local(probe_host)) {
      return;
    }
  }

  char text[128] = {0};
  int code = 0;
  int role_probe_ok = 0;

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_ROLE", text, sizeof(text), &code) == 0 &&
      code == 200 && role_is_valid(text)) {
    snprintf(e->role, sizeof(e->role), "%s", text);
    role_probe_ok = 1;
  }

  if (!role_probe_ok) {
    return;
  }

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_PORT_PUB", text, sizeof(text), &code) == 0 &&
      code == 200) {
    int v = -1;
    if (parse_int_text(text, &v) == 0 && v > 0) e->pub_port = v;
  }

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_PORT_PUSH", text, sizeof(text), &code) == 0 &&
      code == 200) {
    int v = -1;
    if (parse_int_text(text, &v) == 0 && v > 0) e->push_port = v;
  }

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_PAYLOAD_BYTES_PUB", text, sizeof(text), &code) == 0 &&
      code == 200) {
    int v = -1;
    if (parse_int_text(text, &v) == 0 && v >= 0) e->pub_bytes = v;
  }

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_PAYLOAD_BYTES_SUB", text, sizeof(text), &code) == 0 &&
      code == 200) {
    int v = -1;
    if (parse_int_text(text, &v) == 0 && v >= 0) e->sub_bytes = v;
  }

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_PAYLOAD_BYTES_PUSH", text, sizeof(text), &code) == 0 &&
      code == 200) {
    int v = -1;
    if (parse_int_text(text, &v) == 0 && v >= 0) e->push_bytes = v;
  }

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_PAYLOAD_BYTES_PULL", text, sizeof(text), &code) == 0 &&
      code == 200) {
    int v = -1;
    if (parse_int_text(text, &v) == 0 && v >= 0) e->pull_bytes = v;
  }

  if (e->pub_port > 0 && !role_contains_token(e->role, "PUB")) {
    if (strcmp(e->role, "UNKNOWN") == 0 || strcmp(e->role, "NONE") == 0 || !e->role[0]) {
      snprintf(e->role, sizeof(e->role), "PUB");
    }
  }
  if (e->push_port > 0 && !role_contains_token(e->role, "PUSH")) {
    if (strcmp(e->role, "UNKNOWN") == 0 || strcmp(e->role, "NONE") == 0 || !e->role[0]) {
      snprintf(e->role, sizeof(e->role), "PUSH");
    }
  }
}

static void *broker_thread(void *arg) {
  struct zcm_broker *b = (struct zcm_broker *)arg;
  void *sock = zmq_socket(zcm_context_zmq(b->ctx), ZMQ_REP);
  if (!sock) return NULL;
  if (zmq_bind(sock, b->endpoint) != 0) {
    zmq_close(sock);
    return NULL;
  }

  while (b->running) {
    zmq_msg_t part;
    zmq_msg_init(&part);
    int rc = zmq_msg_recv(&part, sock, 0);
    if (rc < 0) {
      zmq_msg_close(&part);
      continue;
    }
    char cmd[32] = {0};
    size_t cmd_len = zmq_msg_size(&part);
    if (cmd_len >= sizeof(cmd)) cmd_len = sizeof(cmd) - 1;
    memcpy(cmd, zmq_msg_data(&part), cmd_len);
    zmq_msg_close(&part);

    (void)entry_prune_stale_local(b);
    if (strcmp(cmd, "LIST_EX") == 0 ||
        strcmp(cmd, "LIST") == 0 ||
        strcmp(cmd, "LOOKUP") == 0 ||
        strcmp(cmd, "INFO") == 0) {
      (void)entry_prune_stale_remote_ctrl(b);
    }

    if (strcmp(cmd, "REGISTER") == 0) {
      char name[256] = {0};
      char endpoint[512] = {0};

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t nlen = zmq_msg_size(&part);
      if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
      memcpy(name, zmq_msg_data(&part), nlen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t elen = zmq_msg_size(&part);
      if (elen >= sizeof(endpoint)) elen = sizeof(endpoint) - 1;
      memcpy(endpoint, zmq_msg_data(&part), elen);
      zmq_msg_close(&part);

      int reg_rc = entry_set(b, name, endpoint);
      if (reg_rc == 0) {
        zmq_send(sock, "OK", 2, 0);
      } else if (reg_rc == 1) {
        zmq_send(sock, "DUPLICATE", 9, 0);
      } else {
        zmq_send(sock, "ERR", 3, 0);
      }
    } else if (strcmp(cmd, "REGISTER_EX") == 0) {
      char name[256] = {0};
      char endpoint[512] = {0};
      char ctrl_ep[512] = {0};
      char host[256] = {0};
      char pid_str[32] = {0};

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t nlen = zmq_msg_size(&part);
      if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
      memcpy(name, zmq_msg_data(&part), nlen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t elen = zmq_msg_size(&part);
      if (elen >= sizeof(endpoint)) elen = sizeof(endpoint) - 1;
      memcpy(endpoint, zmq_msg_data(&part), elen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t clen = zmq_msg_size(&part);
      if (clen >= sizeof(ctrl_ep)) clen = sizeof(ctrl_ep) - 1;
      memcpy(ctrl_ep, zmq_msg_data(&part), clen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t hlen = zmq_msg_size(&part);
      if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
      memcpy(host, zmq_msg_data(&part), hlen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t plen = zmq_msg_size(&part);
      if (plen >= sizeof(pid_str)) plen = sizeof(pid_str) - 1;
      memcpy(pid_str, zmq_msg_data(&part), plen);
      zmq_msg_close(&part);

      int pid = atoi(pid_str);
      int reg_rc = entry_set_ex(b, name, endpoint, ctrl_ep, host, pid);
      if (reg_rc == 0) {
        zmq_send(sock, "OK", 2, 0);
      } else if (reg_rc == 1) {
        zmq_send(sock, "DUPLICATE", 9, 0);
      } else {
        zmq_send(sock, "ERR", 3, 0);
      }
    } else if (strcmp(cmd, "LOOKUP") == 0) {
      char name[256] = {0};
      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t nlen = zmq_msg_size(&part);
      if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
      memcpy(name, zmq_msg_data(&part), nlen);
      zmq_msg_close(&part);

      struct zcm_broker_entry *e = entry_find(b, name);
      if (!e) {
        zmq_send(sock, "NOT_FOUND", 9, 0);
      } else {
        char endpoint[512] = {0};
        entry_effective_endpoint(e, endpoint, sizeof(endpoint));
        zmq_send(sock, "OK", 2, ZMQ_SNDMORE);
        zmq_send(sock, endpoint, strlen(endpoint), 0);
      }
    } else if (strcmp(cmd, "INFO") == 0) {
      char name[256] = {0};
      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t nlen = zmq_msg_size(&part);
      if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
      memcpy(name, zmq_msg_data(&part), nlen);
      zmq_msg_close(&part);

      struct zcm_broker_entry *e = entry_find(b, name);
      if (!e) {
        zmq_send(sock, "NOT_FOUND", 9, 0);
      } else {
        char pid_buf[32];
        char endpoint[512] = {0};
        entry_effective_endpoint(e, endpoint, sizeof(endpoint));
        snprintf(pid_buf, sizeof(pid_buf), "%d", e->pid);
        zmq_send(sock, "OK", 2, ZMQ_SNDMORE);
        zmq_send(sock, endpoint, strlen(endpoint), ZMQ_SNDMORE);
        zmq_send(sock, e->ctrl_endpoint ? e->ctrl_endpoint : "", e->ctrl_endpoint ? strlen(e->ctrl_endpoint) : 0, ZMQ_SNDMORE);
        zmq_send(sock, e->host ? e->host : "", e->host ? strlen(e->host) : 0, ZMQ_SNDMORE);
        zmq_send(sock, pid_buf, strlen(pid_buf), 0);
      }
    } else if (strcmp(cmd, "UNREGISTER") == 0) {
      char name[256] = {0};
      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t nlen = zmq_msg_size(&part);
      if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
      memcpy(name, zmq_msg_data(&part), nlen);
      zmq_msg_close(&part);

      if (entry_remove(b, name) == 0) {
        zmq_send(sock, "OK", 2, 0);
      } else {
        zmq_send(sock, "NOT_FOUND", 9, 0);
      }
    } else if (strcmp(cmd, "METRICS") == 0) {
      char name[256] = {0};
      char role[64] = {0};
      char pub_port_str[32] = {0};
      char push_port_str[32] = {0};
      char pub_bytes_str[32] = {0};
      char sub_bytes_str[32] = {0};
      char push_bytes_str[32] = {0};
      char pull_bytes_str[32] = {0};

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t nlen = zmq_msg_size(&part);
      if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
      memcpy(name, zmq_msg_data(&part), nlen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t rlen = zmq_msg_size(&part);
      if (rlen >= sizeof(role)) rlen = sizeof(role) - 1;
      memcpy(role, zmq_msg_data(&part), rlen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t pplen = zmq_msg_size(&part);
      if (pplen >= sizeof(pub_port_str)) pplen = sizeof(pub_port_str) - 1;
      memcpy(pub_port_str, zmq_msg_data(&part), pplen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t pslen = zmq_msg_size(&part);
      if (pslen >= sizeof(push_port_str)) pslen = sizeof(push_port_str) - 1;
      memcpy(push_port_str, zmq_msg_data(&part), pslen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t pblen = zmq_msg_size(&part);
      if (pblen >= sizeof(pub_bytes_str)) pblen = sizeof(pub_bytes_str) - 1;
      memcpy(pub_bytes_str, zmq_msg_data(&part), pblen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t sblen = zmq_msg_size(&part);
      if (sblen >= sizeof(sub_bytes_str)) sblen = sizeof(sub_bytes_str) - 1;
      memcpy(sub_bytes_str, zmq_msg_data(&part), sblen);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t pb2len = zmq_msg_size(&part);
      if (pb2len >= sizeof(push_bytes_str)) pb2len = sizeof(push_bytes_str) - 1;
      memcpy(push_bytes_str, zmq_msg_data(&part), pb2len);
      zmq_msg_close(&part);

      zmq_msg_init(&part);
      rc = zmq_msg_recv(&part, sock, 0);
      if (rc < 0) { zmq_msg_close(&part); continue; }
      size_t pllen = zmq_msg_size(&part);
      if (pllen >= sizeof(pull_bytes_str)) pllen = sizeof(pull_bytes_str) - 1;
      memcpy(pull_bytes_str, zmq_msg_data(&part), pllen);
      zmq_msg_close(&part);

      struct zcm_broker_entry *e = entry_find(b, name);
      if (!e) {
        zmq_send(sock, "NOT_FOUND", 9, 0);
      } else {
        if (role[0] && strcmp(role, "-") != 0) {
          if (role_is_valid(role) ||
              strcmp(role, "UNKNOWN") == 0 ||
              strcmp(role, "BROKER") == 0 ||
              strcmp(role, "EXTERNAL") == 0) {
            snprintf(e->role, sizeof(e->role), "%s", role);
          }
        }
        int v = -1;
        if (parse_int_text(pub_port_str, &v) == 0) e->pub_port = v;
        if (parse_int_text(push_port_str, &v) == 0) e->push_port = v;
        if (parse_int_text(pub_bytes_str, &v) == 0) e->pub_bytes = v;
        if (parse_int_text(sub_bytes_str, &v) == 0) e->sub_bytes = v;
        if (parse_int_text(push_bytes_str, &v) == 0) e->push_bytes = v;
        if (parse_int_text(pull_bytes_str, &v) == 0) e->pull_bytes = v;
        zmq_send(sock, "OK", 2, 0);
      }
    } else if (strcmp(cmd, "PING") == 0) {
      zmq_send(sock, "PONG", 4, 0);
    } else if (strcmp(cmd, "STOP") == 0) {
      zmq_send(sock, "OK", 2, 0);
      b->running = 0;
    } else if (strcmp(cmd, "LIST_EX") == 0) {
      int count = 0;
      for (struct zcm_broker_entry *e = b->head; e; e = e->next) count++;
      zmq_send(sock, "OK", 2, ZMQ_SNDMORE);
      zmq_send(sock, &count, sizeof(count), (count > 0) ? ZMQ_SNDMORE : 0);
      int idx = 0;
      for (struct zcm_broker_entry *e = b->head; e; e = e->next) {
        entry_refresh_metrics(b, e);
        char endpoint[512] = {0};
        char pub_port[32];
        char push_port[32];
        char pub_bytes[32];
        char sub_bytes[32];
        char push_bytes[32];
        char pull_bytes[32];
        entry_effective_endpoint(e, endpoint, sizeof(endpoint));
        snprintf(pub_port, sizeof(pub_port), "%d", e->pub_port);
        snprintf(push_port, sizeof(push_port), "%d", e->push_port);
        snprintf(pub_bytes, sizeof(pub_bytes), "%d", e->pub_bytes);
        snprintf(sub_bytes, sizeof(sub_bytes), "%d", e->sub_bytes);
        snprintf(push_bytes, sizeof(push_bytes), "%d", e->push_bytes);
        snprintf(pull_bytes, sizeof(pull_bytes), "%d", e->pull_bytes);

        int final_flags = (idx < count - 1) ? ZMQ_SNDMORE : 0;
        zmq_send(sock, e->name ? e->name : "", e->name ? strlen(e->name) : 0, ZMQ_SNDMORE);
        zmq_send(sock, endpoint, strlen(endpoint), ZMQ_SNDMORE);
        zmq_send(sock, e->host ? e->host : "", e->host ? strlen(e->host) : 0, ZMQ_SNDMORE);
        zmq_send(sock, e->role, strlen(e->role), ZMQ_SNDMORE);
        zmq_send(sock, pub_port, strlen(pub_port), ZMQ_SNDMORE);
        zmq_send(sock, push_port, strlen(push_port), ZMQ_SNDMORE);
        zmq_send(sock, pub_bytes, strlen(pub_bytes), ZMQ_SNDMORE);
        zmq_send(sock, sub_bytes, strlen(sub_bytes), ZMQ_SNDMORE);
        zmq_send(sock, push_bytes, strlen(push_bytes), ZMQ_SNDMORE);
        zmq_send(sock, pull_bytes, strlen(pull_bytes), final_flags);
        idx++;
      }
    } else if (strcmp(cmd, "LIST") == 0) {
      int count = 0;
      for (struct zcm_broker_entry *e = b->head; e; e = e->next) count++;
      zmq_send(sock, "OK", 2, ZMQ_SNDMORE);
      zmq_send(sock, &count, sizeof(count), (count > 0) ? ZMQ_SNDMORE : 0);
      int idx = 0;
      for (struct zcm_broker_entry *e = b->head; e; e = e->next) {
        char endpoint[512] = {0};
        entry_effective_endpoint(e, endpoint, sizeof(endpoint));
        int more = (idx < count - 1) ? ZMQ_SNDMORE : 0;
        zmq_send(sock, e->name ? e->name : "", e->name ? strlen(e->name) : 0, ZMQ_SNDMORE);
        zmq_send(sock, endpoint, strlen(endpoint), more);
        idx++;}
    } else {
      zmq_send(sock, "ERR", 3, 0);
    }
  }

  zmq_close(sock);
  return NULL;
}

zcm_broker_t *zcm_broker_start(zcm_context_t *ctx, const char *endpoint) {
  if (!ctx || !endpoint) return NULL;
  zcm_broker_t *b = (zcm_broker_t *)calloc(1, sizeof(zcm_broker_t));
  if (!b) return NULL;
  b->ctx = ctx;
  b->endpoint = strdup(endpoint);
  if (!b->endpoint) { free(b); return NULL; }
  /* Always register the broker itself so names list is never empty. */
  entry_set(b, "zcmbroker", b->endpoint);
  b->running = 1;
  if (pthread_create(&b->thread, NULL, broker_thread, b) != 0) {
    free(b->endpoint);
    free(b);
    return NULL;
  }
  return b;
}

static void broker_request_stop(zcm_broker_t *broker) {
  if (!broker || !broker->ctx || !broker->endpoint) return;
  void *sock = zmq_socket(zcm_context_zmq(broker->ctx), ZMQ_REQ);
  if (!sock) return;
  int to = 1000;
  int linger = 0;
  int immediate = 1;
  zmq_setsockopt(sock, ZMQ_RCVTIMEO, &to, sizeof(to));
  zmq_setsockopt(sock, ZMQ_SNDTIMEO, &to, sizeof(to));
  zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
  zmq_setsockopt(sock, ZMQ_IMMEDIATE, &immediate, sizeof(immediate));
  if (zmq_connect(sock, broker->endpoint) != 0) {
    zmq_close(sock);
    return;
  }
  if (zmq_send(sock, "STOP", 4, 0) < 0) {
    zmq_close(sock);
    return;
  }
  char reply[16] = {0};
  zmq_recv(sock, reply, sizeof(reply) - 1, 0);
  zmq_close(sock);
}

void zcm_broker_stop(zcm_broker_t *broker) {
  if (!broker) return;
  if (broker->running) broker_request_stop(broker);
  broker->running = 0;
  pthread_join(broker->thread, NULL);
  struct zcm_broker_entry *e = broker->head;
  while (e) {
    struct zcm_broker_entry *n = e->next;
    entry_free(e);
    e = n;
  }
  free(broker->endpoint);
  free(broker);
}

int zcm_broker_is_running(const zcm_broker_t *broker) {
  if (!broker) return 0;
  return broker->running ? 1 : 0;
}
