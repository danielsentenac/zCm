#include "zcm/zcm_proc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

struct zcm_proc {
  zcm_context_t *ctx;
  zcm_node_t *node;
  zcm_socket_t *ctrl;
  zcm_socket_t *data;
  char *name;
  pthread_t ctrl_thread;
  int stop;
};

static int load_domain_info(char **broker_ep, char **host_out, int *first_port, int *range_size) {
  const char *domain = getenv("ZCMDOMAIN");
  if (!domain || !*domain) return -1;

  const char *env = getenv("ZCMDOMAIN_DATABASE");
  if (!env || !*env) env = getenv("ZCMMGR");

  char file_name[512];
  if (env && *env) {
    snprintf(file_name, sizeof(file_name), "%s/ZCmDomains", env);
  } else {
    const char *root = getenv("ZCMROOT");
    if (!root || !*root) return -1;
    snprintf(file_name, sizeof(file_name), "%s/mgr/ZCmDomains", root);
  }

  FILE *f = fopen(file_name, "r");
  if (!f) return -1;

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    if (line[0] == '#' || line[0] == '\0') continue;

    char *p = line;
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_domain = strsep(&p, " \t");
    if (!tok_domain || strcmp(tok_domain, domain) != 0) continue;

    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_host = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_port = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_first = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_range = strsep(&p, " \t");

    if (!tok_host || !tok_port || !*tok_host || !*tok_port) {
      fclose(f);
      return -1;
    }

    char *endpoint = malloc(512);
    if (!endpoint) {
      fclose(f);
      return -1;
    }
    snprintf(endpoint, 512, "tcp://%s:%s", tok_host, tok_port);
    fclose(f);
    *broker_ep = endpoint;
    if (host_out) *host_out = strdup(tok_host);
    if (first_port) *first_port = tok_first ? atoi(tok_first) : 0;
    if (range_size) *range_size = tok_range ? atoi(tok_range) : 0;
    return 0;
  }

  fclose(f);
  return -1;
}

static void *ctrl_thread_main(void *arg) {
  struct zcm_proc *proc = (struct zcm_proc *)arg;
  for (;;) {
    char ctrl_buf[64] = {0};
    size_t ctrl_len = 0;
    if (proc->stop) break;
    if (zcm_socket_recv_bytes(proc->ctrl, ctrl_buf, sizeof(ctrl_buf) - 1, &ctrl_len) == 0) {
      ctrl_buf[ctrl_len] = '\0';
      if (strcmp(ctrl_buf, "SHUTDOWN") == 0) {
        const char *ok = "OK";
        zcm_socket_send_bytes(proc->ctrl, ok, strlen(ok));
        zcm_node_unregister(proc->node, proc->name);
        exit(0);
      }
    } else {
      if (proc->stop) break;
    }
  }
  return NULL;
}

static int bind_in_range(zcm_socket_t *sock, int first_port, int range_size, int *out_port, int skip_port) {
  if (first_port <= 0) first_port = 7000;
  if (range_size <= 0) range_size = 100;
  for (int i = 0; i < range_size; i++) {
    int port = first_port + i;
    if (port == skip_port) continue;
    char bind_ep[256];
    snprintf(bind_ep, sizeof(bind_ep), "tcp://0.0.0.0:%d", port);
    if (zcm_socket_bind(sock, bind_ep) == 0) {
      if (out_port) *out_port = port;
      return 0;
    }
  }
  return -1;
}

int zcm_proc_init(const char *name, zcm_socket_type_t data_type, int bind_data,
                  zcm_proc_t **out_proc, zcm_socket_t **out_data) {
  if (!name || !out_proc) return -1;

  char *broker = NULL;
  char *host = NULL;
  int first_port = 0;
  int range_size = 0;
  if (load_domain_info(&broker, &host, &first_port, &range_size) != 0) {
    fprintf(stderr, "zcm_proc: missing ZCMDOMAIN or ZCmDomains entry\n");
    return -1;
  }

  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return -1;
  zcm_node_t *node = zcm_node_new(ctx, broker);
  if (!node) return -1;

  zcm_socket_t *data = NULL;
  int data_port = -1;
  if (bind_data) {
    data = zcm_socket_new(ctx, data_type);
    if (!data) return -1;
    if (bind_in_range(data, first_port, range_size, &data_port, -1) != 0) {
      fprintf(stderr, "zcm_proc: data bind failed\n");
      return -1;
    }
  }

  zcm_socket_t *ctrl = zcm_socket_new(ctx, ZCM_SOCK_REP);
  if (!ctrl) return -1;
  zcm_socket_set_timeouts(ctrl, 200);
  int ctrl_port = -1;
  if (bind_in_range(ctrl, first_port, range_size, &ctrl_port, data_port) != 0) {
    fprintf(stderr, "zcm_proc: control bind failed\n");
    return -1;
  }

  const char *use_host = host ? host : "127.0.0.1";
  char data_reg_ep[256] = {0};
  char ctrl_reg_ep[256] = {0};
  if (data_port > 0) snprintf(data_reg_ep, sizeof(data_reg_ep), "tcp://%s:%d", use_host, data_port);
  snprintf(ctrl_reg_ep, sizeof(ctrl_reg_ep), "tcp://%s:%d", use_host, ctrl_port);

  const char *reg_ep = (data_port > 0) ? data_reg_ep : ctrl_reg_ep;
  if (zcm_node_register_ex(node, name, reg_ep, ctrl_reg_ep, use_host, getpid()) != 0) {
    fprintf(stderr, "zcm_proc: register failed\n");
    return -1;
  }

  zcm_proc_t *proc = calloc(1, sizeof(*proc));
  if (!proc) return -1;
  proc->ctx = ctx;
  proc->node = node;
  proc->ctrl = ctrl;
  proc->data = data;
  proc->name = strdup(name);
  proc->stop = 0;

  pthread_create(&proc->ctrl_thread, NULL, ctrl_thread_main, proc);

  *out_proc = proc;
  if (out_data) *out_data = data;

  free(broker);
  free(host);
  return 0;
}

void zcm_proc_free(zcm_proc_t *proc) {
  if (!proc) return;
  zcm_node_unregister(proc->node, proc->name);
  proc->stop = 1;
  pthread_join(proc->ctrl_thread, NULL);
  if (proc->ctrl) zcm_socket_free(proc->ctrl);
  if (proc->data) zcm_socket_free(proc->data);
  if (proc->node) zcm_node_free(proc->node);
  if (proc->ctx) zcm_context_free(proc->ctx);
  free(proc->name);
  free(proc);
}

zcm_context_t *zcm_proc_context(zcm_proc_t *proc) {
  return proc ? proc->ctx : NULL;
}

zcm_node_t *zcm_proc_node(zcm_proc_t *proc) {
  return proc ? proc->node : NULL;
}
