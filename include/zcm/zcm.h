#ifndef ZCM_ZCM_H
#define ZCM_ZCM_H

/**
 * @file zcm.h
 * @brief Core zCm context and broker lifecycle API.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/** @brief Success return code. */
#define ZCM_OK 0
/** @brief Generic failure return code. */
#define ZCM_ERR (-1)

/** @brief Opaque process-wide context handle. */
typedef struct zcm_context zcm_context_t;
/** @brief Opaque broker handle. */
typedef struct zcm_broker zcm_broker_t;

/**
 * @brief Create a new zCm context.
 *
 * @return Newly allocated context on success, or `NULL` on allocation/backend failure.
 */
zcm_context_t *zcm_context_new(void);

/**
 * @brief Destroy a context created by zcm_context_new().
 *
 * @param ctx Context to free. `NULL` is allowed.
 */
void zcm_context_free(zcm_context_t *ctx);

/**
 * @brief Start a broker service bound to the provided endpoint.
 *
 * @param ctx Valid zCm context.
 * @param endpoint Broker bind endpoint, for example `tcp://0.0.0.0:5555`.
 * @return Broker handle on success, or `NULL` on failure.
 */
zcm_broker_t *zcm_broker_start(zcm_context_t *ctx, const char *endpoint);

/**
 * @brief Stop and free a broker created by zcm_broker_start().
 *
 * @param broker Broker handle to stop. `NULL` is allowed.
 */
void zcm_broker_stop(zcm_broker_t *broker);

/**
 * @brief Return the library version string.
 *
 * @return Null-terminated version string.
 */
const char *zcm_version_string(void);

/**
 * @brief Get the underlying ZeroMQ context pointer.
 *
 * This is intended for internal integrations.
 *
 * @param ctx zCm context.
 * @return Native ZeroMQ context pointer, or `NULL` when `ctx` is `NULL`.
 */
void *zcm_context_zmq(zcm_context_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZCM_ZCM_H */
