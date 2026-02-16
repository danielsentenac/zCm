#include "zcm/zcm.h"
#include "zcm/zcm_node.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_broker_t *broker = NULL;
  zcm_node_t *node = NULL;

  if (!ctx) {
    fprintf(stderr, "zcm_node_ctrl_fallback: context init failed\n");
    return 1;
  }

  broker = zcm_broker_start(ctx, "inproc://zcm-broker-ctrl-fallback");
  if (!broker) {
    fprintf(stderr, "zcm_node_ctrl_fallback: broker start failed\n");
    goto cleanup;
  }

  node = zcm_node_new(ctx, "inproc://zcm-broker-ctrl-fallback");
  if (!node) {
    fprintf(stderr, "zcm_node_ctrl_fallback: node init failed\n");
    goto cleanup;
  }

  /* Legacy REGISTER has no explicit control endpoint. */
  if (zcm_node_register(node, "legacy", "tcp://127.0.0.1:7300") != 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: legacy register failed\n");
    goto cleanup;
  }

  char endpoint[256] = {0};
  char ctrl[256] = {0};
  if (zcm_node_info(node, "legacy",
                    endpoint, sizeof(endpoint),
                    ctrl, sizeof(ctrl),
                    NULL, 0, NULL) != 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: legacy info failed\n");
    goto cleanup;
  }
  if (strcmp(endpoint, "tcp://127.0.0.1:7300") != 0) {
    fprintf(stderr, "unexpected endpoint for legacy: %s\n", endpoint);
    goto cleanup;
  }
  if (strcmp(ctrl, "tcp://127.0.0.1:7301") != 0) {
    fprintf(stderr, "expected inferred ctrl endpoint tcp://127.0.0.1:7301 got: %s\n", ctrl);
    goto cleanup;
  }

  /* Explicit REGISTER_EX control endpoint must remain unchanged. */
  if (zcm_node_register_ex(node, "explicit",
                           "tcp://127.0.0.1:7400",
                           "tcp://127.0.0.1:9400",
                           "127.0.0.1", (int)getpid()) != 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: explicit register_ex failed\n");
    goto cleanup;
  }
  memset(ctrl, 0, sizeof(ctrl));
  if (zcm_node_info(node, "explicit",
                    NULL, 0,
                    ctrl, sizeof(ctrl),
                    NULL, 0, NULL) != 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: explicit info failed\n");
    goto cleanup;
  }
  if (strcmp(ctrl, "tcp://127.0.0.1:9400") != 0) {
    fprintf(stderr, "explicit ctrl endpoint changed unexpectedly: %s\n", ctrl);
    goto cleanup;
  }

  /* Port overflow should not produce an inferred control endpoint. */
  if (zcm_node_register(node, "overflow", "tcp://127.0.0.1:65535") != 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: overflow register failed\n");
    goto cleanup;
  }
  memset(ctrl, 0, sizeof(ctrl));
  if (zcm_node_info(node, "overflow",
                    NULL, 0,
                    ctrl, sizeof(ctrl),
                    NULL, 0, NULL) != 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: overflow info failed\n");
    goto cleanup;
  }
  if (ctrl[0] != '\0') {
    fprintf(stderr, "expected empty ctrl endpoint for overflow case, got: %s\n", ctrl);
    goto cleanup;
  }

  rc = 0;
  printf("zcm_node_ctrl_fallback: PASS\n");

cleanup:
  if (node) zcm_node_free(node);
  if (broker) zcm_broker_stop(broker);
  if (ctx) zcm_context_free(ctx);
  return rc;
}
