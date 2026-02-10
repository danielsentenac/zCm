#include "zcm/zcm.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include <zmq.h>

struct zcm_broker_entry {
  char *name;
  char *endpoint;
  char *ctrl_endpoint;
  char *host;
  int pid;
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
        zmq_send(sock, "OK", 2, ZMQ_SNDMORE);
        zmq_send(sock, e->endpoint, strlen(e->endpoint), 0);
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
        snprintf(pid_buf, sizeof(pid_buf), "%d", e->pid);
        zmq_send(sock, "OK", 2, ZMQ_SNDMORE);
        zmq_send(sock, e->endpoint ? e->endpoint : "", e->endpoint ? strlen(e->endpoint) : 0, ZMQ_SNDMORE);
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
    } else if (strcmp(cmd, "PING") == 0) {
      zmq_send(sock, "PONG", 4, 0);
    } else if (strcmp(cmd, "STOP") == 0) {
      zmq_send(sock, "OK", 2, 0);
      b->running = 0;
    } else if (strcmp(cmd, "LIST") == 0) {
      int count = 0;
      for (struct zcm_broker_entry *e = b->head; e; e = e->next) count++;
      zmq_send(sock, "OK", 2, ZMQ_SNDMORE);
      zmq_send(sock, &count, sizeof(count), ZMQ_SNDMORE);
      int idx = 0;
      for (struct zcm_broker_entry *e = b->head; e; e = e->next) {
        int more = (idx < count - 1) ? ZMQ_SNDMORE : 0;
        zmq_send(sock, e->name, strlen(e->name), ZMQ_SNDMORE);
        zmq_send(sock, e->endpoint, strlen(e->endpoint), more);
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
