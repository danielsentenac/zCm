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

static void usage(const char *prog) {
  fprintf(stderr,
          "usage:\n"
          "  %s names\n"
          "  %s send NAME [-type TYPE] (-t TEXT | -d DOUBLE | -f FLOAT | -i INTEGER)\n"
          "  %s kill NAME\n"
          "  %s ping NAME\n"
          "  %s broker [ping|stop|list]\n",
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

typedef enum {
  SEND_VALUE_NONE = 0,
  SEND_VALUE_TEXT = 1,
  SEND_VALUE_DOUBLE = 2,
  SEND_VALUE_FLOAT = 3,
  SEND_VALUE_INT = 4
} send_value_kind_t;

static int parse_int32_str(const char *text, int32_t *out) {
  if (!text || !out) return -1;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return -1;
  if (v < INT32_MIN || v > INT32_MAX) return -1;
  *out = (int32_t)v;
  return 0;
}

static int parse_float_str(const char *text, float *out) {
  if (!text || !out) return -1;
  char *end = NULL;
  float v = strtof(text, &end);
  if (!end || *end != '\0') return -1;
  *out = v;
  return 0;
}

static int parse_double_str(const char *text, double *out) {
  if (!text || !out) return -1;
  char *end = NULL;
  double v = strtod(text, &end);
  if (!end || *end != '\0') return -1;
  *out = v;
  return 0;
}

static int set_core_payload(zcm_msg_t *msg, send_value_kind_t kind, const char *value) {
  if (!msg || !value) return -1;
  switch (kind) {
    case SEND_VALUE_TEXT:
      return zcm_msg_put_core_text(msg, value);
    case SEND_VALUE_DOUBLE: {
      double v = 0.0;
      if (parse_double_str(value, &v) != 0) return -1;
      return zcm_msg_put_core_double(msg, v);
    }
    case SEND_VALUE_FLOAT: {
      float v = 0.0f;
      if (parse_float_str(value, &v) != 0) return -1;
      return zcm_msg_put_core_float(msg, v);
    }
    case SEND_VALUE_INT: {
      int32_t v = 0;
      if (parse_int32_str(value, &v) != 0) return -1;
      return zcm_msg_put_core_int(msg, v);
    }
    default:
      return -1;
  }
}

static int do_send(const char *endpoint, const char *name, const char *type,
                   send_value_kind_t kind, const char *value) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_node_t *node = NULL;
  zcm_socket_t *req = NULL;
  zcm_msg_t *msg = NULL;
  zcm_msg_t *reply = NULL;

  if (!ctx) return 1;
  node = zcm_node_new(ctx, endpoint);
  if (!node) goto out;

  char ep[256] = {0};
  if (zcm_node_lookup(node, name, ep, sizeof(ep)) != 0) {
    fprintf(stderr, "zcm: lookup failed for %s\n", name);
    goto out;
  }

  req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) goto out;
  if (zcm_socket_connect(req, ep) != 0) {
    fprintf(stderr, "zcm: connect failed\n");
    goto out;
  }
  zcm_socket_set_timeouts(req, 1000);

  msg = zcm_msg_new();
  if (!msg) goto out;
  zcm_msg_set_type(msg, type);
  if (set_core_payload(msg, kind, value) != 0) {
    fprintf(stderr, "zcm: invalid payload value for selected flag\n");
    goto out;
  }

  if (zcm_socket_send_msg(req, msg) != 0) {
    fprintf(stderr, "zcm: send failed\n");
    goto out;
  }

  reply = zcm_msg_new();
  if (!reply) goto out;
  if (zcm_socket_recv_msg(req, reply) != 0) {
    fprintf(stderr, "zcm: no reply from %s\n", name);
    goto out;
  }

  zcm_core_value_t core;
  zcm_msg_rewind(reply);
  if (zcm_msg_get_core(reply, &core) == 0) {
    if (core.kind == ZCM_CORE_VALUE_TEXT) {
      printf("ack %s %.*s\n", zcm_msg_get_type(reply), (int)core.text_len, core.text);
    } else if (core.kind == ZCM_CORE_VALUE_DOUBLE) {
      printf("ack %s %f\n", zcm_msg_get_type(reply), core.d);
    } else if (core.kind == ZCM_CORE_VALUE_FLOAT) {
      printf("ack %s %f\n", zcm_msg_get_type(reply), core.f);
    } else if (core.kind == ZCM_CORE_VALUE_INT) {
      printf("ack %s %d\n", zcm_msg_get_type(reply), core.i);
    } else {
      printf("ack %s\n", zcm_msg_get_type(reply));
    }
  } else {
    const char *text = NULL;
    uint32_t text_len = 0;
    zcm_msg_rewind(reply);
    if (zcm_msg_get_text(reply, &text, &text_len) == 0) {
      printf("ack %s %.*s\n", zcm_msg_get_type(reply), (int)text_len, text);
    } else {
      printf("ack %s\n", zcm_msg_get_type(reply));
    }
  }

  rc = 0;

out:
  if (reply) zcm_msg_free(reply);
  if (msg) zcm_msg_free(msg);
  if (req) zcm_socket_free(req);
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

static int parse_send_args(int argc, char **argv,
                           const char **name,
                           const char **type,
                           send_value_kind_t *kind,
                           const char **value) {
  if (argc < 3) return -1;
  *name = argv[2];
  *type = "CORE";
  *kind = SEND_VALUE_NONE;
  *value = NULL;

  for (int i = 3; i < argc; i++) {
    if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
      *type = argv[++i];
    } else if ((strcmp(argv[i], "-t") == 0 ||
                strcmp(argv[i], "-d") == 0 ||
                strcmp(argv[i], "-f") == 0 ||
                strcmp(argv[i], "-i") == 0) && i + 1 < argc) {
      if (*kind != SEND_VALUE_NONE || *value != NULL) return -1;
      if (strcmp(argv[i], "-t") == 0) *kind = SEND_VALUE_TEXT;
      else if (strcmp(argv[i], "-d") == 0) *kind = SEND_VALUE_DOUBLE;
      else if (strcmp(argv[i], "-f") == 0) *kind = SEND_VALUE_FLOAT;
      else *kind = SEND_VALUE_INT;
      *value = argv[++i];
    } else {
      return -1;
    }
  }

  return (*name && *kind != SEND_VALUE_NONE && *value) ? 0 : -1;
}

int main(int argc, char **argv) {
  const char *cmd = NULL;
  const char *sub = NULL;
  const char *name = NULL;
  const char *type = NULL;
  send_value_kind_t kind = SEND_VALUE_NONE;
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
    sub = (argc >= 3) ? argv[2] : NULL;
    if (sub && (strcmp(sub, "ping") == 0 || strcmp(sub, "stop") == 0 || strcmp(sub, "list") == 0)) {
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
