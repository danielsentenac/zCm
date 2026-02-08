#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"
#include "zcm/zcm_proc.h"

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *name = "echo.service";
  if (argc > 1) name = argv[1];

  zcm_proc_t *proc = NULL;
  zcm_socket_t *rep = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_REP, 1, &proc, &rep) != 0) return 1;

  for (;;) {
    zcm_msg_t *req = zcm_msg_new();
    if (zcm_socket_recv_msg(rep, req) == 0) {
      const char *text = NULL;
      int32_t code = 0;
      if (zcm_msg_get_text(req, &text, NULL) == 0 &&
          zcm_msg_get_int(req, &code) == 0) {
        printf("Received query: type=%s text=%s code=%d\n",
               zcm_msg_get_type(req), text, code);
      } else {
        printf("query decode error: %s\n", zcm_msg_last_error(req));
      }
    }
    zcm_msg_free(req);

    zcm_msg_t *reply = zcm_msg_new();
    zcm_msg_set_type(reply, "REPLY");
    zcm_msg_put_text(reply, "pong");
    zcm_msg_put_int(reply, 200);
    if (zcm_socket_send_msg(rep, reply) != 0) {
      fprintf(stderr, "reply send failed\n");
    }
    zcm_msg_free(reply);
  }

  zcm_socket_free(rep);
  zcm_proc_free(proc);
  return 0;
}
