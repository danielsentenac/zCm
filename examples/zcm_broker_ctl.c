#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

#include <zmq.h>

static int send_cmd(const char *endpoint, const char *cmd) {
  void *ctx = zmq_ctx_new();
  if (!ctx) return 1;
  void *sock = zmq_socket(ctx, ZMQ_REQ);
  if (!sock) {
    zmq_ctx_term(ctx);
    return 1;
  }
  int to = 1000;
  zmq_setsockopt(sock, ZMQ_RCVTIMEO, &to, sizeof(to));
  zmq_setsockopt(sock, ZMQ_SNDTIMEO, &to, sizeof(to));
  if (zmq_connect(sock, endpoint) != 0) {
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return 1;
  }
  if (zmq_send(sock, cmd, strlen(cmd), 0) < 0) {
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return 1;
  }
  char reply[32] = {0};
  int n = zmq_recv(sock, reply, sizeof(reply) - 1, 0);
  zmq_close(sock);
  zmq_ctx_term(ctx);
  if (n <= 0) return 1;
  printf("%s\n", reply);
  return 0;
}

static int send_list(const char *endpoint) {
  void *ctx = zmq_ctx_new();
  if (!ctx) return 1;
  void *sock = zmq_socket(ctx, ZMQ_REQ);
  if (!sock) {
    zmq_ctx_term(ctx);
    return 1;
  }
  int to = 1000;
  zmq_setsockopt(sock, ZMQ_RCVTIMEO, &to, sizeof(to));
  zmq_setsockopt(sock, ZMQ_SNDTIMEO, &to, sizeof(to));
  if (zmq_connect(sock, endpoint) != 0) {
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return 1;
  }
  if (zmq_send(sock, "LIST", 4, 0) < 0) {
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return 1;
  }

  char status[16] = {0};
  if (zmq_recv(sock, status, sizeof(status) - 1, 0) <= 0) {
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return 1;
  }
  if (strcmp(status, "OK") != 0) {
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return 1;
  }

  int count = 0;
  if (zmq_recv(sock, &count, sizeof(count), 0) <= 0) {
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return 1;
  }

  for (int i = 0; i < count; i++) {
    char name[256] = {0};
    char endpoint_buf[512] = {0};
    int n = zmq_recv(sock, name, sizeof(name) - 1, 0);
    if (n <= 0) break;
    name[n] = '\0';

    n = zmq_recv(sock, endpoint_buf, sizeof(endpoint_buf) - 1, 0);
    if (n <= 0) break;
    endpoint_buf[n] = '\0';

    printf("%s %s\n", name, endpoint_buf);
  }

  zmq_close(sock);
  zmq_ctx_term(ctx);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <ping|stop|list>\n", argv[0]);
    return 1;
  }

  char *endpoint = load_endpoint_from_config();
  if (!endpoint) {
    fprintf(stderr, "zcm_broker_ctl: missing ZCMDOMAIN or ZCmDomains entry\n");
    return 1;
  }

  const char *cmd = argv[1];
  int rc = 0;
  if (strcmp(cmd, "ping") == 0) rc = send_cmd(endpoint, "PING");
  else if (strcmp(cmd, "stop") == 0) rc = send_cmd(endpoint, "STOP");
  else if (strcmp(cmd, "list") == 0) rc = send_list(endpoint);
  else {
    fprintf(stderr, "unknown command: %s\n", cmd);
    rc = 1;
  }
  free(endpoint);
  return rc;
}
