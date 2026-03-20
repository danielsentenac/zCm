#include "zcm/zcm.h"
#include "zcm/zcm_domain.h"
#include "zcm/zcm_node.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_sig(int sig) {
  (void)sig;
  g_stop = 1;
}

static int broker_ping_endpoint(const char *endpoint) {
  int ok = 0;
  zcm_context_t *ctx = NULL;
  zcm_socket_t *req = NULL;
  char reply[64] = {0};
  size_t reply_len = 0;

  if (!endpoint || !*endpoint) return 0;

  ctx = zcm_context_new();
  if (!ctx) return 0;
  req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) goto out;

  zcm_socket_set_timeouts(req, 1000);
  if (zcm_socket_connect(req, endpoint) != 0) goto out;
  if (zcm_socket_send_bytes(req, "PING", 4) != 0) goto out;
  if (zcm_socket_recv_bytes(req, reply, sizeof(reply) - 1, &reply_len) != 0) goto out;
  reply[reply_len] = '\0';
  ok = (strcmp(reply, "PONG") == 0);

out:
  if (req) zcm_socket_free(req);
  if (ctx) zcm_context_free(ctx);
  return ok;
}

static int build_domain_lock_path(const zcm_domain_info_t *info,
                                  char *out_path, size_t out_size) {
  const char *slash = NULL;
  size_t dir_len = 0;

  if (!info || !info->has_domain_mapping || !out_path || out_size == 0) return -1;
  slash = strrchr(info->domains_file, '/');
  if (!slash) return -1;
  dir_len = (size_t)(slash - info->domains_file);
  if (dir_len == 0) dir_len = 1;

  if (snprintf(out_path, out_size, "%.*s/.zcm_broker_%s.lock",
               (int)dir_len, info->domains_file, info->domain) >= (int)out_size) {
    return -1;
  }
  return 0;
}

static int acquire_domain_lock(const zcm_domain_info_t *info, char *out_path,
                               size_t out_size, int timeout_ms) {
  int waited_ms = 0;

  if (!info || !info->has_domain_mapping) {
    if (out_path && out_size > 0) out_path[0] = '\0';
    return 0;
  }
  if (build_domain_lock_path(info, out_path, out_size) != 0) return -1;

  while (!g_stop) {
    if (mkdir(out_path, 0775) == 0) return 0;
    if (errno != EEXIST) return -1;
    if (timeout_ms >= 0 && waited_ms >= timeout_ms) return 1;
    usleep(100000);
    waited_ms += 100;
  }
  return 1;
}

static void release_domain_lock(const char *lock_path) {
  if (!lock_path || !*lock_path) return;
  (void)rmdir(lock_path);
}

int main(void) {
  int rc = 1;
  int have_lock = 0;
  char lock_path[ZCM_DOMAIN_PATH_MAX] = {0};
  zcm_domain_info_t info;
  zcm_context_t *ctx = NULL;
  zcm_broker_t *broker = NULL;

  if (zcm_domain_info_load(&info) != 0) {
    fprintf(stderr, "zcm_broker: missing ZCMDOMAIN or ZCmDomains entry\n");
    goto out;
  }

  if (broker_ping_endpoint(info.query_endpoint)) {
    printf("zcm_broker: already running at %s\n", info.query_endpoint);
    rc = 0;
    goto out;
  }

  rc = acquire_domain_lock(&info, lock_path, sizeof(lock_path), 5000);
  if (rc != 0) {
    if (broker_ping_endpoint(info.query_endpoint)) {
      printf("zcm_broker: already running at %s\n", info.query_endpoint);
      rc = 0;
      goto out;
    }
    fprintf(stderr, "zcm_broker: startup lock busy for domain %s\n",
            (info.domain[0] ? info.domain : "-"));
    rc = 1;
    goto out;
  }
  have_lock = info.has_domain_mapping;

  if (broker_ping_endpoint(info.query_endpoint)) {
    printf("zcm_broker: already running at %s\n", info.query_endpoint);
    rc = 0;
    goto out;
  }

  ctx = zcm_context_new();
  if (!ctx) goto out;

  broker = zcm_broker_start(ctx, info.bind_endpoint);
  if (!broker) {
    if (broker_ping_endpoint(info.query_endpoint)) {
      printf("zcm_broker: another broker became active at %s\n", info.query_endpoint);
      rc = 0;
      goto out;
    }
    fprintf(stderr, "zcm_broker: start failed for %s\n", info.bind_endpoint);
    goto out;
  }

  if (info.has_domain_mapping && zcm_domain_info_update_published_host(&info) != 0) {
    fprintf(stderr,
            "zcm_broker: warning: could not update ZCmDomains "
            "(check ZCMDOMAIN and write permissions)\n");
  } else if (info.has_domain_mapping) {
    printf("zcm_broker: updated %s domain=%s host=%s port=%d\n",
           info.domains_file, info.domain, info.publish_host, info.port);
    fflush(stdout);
  }

  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);

  if (have_lock) {
    release_domain_lock(lock_path);
    have_lock = 0;
  }

  printf("zcm_broker listening on %s (Ctrl+C to stop)\n", info.bind_endpoint);
  fflush(stdout);

  while (!g_stop && zcm_broker_is_running(broker)) {
    sleep(1);
  }

  rc = 0;

out:
  if (have_lock) release_domain_lock(lock_path);
  if (broker) zcm_broker_stop(broker);
  if (ctx) zcm_context_free(ctx);
  return rc;
}
