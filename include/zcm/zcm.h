#ifndef ZCM_ZCM_H
#define ZCM_ZCM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define ZCM_OK 0
#define ZCM_ERR (-1)

/* Opaque handles */
typedef struct zcm_context zcm_context_t;
typedef struct zcm_broker zcm_broker_t;

/* Context lifecycle */
zcm_context_t *zcm_context_new(void);
void zcm_context_free(zcm_context_t *ctx);

/* Broker (registry) lifecycle */
zcm_broker_t *zcm_broker_start(zcm_context_t *ctx, const char *endpoint);
void zcm_broker_stop(zcm_broker_t *broker);

/* Version */
const char *zcm_version_string(void);

/* internal: transport access */
void *zcm_context_zmq(zcm_context_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZCM_ZCM_H */
