#ifndef ZCM_ZCM_NODE_H
#define ZCM_ZCM_NODE_H

/**
 * @file zcm_node.h
 * @brief Node registry and transport socket APIs.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "zcm_msg.h"

struct zcm_context;

/** @brief Opaque broker-registry client handle. */
typedef struct zcm_node zcm_node_t;

/**
 * @brief Socket type abstraction mapped to ZeroMQ socket kinds.
 */
typedef enum {
  ZCM_SOCK_REQ = 1,
  ZCM_SOCK_REP = 2,
  ZCM_SOCK_PUB = 3,
  ZCM_SOCK_SUB = 4,
  ZCM_SOCK_PAIR = 5,
  ZCM_SOCK_PUSH = 6,
  ZCM_SOCK_PULL = 7
} zcm_socket_type_t;

/** @brief Opaque transport socket wrapper. */
typedef struct zcm_socket zcm_socket_t;

/**
 * @brief Node listing entry returned by zcm_node_list().
 */
typedef struct zcm_node_entry {
  /** @brief Registered logical name. */
  char *name;
  /** @brief Endpoint string associated with @ref name. */
  char *endpoint;
} zcm_node_entry_t;

/** @brief zcm_node_register_ex() failure code when a name is already owned by another process. */
#define ZCM_NODE_REGISTER_EX_DUPLICATE (-2)

/**
 * @brief Create a node helper bound to one broker endpoint.
 *
 * @param ctx zCm context.
 * @param broker_endpoint Broker endpoint string used for registry requests.
 * @return Node handle on success, or `NULL` on failure.
 */
zcm_node_t *zcm_node_new(struct zcm_context *ctx, const char *broker_endpoint);

/**
 * @brief Destroy a node created by zcm_node_new().
 *
 * @param node Node handle to free. `NULL` is allowed.
 */
void zcm_node_free(zcm_node_t *node);

/**
 * @brief Register a name to a data endpoint in the broker.
 *
 * @param node Node helper.
 * @param name Logical process/service name.
 * @param endpoint Data endpoint associated with `name`.
 * @return `0` on success, `-1` on failure.
 */
int zcm_node_register(zcm_node_t *node, const char *name, const char *endpoint);

/**
 * @brief Remove a previously registered name from the broker.
 *
 * @param node Node helper.
 * @param name Name to remove.
 * @return `0` on success, `-1` on failure.
 */
int zcm_node_unregister(zcm_node_t *node, const char *name);

/**
 * @brief Register a process with extended metadata.
 *
 * @param node Node helper.
 * @param name Logical process/service name.
 * @param endpoint Data endpoint.
 * @param ctrl_endpoint Control endpoint used for management actions.
 * @param host Hostname or IP advertised by the process.
 * @param pid Process ID.
 * @return `0` on success, `ZCM_NODE_REGISTER_EX_DUPLICATE` on duplicate name,
 *         `-1` on transport/internal failure.
 */
int zcm_node_register_ex(zcm_node_t *node, const char *name, const char *endpoint,
                         const char *ctrl_endpoint, const char *host, int pid);

/**
 * @brief Resolve a registered name to its endpoint.
 *
 * @param node Node helper.
 * @param name Name to resolve.
 * @param out_endpoint Output buffer receiving endpoint string.
 * @param out_size Size of `out_endpoint` in bytes.
 * @return `0` on success, `-1` on failure.
 */
int zcm_node_lookup(zcm_node_t *node, const char *name,
                    char *out_endpoint, size_t out_size);

/**
 * @brief Fetch extended metadata for a registered name.
 *
 * Any output pointer can be `NULL` to skip that field.
 *
 * @param node Node helper.
 * @param name Name to inspect.
 * @param out_endpoint Output buffer for data endpoint.
 * @param out_ep_size Size of `out_endpoint`.
 * @param out_ctrl_endpoint Output buffer for control endpoint.
 * @param out_ctrl_size Size of `out_ctrl_endpoint`.
 * @param out_host Output buffer for host name/address.
 * @param out_host_size Size of `out_host`.
 * @param out_pid Optional process ID output.
 * @return `0` on success, `-1` on failure.
 */
int zcm_node_info(zcm_node_t *node, const char *name,
                  char *out_endpoint, size_t out_ep_size,
                  char *out_ctrl_endpoint, size_t out_ctrl_size,
                  char *out_host, size_t out_host_size,
                  int *out_pid);

/**
 * @brief List currently registered node entries.
 *
 * @param node Node helper.
 * @param out_entries Output array allocated by the library.
 * @param out_count Output number of entries in `out_entries`.
 * @return `0` on success, `-1` on failure.
 */
int zcm_node_list(zcm_node_t *node, zcm_node_entry_t **out_entries, size_t *out_count);

/**
 * @brief Free list memory returned by zcm_node_list().
 *
 * @param entries Entry array pointer returned by zcm_node_list().
 * @param count Number of entries in `entries`.
 */
void zcm_node_list_free(zcm_node_entry_t *entries, size_t count);

/**
 * @brief Create a transport socket wrapper.
 *
 * @param ctx zCm context.
 * @param type Desired socket type.
 * @return Socket wrapper on success, or `NULL` on failure.
 */
zcm_socket_t *zcm_socket_new(struct zcm_context *ctx, zcm_socket_type_t type);

/**
 * @brief Destroy a socket created by zcm_socket_new().
 *
 * @param sock Socket wrapper to free. `NULL` is allowed.
 */
void zcm_socket_free(zcm_socket_t *sock);

/**
 * @brief Bind a socket to an endpoint.
 *
 * @param sock Socket wrapper.
 * @param endpoint Bind endpoint string.
 * @return `0` on success, `-1` on failure.
 */
int zcm_socket_bind(zcm_socket_t *sock, const char *endpoint);

/**
 * @brief Connect a socket to an endpoint.
 *
 * @param sock Socket wrapper.
 * @param endpoint Remote endpoint string.
 * @return `0` on success, `-1` on failure.
 */
int zcm_socket_connect(zcm_socket_t *sock, const char *endpoint);

/**
 * @brief Configure subscription prefix on a SUB socket.
 *
 * @param sock Socket wrapper.
 * @param prefix Prefix bytes. Can be empty for all topics.
 * @param len Prefix size in bytes.
 * @return `0` on success, `-1` on failure.
 */
int zcm_socket_set_subscribe(zcm_socket_t *sock, const char *prefix, size_t len);

/**
 * @brief Serialize and send a typed message.
 *
 * @param sock Socket wrapper.
 * @param msg Message to send.
 * @return `0` on success, `-1` on failure.
 */
int zcm_socket_send_msg(zcm_socket_t *sock, const zcm_msg_t *msg);

/**
 * @brief Receive and decode a typed message.
 *
 * @param sock Socket wrapper.
 * @param msg Destination message object.
 * @return `0` on success, `-1` on transport/decode error.
 */
int zcm_socket_recv_msg(zcm_socket_t *sock, zcm_msg_t *msg);

/**
 * @brief Send raw bytes.
 *
 * @param sock Socket wrapper.
 * @param data Byte buffer to send. Can be `NULL` when `len == 0`.
 * @param len Number of bytes to send.
 * @return `0` on success, `-1` on failure.
 */
int zcm_socket_send_bytes(zcm_socket_t *sock, const void *data, size_t len);

/**
 * @brief Receive raw bytes into a caller-provided buffer.
 *
 * @param sock Socket wrapper.
 * @param buf Destination buffer.
 * @param buf_len Capacity of `buf` in bytes.
 * @param out_len Optional output with received byte count.
 * @return `0` on success, `-1` on failure.
 */
int zcm_socket_recv_bytes(zcm_socket_t *sock, void *buf, size_t buf_len, size_t *out_len);

/**
 * @brief Configure receive/send socket timeouts.
 *
 * @param sock Socket wrapper.
 * @param ms Timeout in milliseconds.
 * @return `0` on success, `-1` on failure.
 */
int zcm_socket_set_timeouts(zcm_socket_t *sock, int ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCM_ZCM_NODE_H */
