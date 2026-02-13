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

/** @brief Maximum number of `<type>` handlers supported in one proc config. */
#define ZCM_PROC_TYPE_HANDLER_MAX 32
/** @brief Maximum number of typed arguments parsed for one `<type>` handler. */
#define ZCM_PROC_TYPE_HANDLER_ARG_MAX 32
/** @brief Maximum number of `<dataSocket>` entries supported per proc config. */
#define ZCM_PROC_DATA_SOCKET_MAX 16
/** @brief Maximum number of SUB topics supported for one SUB data socket. */
#define ZCM_PROC_SUB_TOPIC_MAX 16

/**
 * @brief Data socket kind declared in proc runtime config.
 */
typedef enum zcm_proc_data_socket_kind {
  /** Publisher socket (broadcast payload periodically). */
  ZCM_PROC_DATA_SOCKET_PUB = 1,
  /** Subscriber socket (consume payload from one publisher target). */
  ZCM_PROC_DATA_SOCKET_SUB = 2,
  /** Push socket (send payload to pull peers). */
  ZCM_PROC_DATA_SOCKET_PUSH = 3,
  /** Pull socket (receive payload from one push target). */
  ZCM_PROC_DATA_SOCKET_PULL = 4
} zcm_proc_data_socket_kind_t;

/**
 * @brief One runtime data socket entry parsed from XML.
 */
typedef struct zcm_proc_data_socket_cfg {
  /** Socket role (PUB/SUB/PUSH/PULL). */
  zcm_proc_data_socket_kind_t kind;
  /** Bound TCP port for PUB/PUSH; auto-assigned if initially unset. */
  int port;
  /** Target node name for SUB/PULL endpoints. */
  char target[128];
  /** Topic prefixes applied on SUB sockets. */
  char topics[ZCM_PROC_SUB_TOPIC_MAX][128];
  /** Number of valid entries in `topics`. */
  size_t topic_count;
  /** Payload text used by PUB/PUSH workers. */
  char payload[256];
  /** Publish/push loop period in milliseconds. */
  int interval_ms;
} zcm_proc_data_socket_cfg_t;

/**
 * @brief Supported typed argument kinds parsed from `<type format="...">`.
 */
typedef enum zcm_proc_type_arg_kind {
  /** String/text argument (`-t`). */
  ZCM_PROC_TYPE_ARG_TEXT = 1,
  /** Double argument (`-d`). */
  ZCM_PROC_TYPE_ARG_DOUBLE = 2,
  /** Float argument (`-f`). */
  ZCM_PROC_TYPE_ARG_FLOAT = 3,
  /** Integer argument (`-i`). */
  ZCM_PROC_TYPE_ARG_INT = 4
} zcm_proc_type_arg_kind_t;

/**
 * @brief One configured typed request handler signature.
 */
typedef struct zcm_proc_type_handler_cfg {
  /** Type name (e.g. `QUERY`, `VALUE`). */
  char name[64];
  /** Parsed ordered list of expected argument kinds. */
  zcm_proc_type_arg_kind_t args[ZCM_PROC_TYPE_HANDLER_ARG_MAX];
  /** Number of valid entries in `args`. */
  size_t arg_count;
  /** Raw format string from config (if provided). */
  char format[256];
} zcm_proc_type_handler_cfg_t;

/**
 * @brief Parsed runtime config for one `zcm_proc` instance.
 */
typedef struct zcm_proc_runtime_cfg {
  /** Node/process name used for broker registration. */
  char name[128];
  /** Declared typed handlers. */
  zcm_proc_type_handler_cfg_t type_handlers[ZCM_PROC_TYPE_HANDLER_MAX];
  /** Number of valid entries in `type_handlers`. */
  size_t type_handler_count;
  /** Declared data sockets for worker startup. */
  zcm_proc_data_socket_cfg_t data_sockets[ZCM_PROC_DATA_SOCKET_MAX];
  /** Number of valid entries in `data_sockets`. */
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
 *
 * @param cfg Runtime config to inspect.
 * @param type_name Type name to match.
 * @return Matching handler pointer, or `NULL` if not found.
 */
const zcm_proc_type_handler_cfg_t *zcm_proc_runtime_find_type_handler(
    const zcm_proc_runtime_cfg_t *cfg,
    const char *type_name);

/**
 * @brief Decode a message payload according to one TYPE handler signature.
 *
 * On success, a human-readable summary is written to `summary`.
 *
 * @param msg Input message with encoded payload values.
 * @param handler Expected type signature used for decoding.
 * @param summary Output text buffer for decoded values.
 * @param summary_size Size of `summary` in bytes.
 * @return `0` on success, `-1` on decode/validation error.
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
 *
 * @param cfg Runtime config.
 * @return Static role string matching configured sockets.
 */
const char *zcm_proc_runtime_data_role(const zcm_proc_runtime_cfg_t *cfg);

/**
 * @brief Builtin command request literal used by zcm_proc command semantics.
 *
 * @return Static ping request text (e.g. `PING`).
 */
const char *zcm_proc_runtime_builtin_ping_request(void);

/**
 * @brief Builtin command reply literal for builtin ping requests.
 *
 * @return Static ping reply text (e.g. `PONG`).
 */
const char *zcm_proc_runtime_builtin_ping_reply(void);

/**
 * @brief Builtin default reply literal for unknown text commands.
 *
 * @return Static default reply text (e.g. `OK`).
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
 *
 * @param cfg Runtime config.
 * @param out_port Output resolved PUB port.
 * @return `0` on success, `-1` if no PUB socket is configured.
 */
int zcm_proc_runtime_first_pub_port(const zcm_proc_runtime_cfg_t *cfg, int *out_port);

/**
 * @brief Return the first configured PUSH data port.
 *
 * @param cfg Runtime config.
 * @param out_port Output resolved PUSH port.
 * @return `0` on success, `-1` if no PUSH socket is configured.
 */
int zcm_proc_runtime_first_push_port(const zcm_proc_runtime_cfg_t *cfg, int *out_port);

/**
 * @brief Return payload byte count for one data socket kind.
 *
 * For `PUB`/`PUSH`, this returns the configured payload byte count from the
 * first matching data socket. For `SUB`/`PULL`, this returns the last received
 * payload byte count observed by runtime workers (starts at `0` when the kind
 * is configured and no payload has been received yet).
 *
 * @param cfg Runtime config.
 * @param kind Data socket kind to query.
 * @param out_bytes Output byte count.
 * @return `0` on success, `-1` when no matching data socket is configured.
 */
int zcm_proc_runtime_payload_bytes(const zcm_proc_runtime_cfg_t *cfg,
                                   zcm_proc_data_socket_kind_t kind,
                                   int *out_bytes);

/**
 * @brief Start detached background workers for configured data sockets.
 *
 * PUB/PUSH data sockets allocate TCP ports automatically from the current
 * domain range and write chosen ports back into `cfg->data_sockets`.
 *
 * @param cfg Runtime config to read and update.
 * @param proc Running process handle used by workers.
 * @param on_sub_payload Optional callback invoked for SUB/PULL received payloads.
 * @param user Opaque pointer forwarded to `on_sub_payload`.
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
