#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_proc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int pick_free_tcp_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  socklen_t len = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
    close(fd);
    return -1;
  }

  int port = (int)ntohs(addr.sin_port);
  close(fd);
  return port;
}

static int pick_distinct_ports(int *broker_port, int *first_port) {
  if (!broker_port || !first_port) return -1;
  for (int i = 0; i < 64; i++) {
    int b = pick_free_tcp_port();
    int f = pick_free_tcp_port();
    if (b <= 0 || f <= 0) continue;
    if (b == f) continue;
    if (f > b && f < (b + 128)) continue;
    if (b > f && b < (f + 128)) continue;
    *broker_port = b;
    *first_port = f;
    return 0;
  }
  return -1;
}

int main(void) {
  int rc = 1;
  zcm_context_t *ctx = NULL;
  zcm_broker_t *broker = NULL;
  zcm_node_t *node = NULL;
  zcm_proc_t *proc = NULL;
  char tmp_dir[] = "/tmp/zcm-reannounce-XXXXXX";
  char cfg_path[512] = {0};
  char db_path[512] = {0};

  if (!mkdtemp(tmp_dir)) {
    perror("mkdtemp");
    return 1;
  }

  int broker_port = -1;
  int first_port = -1;
  if (pick_distinct_ports(&broker_port, &first_port) != 0) {
    printf("zcm_proc_reannounce: SKIP (no local TCP port allocation available)\n");
    rc = 0;
    goto done;
  }

  snprintf(db_path, sizeof(db_path), "%s/ZCmDomains", tmp_dir);
  FILE *db = fopen(db_path, "w");
  if (!db) {
    perror("fopen");
    goto done;
  }
  fprintf(db, "reannounce_domain 127.0.0.1 %d %d 64 repo\n", broker_port, first_port);
  fclose(db);

  snprintf(cfg_path, sizeof(cfg_path), "%s/reannounce.cfg", tmp_dir);
  FILE *cfg = fopen(cfg_path, "w");
  if (!cfg) {
    perror("fopen");
    goto done;
  }
  fprintf(cfg,
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<procConfig>\n"
          "  <process name=\"reannounce\">\n"
          "    <control timeoutMs=\"100\"/>\n"
          "    <handlers>\n"
          "      <core pingRequest=\"PING\" pingReply=\"PONG\" defaultReply=\"OK\"/>\n"
          "    </handlers>\n"
          "  </process>\n"
          "</procConfig>\n");
  fclose(cfg);

  setenv("ZCMDOMAIN", "reannounce_domain", 1);
  setenv("ZCMDOMAIN_DATABASE", tmp_dir, 1);
  setenv("ZCM_PROC_CONFIG_FILE", cfg_path, 1);
  setenv("ZCM_PROC_REANNOUNCE_MS", "200", 1);

  char broker_ep[128];
  snprintf(broker_ep, sizeof(broker_ep), "tcp://127.0.0.1:%d", broker_port);

  printf("zcm_proc_reannounce: start broker at %s\n", broker_ep);
  ctx = zcm_context_new();
  if (!ctx) goto done;
  broker = zcm_broker_start(ctx, broker_ep);
  if (!broker) {
    printf("zcm_proc_reannounce: SKIP (unable to bind broker TCP endpoint)\n");
    rc = 0;
    goto done;
  }

  printf("zcm_proc_reannounce: start proc and verify first registration\n");
  if (zcm_proc_init("reannounce", ZCM_SOCK_REP, 0, &proc, NULL) != 0) goto done;
  node = zcm_node_new(ctx, broker_ep);
  if (!node) goto done;

  char ep[256] = {0};
  if (zcm_node_lookup(node, "reannounce", ep, sizeof(ep)) != 0) {
    fprintf(stderr, "zcm_proc_reannounce: initial lookup failed\n");
    goto done;
  }

  printf("zcm_proc_reannounce: restart broker and wait for auto re-register\n");
  zcm_broker_stop(broker);
  broker = NULL;
  broker = zcm_broker_start(ctx, broker_ep);
  if (!broker) goto done;

  int found = 0;
  for (int i = 0; i < 30; i++) {
    if (zcm_node_lookup(node, "reannounce", ep, sizeof(ep)) == 0) {
      found = 1;
      break;
    }
    usleep(200 * 1000);
  }
  if (!found) {
    fprintf(stderr, "zcm_proc_reannounce: re-registration after restart failed\n");
    goto done;
  }

  printf("zcm_proc_reannounce: PASS\n");
  rc = 0;

done:
  if (proc) zcm_proc_free(proc);
  if (node) zcm_node_free(node);
  if (broker) zcm_broker_stop(broker);
  if (ctx) zcm_context_free(ctx);
  if (cfg_path[0]) unlink(cfg_path);
  if (db_path[0]) unlink(db_path);
  rmdir(tmp_dir);
  return rc;
}
