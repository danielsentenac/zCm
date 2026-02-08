#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_proc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *name = "vacuum.bytes";
  if (argc > 1) name = argv[1];

  zcm_proc_t *proc = NULL;
  zcm_socket_t *pub = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_PUB, 1, &proc, &pub) != 0) return 1;

  const char *payload = "raw-bytes-vacuum";
  for (int i = 0; i < 5; i++) {
    if (zcm_socket_send_bytes(pub, payload, strlen(payload)) != 0) {
      fprintf(stderr, "send failed\n");
    } else {
      printf("sent bytes %d\n", i);
    }
    sleep(1);
  }

  zcm_proc_free(proc);
  return 0;
}
