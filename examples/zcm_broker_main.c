#include "zcm/zcm.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <zmq.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_sig(int sig) {
  (void)sig;
  g_stop = 1;
}

static int send_stop(const char *endpoint) {
  void *ctx = zmq_ctx_new();
  if (!ctx) return 1;
  void *sock = zmq_socket(ctx, ZMQ_REQ);
  if (!sock) {
    zmq_ctx_term(ctx);
    return 1;
  }
  if (zmq_connect(sock, endpoint) != 0) {
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return 1;
  }
  if (zmq_send(sock, "STOP", 4, 0) < 0) {
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return 1;
  }
  char reply[16] = {0};
  zmq_recv(sock, reply, sizeof(reply) - 1, 0);
  zmq_close(sock);
  zmq_ctx_term(ctx);
  return 0;
}

static char *load_endpoint_from_config(void) {
  const char *domain = getenv("ZCMDOMAIN");
  if (!domain || !*domain) return NULL;

  const char *env = getenv("ZCMDOMAIN_DATABASE");
  if (!env || !*env) env = getenv("ZCMMGR");

  char file_name[512];
  if (env && *env) {
    snprintf(file_name, sizeof(file_name), "%s/ZCmDomains", env);
  } else {
    const char *root = getenv("ZCMROOT");
    if (!root || !*root) return NULL;
    snprintf(file_name, sizeof(file_name), "%s/mgr/ZCmDomains", root);
  }

  FILE *f = fopen(file_name, "r");
  if (!f) return NULL;

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    if (line[0] == '#' || line[0] == '\0') continue;

    char *p = line;
    while (p && (*p == ' ' || *p == '	')) p++;
    char *tok_domain = strsep(&p, " 	");
    if (!tok_domain || strcmp(tok_domain, domain) != 0) continue;

    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_host = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_port = strsep(&p, " \t");

    if (!tok_host || !tok_port || !*tok_host || !*tok_port) {
      fclose(f);
      return NULL;
    }

    char *endpoint = malloc(512);
    if (!endpoint) {
      fclose(f);
      return NULL;
    }
    snprintf(endpoint, 512, "tcp://%s:%s", tok_host, tok_port);
    fclose(f);
    return endpoint;
  }

  fclose(f);
  return NULL;
}

int main(int argc, char **argv) {
  char *endpoint = load_endpoint_from_config();
  if (!endpoint) {
    fprintf(stderr, "zcm-broker: missing ZCMDOMAIN or ZCmDomains entry\n");
    return 1;
  }

  if (argc >= 2 && strcmp(argv[1], "--stop") == 0) {
    int rc = send_stop(endpoint);
    free(endpoint);
    return rc;
  }

  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;

  zcm_broker_t *broker = zcm_broker_start(ctx, endpoint);
  if (!broker) {
    zcm_context_free(ctx);
    return 1;
  }

  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);

  printf("zcm-broker listening on %s (Ctrl+C to stop)\n", endpoint);
  fflush(stdout);

  while (!g_stop) {
    sleep(1);
  }

  zcm_broker_stop(broker);
  zcm_context_free(ctx);
  free(endpoint);
  return 0;
}
