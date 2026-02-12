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
#include <stdint.h>

#include "zcm_proc.h"

/** @addtogroup zcm_high_level
 * @{
 */

#define ZCM_PROC_TYPE_HANDLER_MAX 32
#define ZCM_PROC_TYPE_HANDLER_ARG_MAX 32
#define ZCM_PROC_DATA_SOCKET_MAX 16
#define ZCM_PROC_SUB_TOPIC_MAX 16

typedef enum zcm_proc_data_socket_kind {
  ZCM_PROC_DATA_SOCKET_PUB = 1,
  ZCM_PROC_DATA_SOCKET_SUB = 2,
  ZCM_PROC_DATA_SOCKET_PUSH = 3,
  ZCM_PROC_DATA_SOCKET_PULL = 4
} zcm_proc_data_socket_kind_t;

typedef struct zcm_proc_data_socket_cfg {
  zcm_proc_data_socket_kind_t kind;
  int port;
  char target[128];
  char topics[ZCM_PROC_SUB_TOPIC_MAX][128];
  size_t topic_count;
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
  zcm_proc_type_arg_kind_t args[ZCM_PROC_TYPE_HANDLER_ARG_MAX];
  size_t arg_count;
  char format[256];
} zcm_proc_type_handler_cfg_t;

typedef struct zcm_proc_runtime_cfg {
  char name[128];
  zcm_proc_type_handler_cfg_t type_handlers[ZCM_PROC_TYPE_HANDLER_MAX];
  size_t type_handler_count;
  zcm_proc_data_socket_cfg_t data_sockets[ZCM_PROC_DATA_SOCKET_MAX];
  size_t data_socket_count;
} zcm_proc_runtime_cfg_t;

/**
 * @brief Optional callback for bytes received by SUB/PULL data workers.
 *
 * @param self_name Name of the current process.
 * @param source_name Source process name from config target.
 * @param payload Received payload bytes.
 * @param payload_len Number of bytes in `payload`.
 * @param user Opaque user pointer passed to worker startup.
 */
typedef void (*zcm_proc_runtime_sub_payload_cb_t)(
    const char *self_name,
    const char *source_name,
    const void *payload,
    size_t payload_len,
    void *user);

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
 * @brief Return the data role string for configured data sockets.
 *
 * Possible values include `NONE`, `PUB`, `SUB`, `PUSH`, `PULL`,
 * and `+` combinations (for example: `PUB+SUB`, `PUSH+PULL`).
 */
const char *zcm_proc_runtime_data_role(const zcm_proc_runtime_cfg_t *cfg);

/**
 * @brief Builtin command request literal used by zcm_proc command semantics.
 */
const char *zcm_proc_runtime_builtin_ping_request(void);

/**
 * @brief Builtin command reply literal for builtin ping requests.
 */
const char *zcm_proc_runtime_builtin_ping_reply(void);

/**
 * @brief Builtin default reply literal for unknown text commands.
 */
const char *zcm_proc_runtime_builtin_default_reply(void);

/**
 * @brief Resolve builtin text command reply (`PING` => `PONG`, else `OK`).
 *
 * @param cmd Command text bytes.
 * @param cmd_len Command length in bytes.
 * @return Static reply literal.
 */
const char *zcm_proc_runtime_builtin_reply_for_command(const char *cmd, uint32_t cmd_len);

/**
 * @brief Return the first configured PUB data port.
 */
int zcm_proc_runtime_first_pub_port(const zcm_proc_runtime_cfg_t *cfg, int *out_port);

/**
 * @brief Return the first configured PUSH data port.
 */
int zcm_proc_runtime_first_push_port(const zcm_proc_runtime_cfg_t *cfg, int *out_port);

/**
 * @brief Start detached background workers for configured data sockets.
 *
 * PUB/PUSH data sockets allocate TCP ports automatically from the current
 * domain range and write chosen ports back into `cfg->data_sockets`.
 */
void zcm_proc_runtime_start_data_workers(zcm_proc_runtime_cfg_t *cfg,
                                         zcm_proc_t *proc,
                                         zcm_proc_runtime_sub_payload_cb_t on_sub_payload,
                                         void *user);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZCM_ZCM_PROC_RUNTIME_H */
