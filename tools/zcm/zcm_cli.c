#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <zmq.h>

static volatile sig_atomic_t g_broker_stop = 0;

static void handle_sig(int sig) {
  (void)sig;
  g_broker_stop = 1;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "usage:\n"
          "  %s names\n"
          "  %s send --name NAME -type TYPE -t KIND VALUE\n"
          "  %s kill NAME\n"
          "  %s ping NAME\n"
          "  %s broker [run|ping|stop|list]\n"
          "\n"
          "KIND: char|short|int|long|float|double|text\n",
          prog, prog, prog, prog, prog);
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

static int do_names(const char *endpoint) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;
  zcm_node_t *node = zcm_node_new(ctx, endpoint);
  if (!node) {
    zcm_context_free(ctx);
    return 1;
  }

  zcm_node_entry_t *entries = NULL;
  size_t count = 0;
  if (zcm_node_list(node, &entries, &count) == 0) {
    for (size_t i = 0; i < count; i++) {
      printf("%s %s\n", entries[i].name, entries[i].endpoint);
    }
    zcm_node_list_free(entries, count);
    rc = 0;
  }

  zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int do_names_with_timeout(const char *endpoint, int timeout_ms) {
  pid_t pid = fork();
  if (pid == 0) {
    int rc = do_names(endpoint);
    _exit(rc);
  }
  if (pid < 0) return 1;

  int status = 0;
  int waited = 0;
  while (waited < timeout_ms) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      if (WIFEXITED(status)) return WEXITSTATUS(status);
      return 1;
    }
    usleep(100 * 1000);
    waited += 100;
  }

  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  fprintf(stderr, "zcm: broker not reachable\n");
  return 1;
}

static int set_payload(zcm_msg_t *msg, const char *kind, const char *value) {
  if (strcmp(kind, "char") == 0) {
    if (!value[0] || value[1]) return -1;
    return zcm_msg_put_char(msg, value[0]);
  }
  if (strcmp(kind, "short") == 0) {
    return zcm_msg_put_short(msg, (int16_t)strtol(value, NULL, 0));
  }
  if (strcmp(kind, "int") == 0) {
    return zcm_msg_put_int(msg, (int32_t)strtol(value, NULL, 0));
  }
  if (strcmp(kind, "long") == 0) {
    return zcm_msg_put_long(msg, (int64_t)strtoll(value, NULL, 0));
  }
  if (strcmp(kind, "float") == 0) {
    return zcm_msg_put_float(msg, strtof(value, NULL));
  }
  if (strcmp(kind, "double") == 0) {
    return zcm_msg_put_double(msg, strtod(value, NULL));
  }
  if (strcmp(kind, "text") == 0) {
    return zcm_msg_put_text(msg, value);
  }
  return -1;
}

static int do_send(const char *endpoint, const char *name, const char *type,
                   const char *kind, const char *value) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;
  zcm_socket_t *pub = NULL;
  zcm_msg_t *msg = NULL;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  char ep[256] = {0};
  if (zcm_node_lookup(node, name, ep, sizeof(ep)) != 0) {
    fprintf(stderr, "zcm: lookup failed for %s\n", name);
    goto out;
  }

  pub = zcm_socket_new(ctx, ZCM_SOCK_PUB);
  if (!pub) goto out;
  if (zcm_socket_connect(pub, ep) != 0) {
    fprintf(stderr, "zcm: connect failed\n");
    goto out;
  }

  usleep(200 * 1000);

  msg = zcm_msg_new();
  if (!msg) goto out;
  zcm_msg_set_type(msg, type);
  if (set_payload(msg, kind, value) != 0) {
    fprintf(stderr, "zcm: invalid payload kind/value\n");
    goto out;
  }

  if (zcm_socket_send_msg(pub, msg) != 0) {
    fprintf(stderr, "zcm: send failed\n");
    goto out;
  }

  rc = 0;

out:
  if (msg) zcm_msg_free(msg);
  if (pub) zcm_socket_free(pub);
  if (node) zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int do_kill(const char *endpoint, const char *name) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;
  zcm_socket_t *req = NULL;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  char ctrl_ep[512] = {0};
  char host[256] = {0};
  int pid = 0;
  if (zcm_node_info(node, name, NULL, 0, ctrl_ep, sizeof(ctrl_ep), host, sizeof(host), &pid) != 0) {
    fprintf(stderr, "zcm: kill failed (no info for %s)\n", name);
    goto out;
  }
  if (ctrl_ep[0] == '\0') {
    fprintf(stderr, "zcm: kill failed (no control endpoint for %s)\n", name);
    goto out;
  }

  req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) goto out;
  if (zcm_socket_connect(req, ctrl_ep) != 0) {
    fprintf(stderr, "zcm: kill failed (connect control endpoint)\n");
    goto out;
  }
  zcm_socket_set_timeouts(req, 1000);
  if (zcm_socket_send_bytes(req, "SHUTDOWN", 8) != 0) {
    fprintf(stderr, "zcm: kill failed (send shutdown)\n");
    goto out;
  }

  char reply[32] = {0};
  size_t n = 0;
  if (zcm_socket_recv_bytes(req, reply, sizeof(reply) - 1, &n) != 0 || n == 0) {
    fprintf(stderr, "zcm: kill failed (no reply)\n");
    goto out;
  }
  reply[n] = '\0';
  if (strcmp(reply, "OK") != 0) {
    fprintf(stderr, "zcm: kill failed (reply=%s)\n", reply);
    goto out;
  }

  rc = 0;

out:
  if (req) zcm_socket_free(req);
  if (node) zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int do_ping(const char *endpoint, const char *name) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;
  zcm_socket_t *req = NULL;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  char ctrl_ep[512] = {0};
  char host[256] = {0};
  int pid = 0;
  if (zcm_node_info(node, name, NULL, 0, ctrl_ep, sizeof(ctrl_ep), host, sizeof(host), &pid) != 0) {
    fprintf(stderr, "zcm: ping failed (no info for %s)\n", name);
    goto out;
  }
  if (ctrl_ep[0] == '\0') {
    fprintf(stderr, "zcm: ping failed (no control endpoint for %s)\n", name);
    goto out;
  }

  req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) goto out;
  if (zcm_socket_connect(req, ctrl_ep) != 0) {
    fprintf(stderr, "zcm: ping failed (connect control endpoint)\n");
    goto out;
  }
  zcm_socket_set_timeouts(req, 1000);
  if (zcm_socket_send_bytes(req, "PING", 4) != 0) {
    fprintf(stderr, "zcm: ping failed (send ping)\n");
    goto out;
  }

  char reply[32] = {0};
  size_t n = 0;
  if (zcm_socket_recv_bytes(req, reply, sizeof(reply) - 1, &n) != 0 || n == 0) {
    fprintf(stderr, "zcm: ping failed (no reply)\n");
    goto out;
  }
  reply[n] = '\0';
  if (strcmp(reply, "PONG") != 0) {
    fprintf(stderr, "zcm: ping failed (reply=%s)\n", reply);
    goto out;
  }

  printf("PONG %s %s %d\n", name, host, pid);
  rc = 0;

out:
  if (req) zcm_socket_free(req);
  if (node) zcm_node_free(node);
  zcm_context_free(ctx);
  return rc;
}

static int do_broker_cmd(const char *endpoint, const char *cmd, const char *expected_reply) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  void *req = NULL;

  if (!ctx) return 1;
  req = zmq_socket(zcm_context_zmq(ctx), ZMQ_REQ);
  if (!req) goto out;

  int timeout_ms = 1000;
  int linger = 0;
  int immediate = 1;
  zmq_setsockopt(req, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
  zmq_setsockopt(req, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
  zmq_setsockopt(req, ZMQ_LINGER, &linger, sizeof(linger));
  zmq_setsockopt(req, ZMQ_IMMEDIATE, &immediate, sizeof(immediate));

  if (zmq_connect(req, endpoint) != 0) {
    fprintf(stderr, "zcm: broker not reachable\n");
    goto out;
  }

  if (zmq_send(req, cmd, strlen(cmd), 0) < 0) {
    fprintf(stderr, "zcm: broker command failed (send %s)\n", cmd);
    goto out;
  }

  char reply[64] = {0};
  int n = zmq_recv(req, reply, sizeof(reply) - 1, 0);
  if (n <= 0) {
    fprintf(stderr, "zcm: broker command failed (no reply for %s)\n", cmd);
    goto out;
  }
  reply[n] = '\0';

  if (expected_reply && strcmp(reply, expected_reply) != 0) {
    fprintf(stderr, "zcm: broker command failed (reply=%s)\n", reply);
    goto out;
  }

  printf("%s\n", reply);
  rc = 0;

out:
  if (req) zmq_close(req);
  zcm_context_free(ctx);
  return rc;
}

static int do_broker_run(const char *endpoint) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_broker_t *broker = NULL;

  if (!ctx) return 1;
  broker = zcm_broker_start(ctx, endpoint);
  if (!broker) {
    fprintf(stderr, "zcm: broker start failed\n");
    goto out;
  }

  g_broker_stop = 0;
  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);

  printf("zcm-broker listening on %s (Ctrl+C to stop)\n", endpoint);
  fflush(stdout);

  while (!g_broker_stop && zcm_broker_is_running(broker)) {
    sleep(1);
  }

  rc = 0;

out:
  if (broker) zcm_broker_stop(broker);
  zcm_context_free(ctx);
  return rc;
}

static int parse_send_args(int argc, char **argv,
                           const char **name,
                           const char **type,
                           const char **kind,
                           const char **value) {
  *name = NULL;
  *type = NULL;
  *kind = NULL;
  *value = NULL;

  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
      *name = argv[++i];
    } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
      *type = argv[++i];
    } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      *kind = argv[++i];
    } else if (!*value) {
      *value = argv[i];
    } else {
      return -1;
    }
  }

  return (*name && *type && *kind && *value) ? 0 : -1;
}

int main(int argc, char **argv) {
  const char *cmd = NULL;
  const char *sub = NULL;
  const char *name = NULL;
  const char *type = NULL;
  const char *kind = NULL;
  const char *value = NULL;

  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  cmd = argv[1];

  if (strcmp(cmd, "names") == 0) {
    if (argc != 2) {
      usage(argv[0]);
      return 1;
    }
  } else if (strcmp(cmd, "send") == 0) {
    if (parse_send_args(argc, argv, &name, &type, &kind, &value) != 0) {
      usage(argv[0]);
      return 1;
    }
  } else if (strcmp(cmd, "kill") == 0) {
    if (argc != 3) {
      usage(argv[0]);
      return 1;
    }
    name = argv[2];
  } else if (strcmp(cmd, "ping") == 0) {
    if (argc != 3) {
      usage(argv[0]);
      return 1;
    }
    name = argv[2];
  } else if (strcmp(cmd, "broker") == 0) {
    sub = (argc >= 3) ? argv[2] : "run";
    if (strcmp(sub, "run") == 0) {
      if (!(argc == 2 || argc == 3)) {
        usage(argv[0]);
        return 1;
      }
    } else if (strcmp(sub, "ping") == 0 || strcmp(sub, "stop") == 0 || strcmp(sub, "list") == 0) {
      if (argc != 3) {
        usage(argv[0]);
        return 1;
      }
    } else {
      usage(argv[0]);
      return 1;
    }
  } else {
    usage(argv[0]);
    return 1;
  }

  char *endpoint = load_endpoint_from_config();
  if (!endpoint) {
    fprintf(stderr, "zcm: missing ZCMDOMAIN or ZCmDomains entry\n");
    return 1;
  }

  int rc = 1;
  if (strcmp(cmd, "names") == 0) {
    rc = do_names_with_timeout(endpoint, 2000);
  } else if (strcmp(cmd, "send") == 0) {
    rc = do_send(endpoint, name, type, kind, value);
  } else if (strcmp(cmd, "kill") == 0) {
    rc = do_kill(endpoint, name);
  } else if (strcmp(cmd, "ping") == 0) {
    rc = do_ping(endpoint, name);
  } else if (strcmp(sub, "run") == 0) {
    rc = do_broker_run(endpoint);
  } else if (strcmp(sub, "ping") == 0) {
    rc = do_broker_cmd(endpoint, "PING", "PONG");
  } else if (strcmp(sub, "stop") == 0) {
    rc = do_broker_cmd(endpoint, "STOP", "OK");
  } else {
    rc = do_names_with_timeout(endpoint, 2000);
  }

  free(endpoint);
  return rc;
}
