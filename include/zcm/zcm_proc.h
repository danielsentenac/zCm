#ifndef ZCM_ZCM_PROC_H
#define ZCM_ZCM_PROC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zcm.h"
#include "zcm_node.h"

typedef struct zcm_proc zcm_proc_t;

int zcm_proc_init(const char *name, zcm_socket_type_t data_type, int bind_data,
                  zcm_proc_t **out_proc, zcm_socket_t **out_data);

void zcm_proc_free(zcm_proc_t *proc);

zcm_context_t *zcm_proc_context(zcm_proc_t *proc);
zcm_node_t *zcm_proc_node(zcm_proc_t *proc);

#ifdef __cplusplus
}
#endif

#endif /* ZCM_ZCM_PROC_H */
