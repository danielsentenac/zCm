#include "zcm/zcm.h"
#include "zcm/zcm_node.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int find_dead_pid(void) {
  int self = (int)getpid();
  int candidate = self + 10000;
  for (int i = 0; i < 50000; i++, candidate++) {
    if (kill((pid_t)candidate, 0) != 0 && errno == ESRCH) return candidate;
  }
  return -1;
}

int main(void) {
  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;

  printf("zcm_node_prune_dead: start broker\n");
  zcm_broker_t *broker = zcm_broker_start(ctx, "inproc://zcm-broker-prune-dead");
  if (!broker) {
    zcm_context_free(ctx);
    return 1;
  }

  zcm_node_t *node = zcm_node_new(ctx, "inproc://zcm-broker-prune-dead");
  if (!node) {
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  int dead_pid = find_dead_pid();
  if (dead_pid <= 0) {
    printf("zcm_node_prune_dead: SKIP (unable to find dead pid)\n");
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 0;
  }

  printf("zcm_node_prune_dead: register ghost with dead pid=%d\n", dead_pid);
  if (zcm_node_register_ex(node, "ghost",
                           "tcp://127.0.0.1:7399",
                           "tcp://127.0.0.1:7499",
                           "127.0.0.1", dead_pid) != 0) {
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  printf("zcm_node_prune_dead: list should prune ghost entry\n");
  zcm_node_entry_t *entries = NULL;
  size_t count = 0;
  if (zcm_node_list(node, &entries, &count) != 0) {
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }
  for (size_t i = 0; i < count; i++) {
    if (strcmp(entries[i].name, "ghost") == 0) {
      fprintf(stderr, "ghost entry still present after prune\n");
      zcm_node_list_free(entries, count);
      zcm_node_free(node);
      zcm_broker_stop(broker);
      zcm_context_free(ctx);
      return 1;
    }
  }
  zcm_node_list_free(entries, count);

  char ep[256] = {0};
  if (zcm_node_lookup(node, "ghost", ep, sizeof(ep)) == 0) {
    fprintf(stderr, "ghost lookup unexpectedly succeeded: %s\n", ep);
    zcm_node_free(node);
    zcm_broker_stop(broker);
    zcm_context_free(ctx);
    return 1;
  }

  zcm_node_free(node);
  zcm_broker_stop(broker);
  zcm_context_free(ctx);
  printf("zcm_node_prune_dead: PASS\n");
  return 0;
}
