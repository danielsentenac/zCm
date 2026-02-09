#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"
#include "zcm/zcm_proc.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
  const char *name = "helloworld";
  int do_daemon = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--daemon") == 0) {
      do_daemon = 1;
    } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
      name = argv[++i];
    } else {
      fprintf(stderr, "usage: %s [--daemon] [--name NAME]\n", argv[0]);
      return 1;
    }
  }

  if (do_daemon) {
    if (daemon(0, 1) != 0) {
      fprintf(stderr, "daemonize failed\n");
      return 1;
    }
  }

  zcm_proc_t *proc = NULL;
  zcm_socket_t *sub = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_SUB, 1, &proc, &sub) != 0) return 1;

  if (zcm_socket_set_subscribe(sub, "", 0) != 0) {
    fprintf(stderr, "subscribe failed\n");
    return 1;
  }

  for (;;) {
    zcm_msg_t *msg = zcm_msg_new();
    if (zcm_socket_recv_msg(sub, msg) == 0) {
      double v = 0.0;
      const char *type = zcm_msg_get_type(msg);
      if (type && zcm_msg_get_double(msg, &v) == 0) {
        if (strcmp(type, "HELLOWORLD") == 0) {
          printf("Received message: %s %.1f\n", type, v);
          fflush(stdout);
        }
      } else {
        fprintf(stderr, "message decode error: %s\n", zcm_msg_last_error(msg));
      }
    }
    zcm_msg_free(msg);
  }

  zcm_proc_free(proc);
  return 0;
}
