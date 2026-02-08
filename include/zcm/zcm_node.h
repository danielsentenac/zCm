#ifndef ZCM_ZCM_NODE_H
#define ZCM_ZCM_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "zcm_msg.h"

struct zcm_context;

typedef struct zcm_node zcm_node_t;

typedef enum {
  ZCM_SOCK_REQ = 1,
  ZCM_SOCK_REP = 2,
  ZCM_SOCK_PUB = 3,
  ZCM_SOCK_SUB = 4,
  ZCM_SOCK_PAIR = 5,
  ZCM_SOCK_PUSH = 6,
  ZCM_SOCK_PULL = 7
} zcm_socket_type_t;

typedef struct zcm_socket zcm_socket_t;

typedef struct zcm_node_entry {
  char *name;
  char *endpoint;
} zcm_node_entry_t;

zcm_node_t *zcm_node_new(struct zcm_context *ctx, const char *broker_endpoint);
void zcm_node_free(zcm_node_t *node);

int zcm_node_register(zcm_node_t *node, const char *name, const char *endpoint);
int zcm_node_unregister(zcm_node_t *node, const char *name);
int zcm_node_register_ex(zcm_node_t *node, const char *name, const char *endpoint,
                         const char *ctrl_endpoint, const char *host, int pid);
int zcm_node_lookup(zcm_node_t *node, const char *name,
                    char *out_endpoint, size_t out_size);
int zcm_node_info(zcm_node_t *node, const char *name,
                  char *out_endpoint, size_t out_ep_size,
                  char *out_ctrl_endpoint, size_t out_ctrl_size,
                  char *out_host, size_t out_host_size,
                  int *out_pid);
int zcm_node_list(zcm_node_t *node, zcm_node_entry_t **out_entries, size_t *out_count);
void zcm_node_list_free(zcm_node_entry_t *entries, size_t count);

zcm_socket_t *zcm_socket_new(struct zcm_context *ctx, zcm_socket_type_t type);
void zcm_socket_free(zcm_socket_t *sock);
int zcm_socket_bind(zcm_socket_t *sock, const char *endpoint);
int zcm_socket_connect(zcm_socket_t *sock, const char *endpoint);
int zcm_socket_set_subscribe(zcm_socket_t *sock, const char *prefix, size_t len);

int zcm_socket_send_msg(zcm_socket_t *sock, const zcm_msg_t *msg);
int zcm_socket_recv_msg(zcm_socket_t *sock, zcm_msg_t *msg);

int zcm_socket_send_bytes(zcm_socket_t *sock, const void *data, size_t len);
int zcm_socket_recv_bytes(zcm_socket_t *sock, void *buf, size_t buf_len, size_t *out_len);
int zcm_socket_set_timeouts(zcm_socket_t *sock, int ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCM_ZCM_NODE_H */
