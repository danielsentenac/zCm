#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"
#include "zcm/zcm_proc.h"

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *target = "helloworld";
  const char *self_name = "helloworld.send";
  if (argc > 1) target = argv[1];
  if (argc > 2) self_name = argv[2];

  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_PUB, 0, &proc, NULL) != 0) return 1;
  zcm_context_t *ctx = zcm_proc_context(proc);
  zcm_node_t *node = zcm_proc_node(proc);

  char ep[256] = {0};
  if (zcm_node_lookup(node, target, ep, sizeof(ep)) != 0) {
    fprintf(stderr, "lookup failed\n");
    return 1;
  }

  zcm_socket_t *pub = zcm_socket_new(ctx, ZCM_SOCK_PUB);
  if (!pub) return 1;
  if (zcm_socket_connect(pub, ep) != 0) {
    fprintf(stderr, "connect failed\n");
    return 1;
  }

  usleep(200 * 1000);

  zcm_msg_t *msg = zcm_msg_new();
  zcm_msg_set_type(msg, "HELLOWORLD");
  zcm_msg_put_double(msg, 1.0);

  if (zcm_socket_send_msg(pub, msg) != 0) {
    fprintf(stderr, "send failed\n");
    zcm_msg_free(msg);
    return 1;
  }

  zcm_msg_free(msg);
  zcm_socket_free(pub);
  zcm_proc_free(proc);
  return 0;
}
