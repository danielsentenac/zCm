#include "zcm/zcm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_sig(int sig) {
  (void)sig;
  g_stop = 1;
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
    while (p && (*p == ' ' || *p == '\t')) p++;
    char *tok_domain = strsep(&p, " \t");
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

int main(void) {
  int rc = 1;
  char *endpoint = load_endpoint_from_config();
  zcm_context_t *ctx = NULL;
  zcm_broker_t *broker = NULL;

  if (!endpoint) {
    fprintf(stderr, "zcm_broker: missing ZCMDOMAIN or ZCmDomains entry\n");
    goto out;
  }

  ctx = zcm_context_new();
  if (!ctx) goto out;

  broker = zcm_broker_start(ctx, endpoint);
  if (!broker) {
    fprintf(stderr, "zcm_broker: start failed\n");
    goto out;
  }

  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);

  printf("zcm_broker listening on %s (Ctrl+C to stop)\n", endpoint);
  fflush(stdout);

  while (!g_stop && zcm_broker_is_running(broker)) {
    sleep(1);
  }

  rc = 0;

out:
  if (broker) zcm_broker_stop(broker);
  if (ctx) zcm_context_free(ctx);
  free(endpoint);
  return rc;
}
