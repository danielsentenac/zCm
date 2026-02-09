#ifndef ZCM_ZCM_PROC_H
#define ZCM_ZCM_PROC_H

/**
 * @file zcm_proc.h
 * @brief High-level process bootstrap helpers.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "zcm.h"
#include "zcm_node.h"

/** @brief Opaque process helper handle. */
typedef struct zcm_proc zcm_proc_t;

/**
 * @brief Initialize a process: context, node registration, control socket, and optional data socket.
 *
 * @param name Process/service name to register.
 * @param data_type Socket type used for the optional data socket.
 * @param bind_data Non-zero to create and bind a data socket, zero to skip.
 * @param out_proc Output process handle on success.
 * @param out_data Optional output for created data socket.
 * @return `0` on success, `-1` on failure.
 */
int zcm_proc_init(const char *name, zcm_socket_type_t data_type, int bind_data,
                  zcm_proc_t **out_proc, zcm_socket_t **out_data);

/**
 * @brief Tear down and unregister a process created by zcm_proc_init().
 *
 * @param proc Process handle to free. `NULL` is allowed.
 */
void zcm_proc_free(zcm_proc_t *proc);

/**
 * @brief Access the context owned by a process helper.
 *
 * @param proc Process handle.
 * @return Context pointer, or `NULL` when `proc` is `NULL`.
 */
zcm_context_t *zcm_proc_context(zcm_proc_t *proc);

/**
 * @brief Access the node helper owned by a process helper.
 *
 * @param proc Process handle.
 * @return Node pointer, or `NULL` when `proc` is `NULL`.
 */
zcm_node_t *zcm_proc_node(zcm_proc_t *proc);

#ifdef __cplusplus
}
#endif

#endif /* ZCM_ZCM_PROC_H */
