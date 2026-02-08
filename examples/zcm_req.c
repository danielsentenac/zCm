#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"
#include "zcm/zcm_proc.h"

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *service = "echo.service";
  const char *self_name = "echo.client";
  int count = 1;
  if (argc > 1) service = argv[1];
  if (argc > 2) self_name = argv[2];
  if (argc > 3) count = atoi(argv[3]);

  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_REQ, 0, &proc, NULL) != 0) return 1;
  zcm_context_t *ctx = zcm_proc_context(proc);
  zcm_node_t *node = zcm_proc_node(proc);

  char ep[256] = {0};
  if (zcm_node_lookup(node, service, ep, sizeof(ep)) != 0) {
    fprintf(stderr, "lookup failed\n");
    return 1;
  }

  zcm_socket_t *req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) return 1;
  if (zcm_socket_connect(req, ep) != 0) {
    fprintf(stderr, "connect failed\n");
    return 1;
  }

  for (int i = 0; i < count; i++) {
    zcm_msg_t *msg = zcm_msg_new();
    zcm_msg_set_type(msg, "QUERY");
    zcm_msg_put_text(msg, "ping");
    zcm_msg_put_int(msg, 42 + i);

    if (zcm_socket_send_msg(req, msg) != 0) {
      fprintf(stderr, "send failed\n");
      zcm_msg_free(msg);
      return 1;
    }
    zcm_msg_free(msg);

    zcm_msg_t *reply = zcm_msg_new();
    if (zcm_socket_recv_msg(req, reply) == 0) {
      const char *text = NULL;
      int32_t code = 0;
      if (zcm_msg_get_text(reply, &text, NULL) == 0 &&
          zcm_msg_get_int(reply, &code) == 0) {
        printf("Received reply: type=%s text=%s code=%d\n",
               zcm_msg_get_type(reply), text, code);
      } else {
        printf("reply decode error: %s\n", zcm_msg_last_error(reply));
      }
    }
    zcm_msg_free(reply);
    usleep(200 * 1000);
  }

  zcm_socket_free(req);
  zcm_proc_free(proc);
  return 0;
}
