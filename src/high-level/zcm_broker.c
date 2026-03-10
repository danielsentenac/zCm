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
#include <time.h>

#include <zmq.h>

struct zcm_broker_entry {
  char *name;
  char *endpoint;
  char *ctrl_endpoint;
  char *host;
  int pid;
  uint64_t remote_probe_at_ms;
  int remote_probe_failures;
  char role[512];
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
  int remote_probe_interval_ms;
  int remote_probe_failures_before_drop;
  int trace_reg;
  pthread_t thread;
  int running;
  struct zcm_broker_entry *head;
};

static int entry_remove(struct zcm_broker *b, const char *name);
static struct zcm_broker_entry *entry_find(struct zcm_broker *b, const char *name);
static void entry_effective_endpoint(const struct zcm_broker_entry *e,
                                     char *out_endpoint,
                                     size_t out_size);
static int query_proc_ping_ok(zcm_context_t *ctx, const char *endpoint);
static int host_is_local(const char *host);
static int host_equivalent(const char *a, const char *b);
static int build_tcp_endpoint_text(const char *host, int port,
                                   char *out, size_t out_size);

#define ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS_DEFAULT 3000
#define ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS_MIN 250
#define ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS_MAX 120000
#define ZCM_BROKER_REMOTE_PROBE_FAILS_DEFAULT 3
#define ZCM_BROKER_REMOTE_PROBE_FAILS_MIN 1
#define ZCM_BROKER_REMOTE_PROBE_FAILS_MAX 20
#define ZCM_BROKER_STOP_ACK_GRACE_US 100000

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

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static int parse_remote_probe_interval_ms_from_env(void) {
  const char *env = getenv("ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS");
  if (!env || !*env) return ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS_DEFAULT;
  char *end = NULL;
  long v = strtol(env, &end, 10);
  if (!end || *end != '\0') return ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS_DEFAULT;
  if (v < ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS_MIN ||
      v > ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS_MAX) {
    return ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS_DEFAULT;
  }
  return (int)v;
}

static int parse_remote_probe_fails_from_env(void) {
  const char *env = getenv("ZCM_BROKER_REMOTE_PROBE_FAILS");
  if (!env || !*env) return ZCM_BROKER_REMOTE_PROBE_FAILS_DEFAULT;
  char *end = NULL;
  long v = strtol(env, &end, 10);
  if (!end || *end != '\0') return ZCM_BROKER_REMOTE_PROBE_FAILS_DEFAULT;
  if (v < ZCM_BROKER_REMOTE_PROBE_FAILS_MIN ||
      v > ZCM_BROKER_REMOTE_PROBE_FAILS_MAX) {
    return ZCM_BROKER_REMOTE_PROBE_FAILS_DEFAULT;
  }
  return (int)v;
}

static int parse_bool_env_default0(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v) return 0;
  if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0) return 0;
  return 1;
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

static int endpoint_rewrite_host(const char *endpoint,
                                 const char *new_host,
                                 char *out_endpoint,
                                 size_t out_size) {
  char ep_host[256] = {0};
  int ep_port = 0;
  if (!endpoint || !new_host || !*new_host || !out_endpoint || out_size == 0) return -1;
  if (endpoint_tcp_parse_host_port(endpoint, ep_host, sizeof(ep_host), &ep_port) != 0) return -1;
  return build_tcp_endpoint_text(new_host, ep_port, out_endpoint, out_size);
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

static int peer_address_parse_host(const char *peer_addr,
                                   char *out_host, size_t out_host_size) {
  if (!peer_addr || !*peer_addr || !out_host || out_host_size == 0) return -1;
  out_host[0] = '\0';

  if (strncmp(peer_addr, "tcp://", 6) == 0) {
    int port = 0;
    if (endpoint_tcp_parse_host_port(peer_addr, out_host, out_host_size, &port) == 0) return 0;
    return -1;
  }

  const char *p = peer_addr;
  const char *host_begin = p;
  const char *host_end = NULL;
  if (*p == '[') {
    host_begin = p + 1;
    host_end = strchr(host_begin, ']');
    if (!host_end || host_end <= host_begin) return -1;
  } else {
    host_end = strrchr(p, ':');
    if (!host_end || host_end <= host_begin) return -1;
  }

  size_t host_len = (size_t)(host_end - host_begin);
  if (host_len == 0) return -1;
  if (host_len >= out_host_size) host_len = out_host_size - 1;
  memcpy(out_host, host_begin, host_len);
  out_host[host_len] = '\0';
  return 0;
}

static void extract_peer_host_from_msg(const zmq_msg_t *msg,
                                       char *out_host, size_t out_host_size) {
  if (!out_host || out_host_size == 0) return;
  out_host[0] = '\0';
  if (!msg) return;

  const char *peer_addr = zmq_msg_gets((zmq_msg_t *)msg, "Peer-Address");
  if (!peer_addr || !*peer_addr) return;
  (void)peer_address_parse_host(peer_addr, out_host, out_host_size);
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

static void entry_reqrep_endpoint(const struct zcm_broker_entry *e,
                                  char *out_endpoint,
                                  size_t out_size) {
  if (!out_endpoint || out_size == 0) return;
  out_endpoint[0] = '\0';
  if (!e) return;

  if (e->ctrl_endpoint && e->ctrl_endpoint[0]) {
    snprintf(out_endpoint, out_size, "%s", e->ctrl_endpoint);
    return;
  }
  entry_effective_endpoint(e, out_endpoint, out_size);
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

static char *trim_ascii_ws_inplace(char *text) {
  if (!text) return NULL;
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
  size_t n = strlen(text);
  while (n > 0) {
    char c = text[n - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      text[--n] = '\0';
    } else {
      break;
    }
  }
  return text;
}

static void csv_append_token(char *csv, size_t csv_size, const char *token) {
  if (!csv || csv_size == 0 || !token || !token[0]) return;
  if (strstr(csv, token) != NULL) return;
  if (csv[0]) strncat(csv, ",", csv_size - strlen(csv) - 1);
  strncat(csv, token, csv_size - strlen(csv) - 1);
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
    if (len >= token_len && strncmp(p, token, token_len) == 0) {
      if (len == token_len || p[token_len] == ':') return 1;
    }
    if (!next) break;
    p = next + 1;
  }
  return 0;
}

static void role_add_token(char *role, size_t role_size, const char *token) {
  if (!role || role_size == 0 || !token || !*token) return;
  if (strcmp(role, "UNKNOWN") == 0 || strcmp(role, "NONE") == 0 || role[0] == '\0') {
    snprintf(role, role_size, "%s", token);
    return;
  }
  if (role_contains_token(role, token)) return;

  size_t cur = strlen(role);
  size_t add = strlen(token) + 1;
  if (cur + add >= role_size) return;
  role[cur] = '+';
  role[cur + 1] = '\0';
  strncat(role, token, role_size - strlen(role) - 1);
}

static void infer_publisher_from_sub_target(struct zcm_broker *b, const char *target) {
  if (!b || !target || !target[0]) return;

  char copy[256] = {0};
  char key[256] = {0};
  int port = -1;
  snprintf(copy, sizeof(copy), "%s", target);
  char *last_colon = strrchr(copy, ':');
  if (last_colon && last_colon[1]) {
    int parsed_port = -1;
    if (parse_int_text(last_colon + 1, &parsed_port) == 0 &&
        parsed_port >= 1 && parsed_port <= 65535) {
      *last_colon = '\0';
      snprintf(key, sizeof(key), "%s", copy);
      port = parsed_port;
    }
  }
  if (key[0] == '\0') snprintf(key, sizeof(key), "%s", copy);

  if (key[0]) {
    struct zcm_broker_entry *by_name = entry_find(b, key);
    if (by_name) {
      if (port > 0) by_name->pub_port = port;
      role_add_token(by_name->role, sizeof(by_name->role), "PUB");
      return;
    }
  }

  if (port <= 0) return;
  struct zcm_broker_entry *match = NULL;
  int matches = 0;
  for (struct zcm_broker_entry *e = b->head; e; e = e->next) {
    char endpoint[512] = {0};
    char host[256] = {0};
    int ep_port = 0;
    entry_effective_endpoint(e, endpoint, sizeof(endpoint));
    if (endpoint_tcp_parse_host_port(endpoint, host, sizeof(host), &ep_port) != 0) continue;
    if (ep_port != port) continue;
    if (key[0] && !host_equivalent(host, key)) continue;
    match = e;
    matches++;
    if (matches > 1) return;
  }
  if (matches == 1 && match) {
    match->pub_port = port;
    role_add_token(match->role, sizeof(match->role), "PUB");
  }
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
          (len >= 4 && strncmp(p, "SUB:", 4) == 0) ||
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

static int host_equivalent(const char *a, const char *b) {
  if (!a || !b || !*a || !*b) return 0;
  if (strcasecmp(a, b) == 0) return 1;

  char a_short[256] = {0};
  char b_short[256] = {0};
  snprintf(a_short, sizeof(a_short), "%s", a);
  snprintf(b_short, sizeof(b_short), "%s", b);
  char *dot = strchr(a_short, '.');
  if (dot) *dot = '\0';
  dot = strchr(b_short, '.');
  if (dot) *dot = '\0';
  if (!a_short[0] || !b_short[0]) return 0;
  return strcasecmp(a_short, b_short) == 0;
}

static const char *prefer_host_for_registration(const char *advertised_host,
                                                const char *peer_host) {
  if (!peer_host || !*peer_host) return advertised_host;
  if (!advertised_host || !*advertised_host) return peer_host;
  if (host_equivalent(advertised_host, peer_host)) return advertised_host;

  if (host_is_local(advertised_host) && !host_is_local(peer_host)) return peer_host;
  if (endpoint_host_needs_resolution(advertised_host) &&
      !endpoint_host_needs_resolution(peer_host)) {
    return peer_host;
  }
  return advertised_host;
}

static void normalize_registration_endpoint_host(const char *endpoint,
                                                 const char *advertised_host,
                                                 const char *effective_host,
                                                 char *out_endpoint,
                                                 size_t out_size) {
  if (!out_endpoint || out_size == 0) return;
  out_endpoint[0] = '\0';
  if (!endpoint || !*endpoint) return;
  snprintf(out_endpoint, out_size, "%s", endpoint);
  if (!effective_host || !*effective_host) return;

  char ep_host[256] = {0};
  int ep_port = 0;
  if (endpoint_tcp_parse_host_port(endpoint, ep_host, sizeof(ep_host), &ep_port) != 0) return;
  if (host_equivalent(ep_host, effective_host)) return;

  if (advertised_host && *advertised_host &&
      host_equivalent(ep_host, advertised_host) &&
      !host_equivalent(advertised_host, effective_host)) {
    (void)endpoint_rewrite_host(endpoint, effective_host, out_endpoint, out_size);
    return;
  }

  if (host_is_local(ep_host) && !host_is_local(effective_host)) {
    (void)endpoint_rewrite_host(endpoint, effective_host, out_endpoint, out_size);
  }
}

static int host_is_loopback_literal(const char *host) {
  if (!host || !*host) return 0;
  return (strcmp(host, "127.0.0.1") == 0 ||
          strcmp(host, "::1") == 0 ||
          strcasecmp(host, "localhost") == 0);
}

static int pid_is_alive_local(int pid) {
  if (pid <= 0) return 0;
  if (kill((pid_t)pid, 0) == 0) return 1;
  return errno == EPERM;
}

static int entry_is_stale_local(const struct zcm_broker_entry *e) {
  if (!e || e->pid <= 0) return 0;
  /* PID liveness is reliable only for explicit loopback-advertised entries.
   * Hostname-based "local" processes can run in different PID namespaces. */
  if (!host_is_loopback_literal(e->host)) return 0;
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
 * Register a broker-internal entry.
 * Return codes:
 *   0  success (new or idempotent same endpoint)
 *   1  duplicate name with conflicting endpoint/owner
 *  -1  allocation/internal error
 */
static int entry_set(struct zcm_broker *b, const char *name, const char *endpoint) {
  const char *host = "127.0.0.1";
  int pid = (int)getpid();
  struct zcm_broker_entry *e = entry_find(b, name);
  if (e) {
    if (e->endpoint && strcmp(e->endpoint, endpoint) == 0 &&
        e->ctrl_endpoint && strcmp(e->ctrl_endpoint, endpoint) == 0 &&
        e->host && strcmp(e->host, host) == 0 &&
        e->pid == pid) {
      return 0;
    }
    return 1;
  }

  e = (struct zcm_broker_entry *)calloc(1, sizeof(*e));
  if (!e) return -1;
  e->name = strdup(name);
  e->host = strdup(host);
  if (!e->name) {
    free(e->host);
    free(e);
    return -1;
  }
  e->endpoint = strdup(endpoint);
  e->ctrl_endpoint = strdup(endpoint);
  if (!e->endpoint || !e->ctrl_endpoint || !e->host) {
    free(e->name);
    free(e->endpoint);
    free(e->ctrl_endpoint);
    free(e->host);
    free(e);
    return -1;
  }
  e->pid = pid;
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
                        const char *ctrl_endpoint, const char *host, int pid,
                        const char *role, int pub_port, int push_port) {
  if (!b || !name || !*name || !endpoint || !*endpoint ||
      !ctrl_endpoint || !*ctrl_endpoint || !host || !*host ||
      pid <= 0 || !role || !*role) {
    return -1;
  }

  struct zcm_broker_entry *e = entry_find(b, name);
  if (e && entry_is_stale_local(e)) {
    (void)entry_remove(b, name);
    e = NULL;
  }
  if (e) {
    /* Accept same-name takeover when host matches, even if PID changed.
     * This avoids permanent DUPLICATE locks after process restart/crash. */
    if (!(e->host && host && host_equivalent(e->host, host))) {
      return 1;
    }
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
  snprintf(e->role, sizeof(e->role), "%s", role);
  e->pub_port = (pub_port > 0 ? pub_port : -1);
  e->push_port = (push_port > 0 ? push_port : -1);
  e->remote_probe_at_ms = 0;
  e->remote_probe_failures = 0;
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

/*
 * Remove only if caller peer is compatible with current owner host.
 * Return codes:
 *   0  removed
 *   1  rejected (peer not owner)
 *  -1  not found
 */
static int entry_remove_by_peer(struct zcm_broker *b,
                                const char *name,
                                const char *peer_host) {
  struct zcm_broker_entry *e = NULL;
  if (!b || !name || !*name) return -1;
  e = entry_find(b, name);
  if (!e) return -1;

  if (!e->host || !e->host[0] || !peer_host || !peer_host[0]) {
    return entry_remove(b, name);
  }
  if (host_equivalent(e->host, peer_host)) {
    return entry_remove(b, name);
  }

  /* Protect remote-owned entries from local unregister noise. */
  if (!host_is_local(e->host) && host_is_local(peer_host)) {
    return 1;
  }
  /* Cross-remote mismatch is also rejected. */
  if (!host_is_local(e->host) && !host_is_local(peer_host)) {
    return 1;
  }

  return entry_remove(b, name);
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
  zcm_socket_set_timeouts(req, 50);
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
    /* Broker-side metric probing must not fall back to data endpoint when a
     * control endpoint is declared: data endpoint can be non-REP and stall. */
    return query_proc_command(ctx, ctrl_endpoint, cmd, out_text, out_text_size, out_code);
  }
  if (!endpoint || !*endpoint) return -1;
  return query_proc_command(ctx, endpoint, cmd, out_text, out_text_size, out_code);
}

static int entry_prune_stale_remote_ctrl(struct zcm_broker *b) {
  int removed = 0;
  uint64_t now_ms = monotonic_ms();
  struct zcm_broker_entry *prev = NULL;
  struct zcm_broker_entry *e = NULL;

  if (!b) return 0;
  e = b->head;
  while (e) {
    int should_check = 0;
    char probe_host[256] = {0};
    int probe_port = 0;
    char probe_endpoint[512] = {0};

    if (strcmp(e->name, "zcmbroker") != 0 &&
        e->pid > 0 &&
        e->ctrl_endpoint && e->ctrl_endpoint[0] &&
        endpoint_is_queryable(e->ctrl_endpoint)) {
      should_check = 1;
      snprintf(probe_endpoint, sizeof(probe_endpoint), "%s", e->ctrl_endpoint);
    }

    if (should_check) {
      if (e->host && e->host[0]) {
        snprintf(probe_host, sizeof(probe_host), "%s", e->host);
      } else if (endpoint_tcp_parse_host_port(probe_endpoint, probe_host, sizeof(probe_host), &probe_port) == 0) {
        (void)probe_port;
      } else if (e->endpoint &&
                 endpoint_tcp_parse_host_port(e->endpoint, probe_host, sizeof(probe_host), &probe_port) == 0) {
        (void)probe_port;
      }

      if (probe_host[0] && !host_is_local(probe_host)) {
        if (b->remote_probe_interval_ms > 0 &&
            now_ms != 0 &&
            e->remote_probe_at_ms != 0 &&
            now_ms > e->remote_probe_at_ms &&
            (now_ms - e->remote_probe_at_ms) < (uint64_t)b->remote_probe_interval_ms) {
          prev = e;
          e = e->next;
          continue;
        }
        if (now_ms != 0) e->remote_probe_at_ms = now_ms;
        if (!query_proc_ping_ok(b->ctx, probe_endpoint)) {
          if (e->remote_probe_failures < INT_MAX) e->remote_probe_failures++;
          if (e->remote_probe_failures < b->remote_probe_failures_before_drop) {
            prev = e;
            e = e->next;
            continue;
          }
          struct zcm_broker_entry *dead = e;
          if (prev) prev->next = e->next;
          else b->head = e->next;
          e = e->next;
          entry_free(dead);
          removed++;
          continue;
        }
        e->remote_probe_failures = 0;
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
    }
  }

  if ((!e->ctrl_endpoint || !e->ctrl_endpoint[0]) &&
      (!e->endpoint || !*e->endpoint)) {
    return;
  }

  if (!e->ctrl_endpoint || !e->ctrl_endpoint[0]) {
    return;
  }

  {
    int role_known = (e->role[0] && strcmp(e->role, "UNKNOWN") != 0 && strcmp(e->role, "NONE") != 0);
    int role_pub_complete = !role_contains_token(e->role, "PUB") || e->pub_port > 0;
    int role_push_complete = !role_contains_token(e->role, "PUSH") || e->push_port > 0;
    int role_sub = role_contains_token(e->role, "SUB");
    if (role_known && role_pub_complete && role_push_complete && !role_sub) {
      return;
    }
  }

  char text[128] = {0};
  int code = 0;
  int port = -1;

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_ROLE", text, sizeof(text), &code) == 0 &&
      code == 200 && role_is_valid(text)) {
    snprintf(e->role, sizeof(e->role), "%s", text);
  }

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_PORT_PUB", text, sizeof(text), &code) == 0 &&
      code == 200) {
    if (parse_int_text(text, &port) == 0 && port > 0) e->pub_port = port;
  } else if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                                   "DATA_PORT", text, sizeof(text), &code) == 0 &&
             code == 200) {
    if (parse_int_text(text, &port) == 0 && port > 0) e->pub_port = port;
  }

  if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                            "DATA_PORT_PUSH", text, sizeof(text), &code) == 0 &&
      code == 200) {
    if (parse_int_text(text, &port) == 0 && port > 0) e->push_port = port;
  }

  if (e->pub_port > 0) role_add_token(e->role, sizeof(e->role), "PUB");
  if (e->push_port > 0) role_add_token(e->role, sizeof(e->role), "PUSH");
  if (e->endpoint && endpoint_has_scheme(e->endpoint, "sub://")) {
    role_add_token(e->role, sizeof(e->role), "SUB");
  }

  if (role_contains_token(e->role, "SUB")) {
    char targets_reply[512] = {0};
    if (query_proc_command_with_ctrl_fallback(b->ctx, e->endpoint, e->ctrl_endpoint,
                                              "DATA_SUB_TARGETS",
                                              targets_reply, sizeof(targets_reply), &code) == 0 &&
        code == 200 && targets_reply[0]) {
      char decorated[sizeof(e->role)];
      char copy[sizeof(targets_reply)];
      decorated[0] = '\0';
      snprintf(copy, sizeof(copy), "%s", targets_reply);
      char *saveptr = NULL;
      for (char *tok = strtok_r(copy, ",;", &saveptr);
           tok;
           tok = strtok_r(NULL, ",;", &saveptr)) {
        char *item = trim_ascii_ws_inplace(tok);
        if (!item || !item[0]) continue;
        if (strncmp(item, "SUB:", 4) == 0) item += 4;
        item = trim_ascii_ws_inplace(item);
        if (!item || !item[0]) continue;

        char role_item[320] = {0};
        snprintf(role_item, sizeof(role_item), "SUB:%s", item);
        csv_append_token(decorated, sizeof(decorated), role_item);
        infer_publisher_from_sub_target(b, item);
      }
      if (decorated[0]) snprintf(e->role, sizeof(e->role), "%s", decorated);
    }
  }
}

static int broker_sock_has_more(void *sock) {
  int64_t more = 0;
  size_t more_size = sizeof(more);
  if (!sock) return 0;
  if (zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size) != 0) return 0;
  return more ? 1 : 0;
}

static void broker_sock_drain_remaining_parts(void *sock) {
  while (broker_sock_has_more(sock)) {
    zmq_msg_t part;
    zmq_msg_init(&part);
    if (zmq_msg_recv(&part, sock, 0) < 0) {
      zmq_msg_close(&part);
      break;
    }
    zmq_msg_close(&part);
  }
}

static int broker_recv_part_text(void *sock, char *out, size_t out_size) {
  if (!sock || !out || out_size == 0) return -1;
  out[0] = '\0';
  if (!broker_sock_has_more(sock)) return -1;

  zmq_msg_t part;
  zmq_msg_init(&part);
  if (zmq_msg_recv(&part, sock, 0) < 0) {
    zmq_msg_close(&part);
    return -1;
  }
  size_t n = zmq_msg_size(&part);
  if (n >= out_size) n = out_size - 1;
  memcpy(out, zmq_msg_data(&part), n);
  out[n] = '\0';
  zmq_msg_close(&part);
  return 0;
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
    char peer_host[256] = {0};
    extract_peer_host_from_msg(&part, peer_host, sizeof(peer_host));
    char cmd[32] = {0};
    size_t cmd_len = zmq_msg_size(&part);
    if (cmd_len >= sizeof(cmd)) cmd_len = sizeof(cmd) - 1;
    memcpy(cmd, zmq_msg_data(&part), cmd_len);
    zmq_msg_close(&part);

#define RECV_PART_OR_REPLY_ERR(dst)                                            \
    do {                                                                        \
      if (broker_recv_part_text(sock, (dst), sizeof(dst)) != 0) {              \
        broker_sock_drain_remaining_parts(sock);                                \
        zmq_send(sock, "ERR_MALFORMED", 13, 0);                                \
        continue;                                                               \
      }                                                                         \
    } while (0)

    (void)entry_prune_stale_local(b);

    if (strcmp(cmd, "REGISTER") == 0) {
      char name[256] = {0};
      char endpoint[512] = {0};
      RECV_PART_OR_REPLY_ERR(name);
      RECV_PART_OR_REPLY_ERR(endpoint);
      broker_sock_drain_remaining_parts(sock);

      if (b->trace_reg) {
        fprintf(stderr, "zcm_broker: REGISTER name=%s peer=%s endpoint=%s rc=UNSUPPORTED\n",
                name,
                (peer_host[0] ? peer_host : "-"),
                endpoint);
      }
      zmq_send(sock, "ERR_UNSUPPORTED", 15, 0);
    } else if (strcmp(cmd, "REGISTER_EX") == 0) {
      char name[256] = {0};
      char endpoint[512] = {0};
      char ctrl_ep[512] = {0};
      char host[256] = {0};
      char pid_str[32] = {0};
      char role[64] = {0};
      char pub_port_str[32] = {0};
      char push_port_str[32] = {0};
      RECV_PART_OR_REPLY_ERR(name);
      RECV_PART_OR_REPLY_ERR(endpoint);
      RECV_PART_OR_REPLY_ERR(ctrl_ep);
      RECV_PART_OR_REPLY_ERR(host);
      RECV_PART_OR_REPLY_ERR(pid_str);
      RECV_PART_OR_REPLY_ERR(role);
      RECV_PART_OR_REPLY_ERR(pub_port_str);
      RECV_PART_OR_REPLY_ERR(push_port_str);
      broker_sock_drain_remaining_parts(sock);

      const char *effective_host = prefer_host_for_registration(host, peer_host);
      char effective_endpoint[512] = {0};
      char effective_ctrl_ep[512] = {0};
      normalize_registration_endpoint_host(endpoint, host, effective_host,
                                           effective_endpoint, sizeof(effective_endpoint));
      normalize_registration_endpoint_host(ctrl_ep, host, effective_host,
                                           effective_ctrl_ep, sizeof(effective_ctrl_ep));
      int pid = 0;
      int pub_port = -1;
      int push_port = -1;
      if (parse_int_text(pid_str, &pid) != 0 || pid <= 0 ||
          !role[0] || !role_is_valid(role) ||
          parse_int_text(pub_port_str, &pub_port) != 0 ||
          parse_int_text(push_port_str, &push_port) != 0 ||
          pub_port > 65535 || push_port > 65535) {
        if (b->trace_reg) {
          fprintf(stderr,
                  "zcm_broker: REGISTER_EX name=%s peer=%s adv_host=%s endpoint=%s ctrl=%s pid=%s role=%s pub_port=%s push_port=%s rc=ERR_MALFORMED\n",
                  name,
                  (peer_host[0] ? peer_host : "-"),
                  (host[0] ? host : "-"),
                  endpoint,
                  ctrl_ep,
                  (pid_str[0] ? pid_str : "-"),
                  (role[0] ? role : "-"),
                  (pub_port_str[0] ? pub_port_str : "-"),
                  (push_port_str[0] ? push_port_str : "-"));
        }
        zmq_send(sock, "ERR_MALFORMED", 13, 0);
        continue;
      }
      if (pub_port <= 0) pub_port = -1;
      if (push_port <= 0) push_port = -1;
      if ((role_contains_token(role, "PUB") && pub_port <= 0) ||
          (role_contains_token(role, "PUSH") && push_port <= 0)) {
        if (b->trace_reg) {
          fprintf(stderr,
                  "zcm_broker: REGISTER_EX name=%s peer=%s adv_host=%s endpoint=%s ctrl=%s pid=%d role=%s pub_port=%d push_port=%d rc=ERR_MALFORMED\n",
                  name,
                  (peer_host[0] ? peer_host : "-"),
                  (host[0] ? host : "-"),
                  endpoint,
                  ctrl_ep,
                  pid,
                  role,
                  pub_port,
                  push_port);
        }
        zmq_send(sock, "ERR_MALFORMED", 13, 0);
        continue;
      }
      int reg_rc = entry_set_ex(b, name, effective_endpoint[0] ? effective_endpoint : endpoint,
                                effective_ctrl_ep[0] ? effective_ctrl_ep : ctrl_ep,
                                effective_host, pid, role, pub_port, push_port);
      if (b->trace_reg) {
        fprintf(stderr,
                "zcm_broker: REGISTER_EX name=%s peer=%s adv_host=%s eff_host=%s endpoint=%s ctrl=%s pid=%d role=%s pub_port=%d push_port=%d rc=%s\n",
                name,
                (peer_host[0] ? peer_host : "-"),
                (host[0] ? host : "-"),
                (effective_host && *effective_host ? effective_host : "-"),
                (effective_endpoint[0] ? effective_endpoint : endpoint),
                (effective_ctrl_ep[0] ? effective_ctrl_ep : ctrl_ep),
                pid,
                (role[0] ? role : "UNKNOWN"),
                pub_port,
                push_port,
                (reg_rc == 0 ? "OK" : (reg_rc == 1 ? "DUPLICATE" : "ERR")));
      }
      if (reg_rc == 0) {
        zmq_send(sock, "OK", 2, 0);
      } else if (reg_rc == 1) {
        zmq_send(sock, "DUPLICATE", 9, 0);
      } else {
        zmq_send(sock, "ERR", 3, 0);
      }
    } else if (strcmp(cmd, "LOOKUP") == 0) {
      char name[256] = {0};
      RECV_PART_OR_REPLY_ERR(name);
      broker_sock_drain_remaining_parts(sock);

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
      RECV_PART_OR_REPLY_ERR(name);
      broker_sock_drain_remaining_parts(sock);

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
      RECV_PART_OR_REPLY_ERR(name);
      broker_sock_drain_remaining_parts(sock);

      int unreg_rc = entry_remove_by_peer(b, name, peer_host);
      if (b->trace_reg) {
        fprintf(stderr, "zcm_broker: UNREGISTER name=%s peer=%s rc=%s\n",
                name,
                (peer_host[0] ? peer_host : "-"),
                (unreg_rc == 0 ? "OK" : (unreg_rc == 1 ? "NOT_OWNER" : "NOT_FOUND")));
      }
      if (unreg_rc == 0) {
        zmq_send(sock, "OK", 2, 0);
      } else if (unreg_rc == 1) {
        zmq_send(sock, "NOT_OWNER", 9, 0);
      } else {
        zmq_send(sock, "NOT_FOUND", 9, 0);
      }
    } else if (strcmp(cmd, "METRICS") == 0) {
      char name[256] = {0};
      char role[512] = {0};
      char pub_port_str[32] = {0};
      char push_port_str[32] = {0};
      char pub_bytes_str[32] = {0};
      char sub_bytes_str[32] = {0};
      char push_bytes_str[32] = {0};
      char pull_bytes_str[32] = {0};
      RECV_PART_OR_REPLY_ERR(name);
      RECV_PART_OR_REPLY_ERR(role);
      RECV_PART_OR_REPLY_ERR(pub_port_str);
      RECV_PART_OR_REPLY_ERR(push_port_str);
      RECV_PART_OR_REPLY_ERR(pub_bytes_str);
      RECV_PART_OR_REPLY_ERR(sub_bytes_str);
      RECV_PART_OR_REPLY_ERR(push_bytes_str);
      RECV_PART_OR_REPLY_ERR(pull_bytes_str);
      broker_sock_drain_remaining_parts(sock);

      struct zcm_broker_entry *e = entry_find(b, name);
      if (!e) {
        zmq_send(sock, "NOT_FOUND", 9, 0);
      } else {
        if (role[0] && strcmp(role, "-") != 0 && role_is_valid(role)) {
          snprintf(e->role, sizeof(e->role), "%s", role);
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
      broker_sock_drain_remaining_parts(sock);
      zmq_send(sock, "PONG", 4, 0);
    } else if (strcmp(cmd, "STOP") == 0) {
      broker_sock_drain_remaining_parts(sock);
      if (zmq_send(sock, "OK", 2, 0) >= 0) {
        /* Give the requestor a chance to receive the final ACK before teardown. */
        usleep(ZCM_BROKER_STOP_ACK_GRACE_US);
      }
      b->running = 0;
    } else if (strcmp(cmd, "LIST_EX") == 0) {
      broker_sock_drain_remaining_parts(sock);
      int count = 0;
      for (struct zcm_broker_entry *e = b->head; e; e = e->next) count++;
      zmq_send(sock, "OK", 2, ZMQ_SNDMORE);
      zmq_send(sock, &count, sizeof(count), (count > 0) ? ZMQ_SNDMORE : 0);
      int idx = 0;
      for (struct zcm_broker_entry *e = b->head; e; e = e->next) {
        char endpoint[512] = {0};
        char pub_port[32];
        char push_port[32];
        char pub_bytes[32];
        char sub_bytes[32];
        char push_bytes[32];
        char pull_bytes[32];
        entry_reqrep_endpoint(e, endpoint, sizeof(endpoint));
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
      broker_sock_drain_remaining_parts(sock);
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
      broker_sock_drain_remaining_parts(sock);
      zmq_send(sock, "ERR", 3, 0);
    }

#undef RECV_PART_OR_REPLY_ERR
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
  b->remote_probe_interval_ms = parse_remote_probe_interval_ms_from_env();
  b->remote_probe_failures_before_drop = parse_remote_probe_fails_from_env();
  b->trace_reg = parse_bool_env_default0("ZCM_BROKER_TRACE_REG");
  if (!b->endpoint) { free(b); return NULL; }
  /* Always register the broker itself so names list is never empty. */
  entry_set(b, "zcmbroker", b->endpoint);
  {
    struct zcm_broker_entry *self = entry_find(b, "zcmbroker");
    if (self) snprintf(self->role, sizeof(self->role), "BROKER");
  }
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
