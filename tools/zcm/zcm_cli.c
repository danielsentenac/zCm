#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static void usage(const char *prog) {
  fprintf(stderr,
          "usage:\n"
          "  %s names\n"
          "  %s send --name NAME -type TYPE -t KIND VALUE\n"
          "  %s kill --name NAME\n"
          "  %s kill NAME\n"
          "\n"
          "KIND: char|short|int|long|float|double|text\n",
          prog, prog, prog, prog);
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

static int do_names(const char *endpoint) {
  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;
  zcm_node_t *node = zcm_node_new(ctx, endpoint);
  if (!node) return 1;

  zcm_node_entry_t *entries = NULL;
  size_t count = 0;
  if (zcm_node_list(node, &entries, &count) != 0) {
    zcm_node_free(node);
    zcm_context_free(ctx);
    return 1;
  }

  for (size_t i = 0; i < count; i++) {
    printf("%s %s\n", entries[i].name, entries[i].endpoint);
  }

  zcm_node_list_free(entries, count);
  zcm_node_free(node);
  zcm_context_free(ctx);
  return 0;
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
  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;
  zcm_node_t *node = zcm_node_new(ctx, endpoint);
  if (!node) return 1;

  char ep[256] = {0};
  if (zcm_node_lookup(node, name, ep, sizeof(ep)) != 0) {
    fprintf(stderr, "zcm: lookup failed for %s\n", name);
    zcm_node_free(node);
    zcm_context_free(ctx);
    return 1;
  }

  zcm_socket_t *pub = zcm_socket_new(ctx, ZCM_SOCK_PUB);
  if (!pub) return 1;
  if (zcm_socket_connect(pub, ep) != 0) {
    fprintf(stderr, "zcm: connect failed\n");
    zcm_socket_free(pub);
    zcm_node_free(node);
    zcm_context_free(ctx);
    return 1;
  }

  usleep(200 * 1000);

  zcm_msg_t *msg = zcm_msg_new();
  zcm_msg_set_type(msg, type);
  if (set_payload(msg, kind, value) != 0) {
    fprintf(stderr, "zcm: invalid payload kind/value\n");
    zcm_msg_free(msg);
    zcm_socket_free(pub);
    zcm_node_free(node);
    zcm_context_free(ctx);
    return 1;
  }

  if (zcm_socket_send_msg(pub, msg) != 0) {
    fprintf(stderr, "zcm: send failed\n");
    zcm_msg_free(msg);
    zcm_socket_free(pub);
    zcm_node_free(node);
    zcm_context_free(ctx);
    return 1;
  }

  zcm_msg_free(msg);
  zcm_socket_free(pub);
  zcm_node_free(node);
  zcm_context_free(ctx);
  return 0;
}

static int do_kill(const char *endpoint, const char *name) {
  zcm_context_t *ctx = zcm_context_new();
  if (!ctx) return 1;
  zcm_node_t *node = zcm_node_new(ctx, endpoint);
  if (!node) return 1;

  char ctrl_ep[512] = {0};
  char host[256] = {0};
  int pid = 0;
  if (zcm_node_info(node, name, NULL, 0, ctrl_ep, sizeof(ctrl_ep), host, sizeof(host), &pid) != 0) {
    fprintf(stderr, "zcm: kill failed (no info for %s)\n", name);
    zcm_node_free(node);
    zcm_context_free(ctx);
    return 1;
  }
  if (ctrl_ep[0] == '\0') {
    fprintf(stderr, "zcm: kill failed (no control endpoint for %s)\n", name);
    zcm_node_free(node);
    zcm_context_free(ctx);
    return 1;
  }

  zcm_socket_t *req = zcm_socket_new(ctx, ZCM_SOCK_REQ);
  if (!req) {
    zcm_node_free(node);
    zcm_context_free(ctx);
    return 1;
  }
  if (zcm_socket_connect(req, ctrl_ep) != 0) {
    fprintf(stderr, "zcm: kill failed (connect control endpoint)\n");
    zcm_socket_free(req);
    zcm_node_free(node);
    zcm_context_free(ctx);
    return 1;
  }
  const char *cmd = "SHUTDOWN";
  if (zcm_socket_send_bytes(req, cmd, strlen(cmd)) != 0) {
    fprintf(stderr, "zcm: kill failed (send shutdown)\n");
  }
  zcm_socket_free(req);

  zcm_node_free(node);
  zcm_context_free(ctx);
  return 0;
}

int main(int argc, char **argv) {
  const char *endpoint = NULL;
  const char *cmd = NULL;
  const char *name = NULL;
  const char *type = NULL;
  const char *kind = NULL;
  const char *value = NULL;

  for (int i = 1; i < argc; i++) {
    if (!cmd) {
      cmd = argv[i];
    } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
      name = argv[++i];
    } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
      type = argv[++i];
    } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      kind = argv[++i];
    } else if (!value) {
      if (strcmp(cmd, "kill") == 0 && !name) {
        name = argv[i];
      } else {
        value = argv[i];
      }
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  if (!cmd || (strcmp(cmd, "names") != 0 && strcmp(cmd, "send") != 0 && strcmp(cmd, "kill") != 0)) {
    usage(argv[0]);
    return 1;
  }

  char *ep = NULL;
  (void)endpoint;
  ep = load_endpoint_from_config();
  if (!ep) {
    fprintf(stderr, "zcm: missing ZCMDOMAIN or ZCmDomains entry\n");
    return 1;
  }

  int rc = 0;
  if (strcmp(cmd, "names") == 0) {
    rc = do_names_with_timeout(ep, 2000);
    if (rc != 0) {
      free(ep);
      return 1;
    }
  } else if (strcmp(cmd, "send") == 0) {
    if (!name || !type || !kind || !value) {
      usage(argv[0]);
      free(ep);
      return 1;
    }
    rc = do_send(ep, name, type, kind, value);
  } else {
    if (!name) {
      usage(argv[0]);
      free(ep);
      return 1;
    }
    rc = do_kill(ep, name);
  }
  free(ep);
  return rc;
}
#include <pthread.h>
