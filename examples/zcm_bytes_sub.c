#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_proc.h"

#include <stdio.h>

int main(int argc, char **argv) {
  const char *target = "procbytes";
  const char *self_name = "procbytesub";
  if (argc > 1) target = argv[1];
  if (argc > 2) self_name = argv[2];

  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_SUB, 0, &proc, NULL) != 0) return 1;
  zcm_context_t *ctx = zcm_proc_context(proc);
  zcm_node_t *node = zcm_proc_node(proc);

  char ep[256] = {0};
  if (zcm_node_lookup(node, target, ep, sizeof(ep)) != 0) {
    fprintf(stderr, "lookup failed\n");
    return 1;
  }

  zcm_socket_t *sub = zcm_socket_new(ctx, ZCM_SOCK_SUB);
  if (!sub) return 1;
  if (zcm_socket_connect(sub, ep) != 0) {
    fprintf(stderr, "connect failed\n");
    return 1;
  }
  if (zcm_socket_set_subscribe(sub, "", 0) != 0) {
    fprintf(stderr, "subscribe failed\n");
    return 1;
  }

  for (int i = 0; i < 5; i++) {
    char buf[256] = {0};
    size_t n = 0;
    if (zcm_socket_recv_bytes(sub, buf, sizeof(buf) - 1, &n) == 0) {
      buf[n] = '\0';
      printf("received bytes: %s\n", buf);
    }
  }

  zcm_socket_free(sub);
  zcm_proc_free(proc);
  return 0;
}
