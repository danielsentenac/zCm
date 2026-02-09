#include "zcm/zcm_node.h"
#include "zcm/zcm.h"

#include <stdlib.h>
#include <string.h>

#include <zmq.h>

struct zcm_node {
  zcm_context_t *ctx;
  char *broker_endpoint;
};

struct zcm_socket {
  void *sock;
};

/* from zcm_msg.c */
int zcm_msg__serialize(const zcm_msg_t *msg, const void **data, size_t *len, void **owned);

static int map_socket_type(zcm_socket_type_t type) {
  switch (type) {
    case ZCM_SOCK_REQ: return ZMQ_REQ;
    case ZCM_SOCK_REP: return ZMQ_REP;
    case ZCM_SOCK_PUB: return ZMQ_PUB;
    case ZCM_SOCK_SUB: return ZMQ_SUB;
    case ZCM_SOCK_PAIR: return ZMQ_PAIR;
    case ZCM_SOCK_PUSH: return ZMQ_PUSH;
    case ZCM_SOCK_PULL: return ZMQ_PULL;
    default: return -1;
  }
}

zcm_node_t *zcm_node_new(zcm_context_t *ctx, const char *broker_endpoint) {
  if (!ctx || !broker_endpoint) return NULL;
  zcm_node_t *n = (zcm_node_t *)calloc(1, sizeof(zcm_node_t));
  if (!n) return NULL;
  n->ctx = ctx;
  n->broker_endpoint = strdup(broker_endpoint);
  if (!n->broker_endpoint) {
    free(n);
    return NULL;
  }
  return n;
}

void zcm_node_free(zcm_node_t *node) {
  if (!node) return;
  free(node->broker_endpoint);
  free(node);
}

static int send_frames_req(void *sock, const char *cmd, const char *name, const char *endpoint) {
  if (zmq_send(sock, cmd, strlen(cmd), ZMQ_SNDMORE) < 0) return -1;
  if (zmq_send(sock, name, strlen(name), endpoint ? ZMQ_SNDMORE : 0) < 0) return -1;
  if (endpoint) {
    if (zmq_send(sock, endpoint, strlen(endpoint), 0) < 0) return -1;
  }
  return 0;
}

static void set_req_socket_options(void *sock, int timeout_ms) {
  int linger_ms = 0;
  int immediate = 1;
  zmq_setsockopt(sock, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
  zmq_setsockopt(sock, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
  zmq_setsockopt(sock, ZMQ_LINGER, &linger_ms, sizeof(linger_ms));
  zmq_setsockopt(sock, ZMQ_IMMEDIATE, &immediate, sizeof(immediate));
}

static int recv_with_timeout(void *sock, void *buf, size_t len, int timeout_ms) {
  zmq_pollitem_t items[] = { { sock, 0, ZMQ_POLLIN, 0 } };
  int rc = zmq_poll(items, 1, timeout_ms);
  if (rc <= 0) return -1;
  int n = zmq_recv(sock, buf, len, ZMQ_DONTWAIT);
  return n;
}

int zcm_node_register(zcm_node_t *node, const char *name, const char *endpoint) {
  if (!node || !name || !endpoint) return -1;
  void *sock = zmq_socket(zcm_context_zmq(node->ctx), ZMQ_REQ);
  if (!sock) return -1;
  set_req_socket_options(sock, 1000);
  int rc = zmq_connect(sock, node->broker_endpoint);
  if (rc != 0) {
    zmq_close(sock);
    return -1;
  }
  if (send_frames_req(sock, "REGISTER", name, endpoint) != 0) {
    zmq_close(sock);
    return -1;
  }
  char reply[32] = {0};
  int n = zmq_recv(sock, reply, sizeof(reply) - 1, 0);
  zmq_close(sock);
  if (n <= 0) return -1;
  return (strncmp(reply, "OK", 2) == 0) ? 0 : -1;
}

int zcm_node_unregister(zcm_node_t *node, const char *name) {
  if (!node || !name) return -1;
  void *sock = zmq_socket(zcm_context_zmq(node->ctx), ZMQ_REQ);
  if (!sock) return -1;
  set_req_socket_options(sock, 1000);
  if (zmq_connect(sock, node->broker_endpoint) != 0) {
    zmq_close(sock);
    return -1;
  }
  if (send_frames_req(sock, "UNREGISTER", name, NULL) != 0) {
    zmq_close(sock);
    return -1;
  }
  char reply[32] = {0};
  int n = zmq_recv(sock, reply, sizeof(reply) - 1, 0);
  zmq_close(sock);
  if (n <= 0) return -1;
  return (strncmp(reply, "OK", 2) == 0) ? 0 : -1;
}

int zcm_node_register_ex(zcm_node_t *node, const char *name, const char *endpoint,
                         const char *ctrl_endpoint, const char *host, int pid) {
  if (!node || !name || !endpoint || !ctrl_endpoint || !host) return -1;
  void *sock = zmq_socket(zcm_context_zmq(node->ctx), ZMQ_REQ);
  if (!sock) return -1;
  set_req_socket_options(sock, 1000);
  if (zmq_connect(sock, node->broker_endpoint) != 0) {
    zmq_close(sock);
    return -1;
  }
  if (zmq_send(sock, "REGISTER_EX", 11, ZMQ_SNDMORE) < 0) { zmq_close(sock); return -1; }
  if (zmq_send(sock, name, strlen(name), ZMQ_SNDMORE) < 0) { zmq_close(sock); return -1; }
  if (zmq_send(sock, endpoint, strlen(endpoint), ZMQ_SNDMORE) < 0) { zmq_close(sock); return -1; }
  if (zmq_send(sock, ctrl_endpoint, strlen(ctrl_endpoint), ZMQ_SNDMORE) < 0) { zmq_close(sock); return -1; }
  if (zmq_send(sock, host, strlen(host), ZMQ_SNDMORE) < 0) { zmq_close(sock); return -1; }
  char pid_buf[32];
  snprintf(pid_buf, sizeof(pid_buf), "%d", pid);
  if (zmq_send(sock, pid_buf, strlen(pid_buf), 0) < 0) { zmq_close(sock); return -1; }
  char reply[32] = {0};
  int n = zmq_recv(sock, reply, sizeof(reply) - 1, 0);
  zmq_close(sock);
  if (n <= 0) return -1;
  return (strncmp(reply, "OK", 2) == 0) ? 0 : -1;
}

int zcm_node_info(zcm_node_t *node, const char *name,
                  char *out_endpoint, size_t out_ep_size,
                  char *out_ctrl_endpoint, size_t out_ctrl_size,
                  char *out_host, size_t out_host_size,
                  int *out_pid) {
  if (!node || !name) return -1;
  void *sock = zmq_socket(zcm_context_zmq(node->ctx), ZMQ_REQ);
  if (!sock) return -1;
  set_req_socket_options(sock, 1000);
  if (zmq_connect(sock, node->broker_endpoint) != 0) {
    zmq_close(sock);
    return -1;
  }
  if (send_frames_req(sock, "INFO", name, NULL) != 0) {
    zmq_close(sock);
    return -1;
  }
  char status[32] = {0};
  int n = zmq_recv(sock, status, sizeof(status) - 1, 0);
  if (n <= 0 || strncmp(status, "OK", 2) != 0) {
    zmq_close(sock);
    return -1;
  }

  char ep[512] = {0};
  char ctrl[512] = {0};
  char host[256] = {0};
  char pid_buf[32] = {0};

  n = zmq_recv(sock, ep, sizeof(ep) - 1, 0);
  if (n < 0) { zmq_close(sock); return -1; }
  ep[n] = '\0';
  n = zmq_recv(sock, ctrl, sizeof(ctrl) - 1, 0);
  if (n < 0) { zmq_close(sock); return -1; }
  ctrl[n] = '\0';
  n = zmq_recv(sock, host, sizeof(host) - 1, 0);
  if (n < 0) { zmq_close(sock); return -1; }
  host[n] = '\0';
  n = zmq_recv(sock, pid_buf, sizeof(pid_buf) - 1, 0);
  if (n < 0) { zmq_close(sock); return -1; }
  pid_buf[n] = '\0';
  zmq_close(sock);

  if (out_endpoint && out_ep_size) {
    strncpy(out_endpoint, ep, out_ep_size - 1);
    out_endpoint[out_ep_size - 1] = '\0';
  }
  if (out_ctrl_endpoint && out_ctrl_size) {
    strncpy(out_ctrl_endpoint, ctrl, out_ctrl_size - 1);
    out_ctrl_endpoint[out_ctrl_size - 1] = '\0';
  }
  if (out_host && out_host_size) {
    strncpy(out_host, host, out_host_size - 1);
    out_host[out_host_size - 1] = '\0';
  }
  if (out_pid) *out_pid = atoi(pid_buf);
  return 0;
}

int zcm_node_lookup(zcm_node_t *node, const char *name, char *out_endpoint, size_t out_size) {
  if (!node || !name || !out_endpoint || out_size == 0) return -1;
  void *sock = zmq_socket(zcm_context_zmq(node->ctx), ZMQ_REQ);
  if (!sock) return -1;
  set_req_socket_options(sock, 1000);
  int rc = zmq_connect(sock, node->broker_endpoint);
  if (rc != 0) {
    zmq_close(sock);
    return -1;
  }
  if (send_frames_req(sock, "LOOKUP", name, NULL) != 0) {
    zmq_close(sock);
    return -1;
  }
  char status[32] = {0};
  int n = zmq_recv(sock, status, sizeof(status) - 1, 0);
  if (n <= 0) {
    zmq_close(sock);
    return -1;
  }
  if (strncmp(status, "OK", 2) != 0) {
    zmq_close(sock);
    return -1;
  }
  n = zmq_recv(sock, out_endpoint, out_size - 1, 0);
  zmq_close(sock);
  if (n <= 0) return -1;
  out_endpoint[n] = '\0';
  return 0;
}

int zcm_node_list(zcm_node_t *node, zcm_node_entry_t **out_entries, size_t *out_count) {
  if (!node || !out_entries || !out_count) return -1;
  *out_entries = NULL;
  *out_count = 0;

  void *sock = zmq_socket(zcm_context_zmq(node->ctx), ZMQ_REQ);
  if (!sock) return -1;
  set_req_socket_options(sock, 1000);
  if (zmq_connect(sock, node->broker_endpoint) != 0) {
    zmq_close(sock);
    return -1;
  }
  if (zmq_send(sock, "LIST", 4, 0) < 0) {
    zmq_close(sock);
    return -1;
  }

  char status[16] = {0};
  int n = recv_with_timeout(sock, status, sizeof(status) - 1, 1000);
  if (n <= 0) {
    zmq_close(sock);
    return -1;
  }
  status[n] = '\0';
  if (strncmp(status, "OK", 2) != 0) {
    zmq_close(sock);
    return -1;
  }

  int count = 0;
  n = recv_with_timeout(sock, &count, sizeof(count), 1000);
  if (n <= 0) {
    zmq_close(sock);
    return -1;
  }
  if (count <= 0) {
    zmq_close(sock);
    return 0;
  }

  zcm_node_entry_t *entries = (zcm_node_entry_t *)calloc((size_t)count, sizeof(zcm_node_entry_t));
  if (!entries) {
    zmq_close(sock);
    return -1;
  }

  for (int i = 0; i < count; i++) {
    char name[256] = {0};
    char endpoint[512] = {0};
    n = recv_with_timeout(sock, name, sizeof(name) - 1, 1000);
    if (n <= 0) { count = i; break; }
    name[n] = '\0';

    n = recv_with_timeout(sock, endpoint, sizeof(endpoint) - 1, 1000);
    if (n <= 0) { count = i; break; }
    endpoint[n] = '\0';

    entries[i].name = strdup(name);
    entries[i].endpoint = strdup(endpoint);
    if (!entries[i].name || !entries[i].endpoint) {
      count = i + 1;
      break;
    }
  }

  zmq_close(sock);

  *out_entries = entries;
  *out_count = (size_t)count;
  return 0;
}

void zcm_node_list_free(zcm_node_entry_t *entries, size_t count) {
  if (!entries) return;
  for (size_t i = 0; i < count; i++) {
    free(entries[i].name);
    free(entries[i].endpoint);
  }
  free(entries);
}

zcm_socket_t *zcm_socket_new(zcm_context_t *ctx, zcm_socket_type_t type) {
  if (!ctx) return NULL;
  int zt = map_socket_type(type);
  if (zt < 0) return NULL;
  zcm_socket_t *s = (zcm_socket_t *)calloc(1, sizeof(zcm_socket_t));
  if (!s) return NULL;
  s->sock = zmq_socket(zcm_context_zmq(ctx), zt);
  if (!s->sock) {
    free(s);
    return NULL;
  }
  return s;
}

void zcm_socket_free(zcm_socket_t *sock) {
  if (!sock) return;
  if (sock->sock) zmq_close(sock->sock);
  free(sock);
}

int zcm_socket_bind(zcm_socket_t *sock, const char *endpoint) {
  if (!sock || !sock->sock || !endpoint) return -1;
  return zmq_bind(sock->sock, endpoint);
}

int zcm_socket_connect(zcm_socket_t *sock, const char *endpoint) {
  if (!sock || !sock->sock || !endpoint) return -1;
  return zmq_connect(sock->sock, endpoint);
}

int zcm_socket_set_subscribe(zcm_socket_t *sock, const char *prefix, size_t len) {
  if (!sock || !sock->sock) return -1;
  return zmq_setsockopt(sock->sock, ZMQ_SUBSCRIBE, prefix, len);
}

int zcm_socket_send_msg(zcm_socket_t *sock, const zcm_msg_t *msg) {
  if (!sock || !sock->sock || !msg) return -1;
  const void *data = NULL;
  size_t len = 0;
  void *owned = NULL;
  if (zcm_msg__serialize(msg, &data, &len, &owned) != 0) return -1;
  int rc = zmq_send(sock->sock, data, len, 0);
  free(owned);
  return (rc >= 0) ? 0 : -1;
}

int zcm_socket_recv_msg(zcm_socket_t *sock, zcm_msg_t *msg) {
  if (!sock || !sock->sock || !msg) return -1;
  zmq_msg_t zmsg;
  zmq_msg_init(&zmsg);
  int rc = zmq_msg_recv(&zmsg, sock->sock, 0);
  if (rc < 0) {
    zmq_msg_close(&zmsg);
    return -1;
  }
  rc = zcm_msg_from_bytes(msg, zmq_msg_data(&zmsg), zmq_msg_size(&zmsg));
  zmq_msg_close(&zmsg);
  return rc;
}

int zcm_socket_send_bytes(zcm_socket_t *sock, const void *data, size_t len) {
  if (!sock || !sock->sock || (!data && len)) return -1;
  int rc = zmq_send(sock->sock, data, len, 0);
  return (rc >= 0) ? 0 : -1;
}

int zcm_socket_recv_bytes(zcm_socket_t *sock, void *buf, size_t buf_len, size_t *out_len) {
  if (!sock || !sock->sock || !buf || buf_len == 0) return -1;
  int n = zmq_recv(sock->sock, buf, buf_len, 0);
  if (n < 0) return -1;
  if (out_len) *out_len = (size_t)n;
  return 0;
}

int zcm_socket_set_timeouts(zcm_socket_t *sock, int ms) {
  if (!sock || !sock->sock) return -1;
  if (zmq_setsockopt(sock->sock, ZMQ_RCVTIMEO, &ms, sizeof(ms)) != 0) return -1;
  if (zmq_setsockopt(sock->sock, ZMQ_SNDTIMEO, &ms, sizeof(ms)) != 0) return -1;
  return 0;
}
