#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"
#include "zcm/zcm_proc.h"

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *name = "proc.example";
  if (argc > 1) name = argv[1];

  zcm_proc_t *proc = NULL;
  zcm_socket_t *pub = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_PUB, 1, &proc, &pub) != 0) return 1;

  for (int i = 0;; i++) {
    zcm_msg_t *msg = zcm_msg_new();
    zcm_msg_set_type(msg, "Example");
    zcm_msg_put_int(msg, i);
    zcm_msg_put_text(msg, "hello");

    if (zcm_socket_send_msg(pub, msg) != 0) {
      fprintf(stderr, "send failed\n");
    } else {
      printf("sent example %d\n", i);
    }

    zcm_msg_free(msg);
    sleep(1);
  }

  zcm_proc_free(proc);
  return 0;
}
