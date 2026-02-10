#include "zcm/zcm.h"
#include "zcm/zcm_node.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  int owner_pid = (int)getpid();
  int other_pid = owner_pid + 1;

  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;

  printf("zcm_node_unique_name: start broker\n");
  zcm_broker_t *broker = zcm_broker_start(ctx, "inproc://zcm-broker-unique-name");
  if (!broker) {
    zcm_context_free(ctx);
    return 1;
  }

  zcm_node_t *node = zcm_node_new(ctx, "inproc://zcm-broker-unique-name");
  if (!node) {
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  printf("zcm_node_unique_name: first register_ex succeeds\n");
  if (zcm_node_register_ex(node, "basic",
                           "tcp://127.0.0.1:7301",
                           "tcp://127.0.0.1:7401",
                           "127.0.0.1", owner_pid) != 0) {
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  printf("zcm_node_unique_name: same owner re-register_ex succeeds\n");
  if (zcm_node_register_ex(node, "basic",
                           "tcp://127.0.0.1:7301",
                           "tcp://127.0.0.1:7401",
                           "127.0.0.1", owner_pid) != 0) {
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  printf("zcm_node_unique_name: different owner with same name is rejected\n");
  int dup_rc = zcm_node_register_ex(node, "basic",
                                    "tcp://127.0.0.1:7302",
                                    "tcp://127.0.0.1:7402",
                                    "127.0.0.1", other_pid);
  if (dup_rc != ZCM_NODE_REGISTER_EX_DUPLICATE) {
    fprintf(stderr, "expected duplicate rc=%d got %d\n",
            ZCM_NODE_REGISTER_EX_DUPLICATE, dup_rc);
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  char ep[256] = {0};
  if (zcm_node_lookup(node, "basic", ep, sizeof(ep)) != 0) {
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }
  if (strcmp(ep, "tcp://127.0.0.1:7301") != 0) {
    fprintf(stderr, "unexpected endpoint after duplicate reject: %s\n", ep);
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  zcm_node_free(node);
  zcm_broker_stop(broker);
  zcm_context_free(ctx);
  printf("zcm_node_unique_name: PASS\n");
  return 0;
}
