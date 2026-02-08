#include "zcm/zcm.h"

#include <stdlib.h>

#include <zmq.h>

struct zcm_context {
  void *zmq;
};

zcm_context_t *zcm_context_new(void) {
  zcm_context_t *ctx = (zcm_context_t *)calloc(1, sizeof(zcm_context_t));
  if (!ctx) return NULL;
  ctx->zmq = zmq_ctx_new();
  if (!ctx->zmq) {
    free(ctx);
    return NULL;
  }
  return ctx;
}

void zcm_context_free(zcm_context_t *ctx) {
  if (!ctx) return;
  if (ctx->zmq) zmq_ctx_term(ctx->zmq);
  free(ctx);
}

const char *zcm_version_string(void) {
  return "zCm 0.1.0";
}

void *zcm_context_zmq(zcm_context_t *ctx) {
  if (!ctx) return NULL;
  return ctx->zmq;
}
