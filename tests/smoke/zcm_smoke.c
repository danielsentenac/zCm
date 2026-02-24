#include "zcm/zcm.h"
#include "zcm/zcm_node.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;

  printf("zcm_smoke: starting broker\n");
  zcm_broker_t *broker = zcm_broker_start(ctx, "inproc://zcm-broker");
  if (!broker) {
    zcm_context_free(ctx);
    return 1;
  }

  printf("zcm_smoke: creating node and registering name\n");
  zcm_node_t *node = zcm_node_new(ctx, "inproc://zcm-broker");
  if (!node) {
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  if (zcm_node_register_ex(node, "proc",
                           "tcp://127.0.0.1:5555",
                           "tcp://127.0.0.1:5556",
                           "127.0.0.1",
                           (int)getpid(),
                           "PUB", 5555, -1) != 0) {
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  printf("zcm_smoke: lookup name and verify endpoint\n");
  char ep[256] = {0};
  if (zcm_node_lookup(node, "proc", ep, sizeof(ep)) != 0) {
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  if (strcmp(ep, "tcp://127.0.0.1:5555") != 0) {
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  printf("%s\n", zcm_version_string());
  printf("zcm_smoke: PASS\n");

  zcm_node_free(node);
  zcm_broker_stop(broker);
  zcm_context_free(ctx);
  return 0;
}
