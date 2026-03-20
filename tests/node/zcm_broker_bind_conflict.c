#include "zcm/zcm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

int main(void) {
  int rc = 1;
  zcm_context_t *ctx = NULL;
  zcm_broker_t *a = NULL;
  zcm_broker_t *b = NULL;
  char endpoint[128] = {0};

  int port = pick_free_tcp_port();
  if (port <= 0) {
    printf("zcm_broker_bind_conflict: SKIP (no local TCP port allocation available)\n");
    return 0;
  }

  snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%d", port);
  ctx = zcm_context_new();
  if (!ctx) goto done;

  a = zcm_broker_start(ctx, endpoint);
  if (!a) {
    printf("zcm_broker_bind_conflict: SKIP (unable to bind first broker)\n");
    rc = 0;
    goto done;
  }
  if (!zcm_broker_is_running(a)) {
    fprintf(stderr, "zcm_broker_bind_conflict: first broker not running\n");
    goto done;
  }

  b = zcm_broker_start(ctx, endpoint);
  if (b) {
    fprintf(stderr, "zcm_broker_bind_conflict: second broker unexpectedly started\n");
    goto done;
  }

  printf("zcm_broker_bind_conflict: PASS\n");
  rc = 0;

done:
  if (b) zcm_broker_stop(b);
  if (a) zcm_broker_stop(a);
  if (ctx) zcm_context_free(ctx);
  return rc;
}
