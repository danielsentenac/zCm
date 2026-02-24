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

  if (zcm_node_register_ex(node, "missing-ctrl",
                           "tcp://127.0.0.1:7300",
                           "",
                           "127.0.0.1", (int)getpid(),
                           "PUB", 7300, -1) == 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: missing ctrl endpoint must fail\n");
    goto cleanup;
  }
  if (zcm_node_register_ex(node, "missing-host",
                           "tcp://127.0.0.1:7300",
                           "tcp://127.0.0.1:9300",
                           "", (int)getpid(),
                           "PUB", 7300, -1) == 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: missing host must fail\n");
    goto cleanup;
  }
  if (zcm_node_register_ex(node, "missing-pid",
                           "tcp://127.0.0.1:7300",
                           "tcp://127.0.0.1:9300",
                           "127.0.0.1", 0,
                           "PUB", 7300, -1) == 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: non-positive pid must fail\n");
    goto cleanup;
  }

  if (zcm_node_register_ex(node, "explicit",
                           "tcp://127.0.0.1:7400",
                           "tcp://127.0.0.1:9400",
                           "127.0.0.1", (int)getpid(),
                           "PUB", 7400, -1) != 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: explicit register_ex failed\n");
    goto cleanup;
  }
  char endpoint[256] = {0};
  char ctrl[256] = {0};
  char host[256] = {0};
  int pid = 0;
  memset(endpoint, 0, sizeof(endpoint));
  memset(ctrl, 0, sizeof(ctrl));
  memset(host, 0, sizeof(host));
  if (zcm_node_info(node, "explicit",
                    endpoint, sizeof(endpoint),
                    ctrl, sizeof(ctrl),
                    host, sizeof(host), &pid) != 0) {
    fprintf(stderr, "zcm_node_ctrl_fallback: explicit info failed\n");
    goto cleanup;
  }
  if (strcmp(endpoint, "tcp://127.0.0.1:7400") != 0) {
    fprintf(stderr, "explicit endpoint mismatch: %s\n", endpoint);
    goto cleanup;
  }
  if (strcmp(ctrl, "tcp://127.0.0.1:9400") != 0) {
    fprintf(stderr, "explicit ctrl endpoint changed unexpectedly: %s\n", ctrl);
    goto cleanup;
  }
  if (strcmp(host, "127.0.0.1") != 0) {
    fprintf(stderr, "explicit host mismatch: %s\n", host);
    goto cleanup;
  }
  if (pid != (int)getpid()) {
    fprintf(stderr, "explicit pid mismatch: %d\n", pid);
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
