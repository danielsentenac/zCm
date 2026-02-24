#include "zcm/zcm.h"
#include "zcm/zcm_node.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;

  printf("zcm_node_list: start broker\n");
  zcm_broker_t *broker = zcm_broker_start(ctx, "inproc://zcm-broker-list");
  if (!broker) {
    zcm_context_free(ctx);
    return 1;
  }

  printf("zcm_node_list: register two names\n");
  zcm_node_t *node = zcm_node_new(ctx, "inproc://zcm-broker-list");
  if (!node) return 1;

  zcm_node_register_ex(node, "a",
                       "tcp://127.0.0.1:7001",
                       "tcp://127.0.0.1:7101",
                       "127.0.0.1", (int)getpid(),
                       "PUB", 7001, -1);
  zcm_node_register_ex(node, "b",
                       "tcp://127.0.0.1:7002",
                       "tcp://127.0.0.1:7102",
                       "127.0.0.1", (int)getpid(),
                       "PUB", 7002, -1);
  if (zcm_node_register_ex(node, "ifacepub", "tcp://eth0:7003",
                           "tcp://publisher-host:7004",
                           "publisher-host", 1234,
                           "PUB", 7003, -1) != 0) {
    return 1;
  }
  if (zcm_node_register_ex(node, "bad-role",
                           "tcp://127.0.0.1:7010",
                           "tcp://127.0.0.1:7110",
                           "127.0.0.1", (int)getpid(),
                           "UNKNOWN", -1, -1) == 0) {
    return 1;
  }
  if (zcm_node_register_ex(node, "bad-pub-port",
                           "tcp://127.0.0.1:7011",
                           "tcp://127.0.0.1:7111",
                           "127.0.0.1", (int)getpid(),
                           "PUB", -1, -1) == 0) {
    return 1;
  }

  printf("zcm_node_list: verify lookup endpoint normalization\n");
  char lookup_ep[512] = {0};
  if (zcm_node_lookup(node, "ifacepub", lookup_ep, sizeof(lookup_ep)) != 0) return 1;
  if (strcmp(lookup_ep, "tcp://publisher-host:7003") != 0) return 1;

  printf("zcm_node_list: list registry and verify\n");
  zcm_node_entry_t *entries = NULL;
  size_t count = 0;
  if (zcm_node_list(node, &entries, &count) != 0) return 1;
  if (count < 3) return 1;

  int found_a = 0, found_b = 0;
  for (size_t i = 0; i < count; i++) {
    if (strcmp(entries[i].name, "a") == 0) found_a = 1;
    if (strcmp(entries[i].name, "b") == 0) found_b = 1;
  }

  zcm_node_list_free(entries, count);
  zcm_node_free(node);
  zcm_broker_stop(broker);
  zcm_context_free(ctx);

  if (found_a && found_b) {
    printf("zcm_node_list: PASS\n");
    return 0;
  }
  return 1;
}
