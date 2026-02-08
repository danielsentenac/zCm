#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_proc.h"

#include <stdio.h>

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const char *self_name = "zcm_list";

  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_REQ, 0, &proc, NULL) != 0) return 1;
  zcm_node_t *node = zcm_proc_node(proc);

  zcm_node_entry_t *entries = NULL;
  size_t count = 0;
  if (zcm_node_list(node, &entries, &count) != 0) {
    fprintf(stderr, "list failed\n");
    return 1;
  }

  for (size_t i = 0; i < count; i++) {
    printf("%s %s\n", entries[i].name, entries[i].endpoint);
  }

  zcm_node_list_free(entries, count);
  zcm_proc_free(proc);
  return 0;
}
