#ifndef ZCM_ZCM_PROC_RUNTIME_H
#define ZCM_ZCM_PROC_RUNTIME_H

/**
 * @file zcm_proc_runtime.h
 * @brief Runtime config parsing and bootstrap helpers for zcm_proc daemons.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "zcm_proc.h"

#define ZCM_PROC_TYPE_HANDLER_MAX 32
#define ZCM_PROC_TYPE_HANDLER_ARG_MAX 32
#define ZCM_PROC_DATA_SOCKET_MAX 16

typedef enum zcm_proc_data_socket_kind {
  ZCM_PROC_DATA_SOCKET_PUB = 1,
  ZCM_PROC_DATA_SOCKET_SUB = 2
} zcm_proc_data_socket_kind_t;

typedef struct zcm_proc_data_socket_cfg {
  zcm_proc_data_socket_kind_t kind;
  int port;
  char target[128];
  char payload[256];
  int interval_ms;
} zcm_proc_data_socket_cfg_t;

typedef enum zcm_proc_type_arg_kind {
  ZCM_PROC_TYPE_ARG_TEXT = 1,
  ZCM_PROC_TYPE_ARG_DOUBLE = 2,
  ZCM_PROC_TYPE_ARG_FLOAT = 3,
  ZCM_PROC_TYPE_ARG_INT = 4
} zcm_proc_type_arg_kind_t;

typedef struct zcm_proc_type_handler_cfg {
  char name[64];
  char reply[128];
  zcm_proc_type_arg_kind_t args[ZCM_PROC_TYPE_HANDLER_ARG_MAX];
  size_t arg_count;
  char format[256];
} zcm_proc_type_handler_cfg_t;

typedef struct zcm_proc_runtime_cfg {
  char name[128];
  char core_ping_request[64];
  char core_ping_reply[64];
  char core_default_reply[64];
  zcm_proc_type_handler_cfg_t type_handlers[ZCM_PROC_TYPE_HANDLER_MAX];
  size_t type_handler_count;
  zcm_proc_data_socket_cfg_t data_sockets[ZCM_PROC_DATA_SOCKET_MAX];
  size_t data_socket_count;
} zcm_proc_runtime_cfg_t;

/**
 * @brief Parse and validate a runtime config XML into an in-memory structure.
 *
 * @param cfg_path Path to proc config XML.
 * @param cfg Destination runtime config object.
 * @return `0` on success, `-1` on failure.
 */
int zcm_proc_runtime_load_config(const char *cfg_path, zcm_proc_runtime_cfg_t *cfg);

/**
 * @brief Load config and initialize a daemon process/socket pair.
 *
 * This helper sets `ZCM_PROC_CONFIG_FILE` so zcm_proc internals can read the
 * same XML file.
 *
 * @param cfg_path Path to proc config XML.
 * @param cfg Output parsed config.
 * @param out_proc Output process handle.
 * @param out_rep Output REP control socket.
 * @return `0` on success, `-1` on failure.
 */
int zcm_proc_runtime_bootstrap(const char *cfg_path,
                               zcm_proc_runtime_cfg_t *cfg,
                               zcm_proc_t **out_proc,
                               zcm_socket_t **out_rep);

/**
 * @brief Find a configured TYPE handler by case-insensitive name.
 */
const zcm_proc_type_handler_cfg_t *zcm_proc_runtime_find_type_handler(
    const zcm_proc_runtime_cfg_t *cfg,
    const char *type_name);

/**
 * @brief Decode a message payload according to one TYPE handler signature.
 *
 * On success, a human-readable summary is written to `summary`.
 */
int zcm_proc_runtime_decode_type_payload(zcm_msg_t *msg,
                                         const zcm_proc_type_handler_cfg_t *handler,
                                         char *summary,
                                         size_t summary_size);

/**
 * @brief Return the first configured PUB data port.
 */
int zcm_proc_runtime_first_pub_port(const zcm_proc_runtime_cfg_t *cfg, int *out_port);

/**
 * @brief Start detached background workers for configured PUB/SUB data sockets.
 */
void zcm_proc_runtime_start_data_workers(const zcm_proc_runtime_cfg_t *cfg, zcm_proc_t *proc);

#ifdef __cplusplus
}
#endif

#endif /* ZCM_ZCM_PROC_RUNTIME_H */
