#include "zcm/zcm.h"
#include "zcm/zcm_node.h"
#include "zcm/zcm_msg.h"
#include "zcm/zcm_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>

static int parse_int(const char *text, int fallback) {
  if (!text) return fallback;
  char *end = NULL;
  long v = strtol(text, &end, 10);
  if (!end || *end != '\0') return fallback;
  if (v < -1 || v > 1000000) return fallback;
  return (int)v;
}

static int lookup_endpoint(zcm_proc_t *proc, const char *target, char *ep, size_t ep_size) {
  zcm_node_t *node = zcm_proc_node(proc);
  if (!node) return -1;
  if (zcm_node_lookup(node, target, ep, ep_size) != 0) {
    fprintf(stderr, "lookup failed for '%s'\n", target);
    return -1;
  }
  return 0;
}

static int text_equals_nocase(const char *text, uint32_t len, const char *lit) {
  size_t n = strlen(lit);
  if (len != n) return 0;
  return strncasecmp(text, lit, n) == 0;
}

static int run_pub_msg(const char *name, int count) {
  zcm_proc_t *proc = NULL;
  zcm_socket_t *pub = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_PUB, 1, &proc, &pub) != 0) return 1;

  for (int i = 0; count < 0 || i < count; i++) {
    zcm_msg_t *msg = zcm_msg_new();
    if (!msg) {
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_set_type(msg, "ProcStatus");
    zcm_msg_put_int(msg, i);
    zcm_msg_put_text(msg, "proc_ok");

    if (zcm_socket_send_msg(pub, msg) != 0) {
      fprintf(stderr, "send failed\n");
      zcm_msg_free(msg);
      zcm_proc_free(proc);
      return 1;
    }

    printf("sent message %d\n", i);
    zcm_msg_free(msg);
    sleep(1);
  }

  zcm_proc_free(proc);
  return 0;
}

static int run_sub_msg(const char *target, const char *self_name, int count) {
  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_SUB, 0, &proc, NULL) != 0) return 1;

  char ep[256] = {0};
  if (lookup_endpoint(proc, target, ep, sizeof(ep)) != 0) {
    zcm_proc_free(proc);
    return 1;
  }

  zcm_socket_t *sub = zcm_socket_new(zcm_proc_context(proc), ZCM_SOCK_SUB);
  if (!sub) {
    zcm_proc_free(proc);
    return 1;
  }
  if (zcm_socket_connect(sub, ep) != 0 ||
      zcm_socket_set_subscribe(sub, "", 0) != 0) {
    fprintf(stderr, "connect/subscribe failed\n");
    zcm_socket_free(sub);
    zcm_proc_free(proc);
    return 1;
  }

  for (int i = 0; count < 0 || i < count; i++) {
    zcm_msg_t *msg = zcm_msg_new();
    if (!msg) {
      zcm_socket_free(sub);
      zcm_proc_free(proc);
      return 1;
    }
    if (zcm_socket_recv_msg(sub, msg) == 0) {
      zcm_core_value_t core;
      zcm_msg_rewind(msg);
      if (zcm_msg_get_core(msg, &core) == 0) {
        if (core.kind == ZCM_CORE_VALUE_TEXT) {
          printf("type=%s core.text=%.*s\n",
                 zcm_msg_get_type(msg), (int)core.text_len, core.text);
        } else if (core.kind == ZCM_CORE_VALUE_DOUBLE) {
          printf("type=%s core.double=%f\n", zcm_msg_get_type(msg), core.d);
        } else if (core.kind == ZCM_CORE_VALUE_FLOAT) {
          printf("type=%s core.float=%f\n", zcm_msg_get_type(msg), core.f);
        } else if (core.kind == ZCM_CORE_VALUE_INT) {
          printf("type=%s core.int=%d\n", zcm_msg_get_type(msg), core.i);
        } else {
          printf("message decode error: unsupported core kind\n");
        }
      } else {
        zcm_msg_rewind(msg);
        int32_t v = 0;
        const char *text = NULL;
        uint32_t text_len = 0;
        if (zcm_msg_get_int(msg, &v) == 0 &&
            zcm_msg_get_text(msg, &text, &text_len) == 0) {
          printf("type=%s v=%d text=%.*s\n",
                 zcm_msg_get_type(msg), v, (int)text_len, text);
        } else {
          printf("message decode error: %s\n", zcm_msg_last_error(msg));
        }
      }
    }
    zcm_msg_free(msg);
  }

  zcm_socket_free(sub);
  zcm_proc_free(proc);
  return 0;
}

static int run_pub_bytes(const char *name, int count, const char *payload) {
  zcm_proc_t *proc = NULL;
  zcm_socket_t *pub = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_PUB, 1, &proc, &pub) != 0) return 1;

  for (int i = 0; count < 0 || i < count; i++) {
    if (zcm_socket_send_bytes(pub, payload, strlen(payload)) != 0) {
      fprintf(stderr, "send failed\n");
      zcm_proc_free(proc);
      return 1;
    }
    printf("sent bytes %d\n", i);
    sleep(1);
  }

  zcm_proc_free(proc);
  return 0;
}

static int run_sub_bytes(const char *target, const char *self_name, int count) {
  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_SUB, 0, &proc, NULL) != 0) return 1;

  char ep[256] = {0};
  if (lookup_endpoint(proc, target, ep, sizeof(ep)) != 0) {
    zcm_proc_free(proc);
    return 1;
  }

  zcm_socket_t *sub = zcm_socket_new(zcm_proc_context(proc), ZCM_SOCK_SUB);
  if (!sub) {
    zcm_proc_free(proc);
    return 1;
  }
  if (zcm_socket_connect(sub, ep) != 0 ||
      zcm_socket_set_subscribe(sub, "", 0) != 0) {
    fprintf(stderr, "connect/subscribe failed\n");
    zcm_socket_free(sub);
    zcm_proc_free(proc);
    return 1;
  }

  for (int i = 0; count < 0 || i < count; i++) {
    char buf[256] = {0};
    size_t n = 0;
    if (zcm_socket_recv_bytes(sub, buf, sizeof(buf) - 1, &n) == 0) {
      buf[n] = '\0';
      printf("received bytes: %s\n", buf);
    }
  }

  zcm_socket_free(sub);
  zcm_proc_free(proc);
  return 0;
}

static int run_daemon(const char *name) {
  zcm_proc_t *proc = NULL;
  zcm_socket_t *rep = NULL;
  if (zcm_proc_init(name, ZCM_SOCK_REP, 1, &proc, &rep) != 0) return 1;
  printf("zcm_proc daemon started: %s\n", name);
  printf("default request/reply: PING -> PONG\n");

  for (;;) {
    zcm_msg_t *req = zcm_msg_new();
    if (!req) {
      zcm_proc_free(proc);
      return 1;
    }
    const char *cmd = NULL;
    uint32_t cmd_len = 0;
    int32_t req_code = 200;
    int core_request = 0;
    if (zcm_socket_recv_msg(rep, req) == 0) {
      zcm_core_value_t core;
      zcm_msg_rewind(req);
      if (zcm_msg_get_core(req, &core) == 0) {
        core_request = 1;
        if (core.kind == ZCM_CORE_VALUE_TEXT) {
          cmd = core.text;
          cmd_len = core.text_len;
          printf("received query: type=%s core.text=%.*s\n",
                 zcm_msg_get_type(req), (int)cmd_len, cmd);
        } else if (core.kind == ZCM_CORE_VALUE_DOUBLE) {
          printf("received query: type=%s core.double=%f\n", zcm_msg_get_type(req), core.d);
        } else if (core.kind == ZCM_CORE_VALUE_FLOAT) {
          printf("received query: type=%s core.float=%f\n", zcm_msg_get_type(req), core.f);
        } else if (core.kind == ZCM_CORE_VALUE_INT) {
          printf("received query: type=%s core.int=%d\n", zcm_msg_get_type(req), core.i);
        }
      } else {
        zcm_msg_rewind(req);
        if (zcm_msg_get_text(req, &cmd, &cmd_len) != 0) {
          cmd = NULL;
          cmd_len = 0;
        }
        if (zcm_msg_get_int(req, &req_code) != 0) {
          req_code = 200;
        }
        printf("received query: type=%s cmd=%s\n",
               zcm_msg_get_type(req), cmd ? cmd : "<none>");
      }
    }
    zcm_msg_free(req);

    zcm_msg_t *reply = zcm_msg_new();
    if (!reply) {
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_set_type(reply, "REPLY");
    const char *reply_text = "ERR";
    if (cmd && text_equals_nocase(cmd, cmd_len, "PING")) {
      reply_text = "PONG";
    } else if (core_request) {
      reply_text = "OK";
    } else if (cmd && cmd_len > 0) {
      reply_text = "OK";
    }

    if (core_request) zcm_msg_put_core_text(reply, reply_text);
    else zcm_msg_put_text(reply, reply_text);
    zcm_msg_put_int(reply, req_code);
    if (zcm_socket_send_msg(rep, reply) != 0) {
      fprintf(stderr, "reply send failed\n");
      zcm_msg_free(reply);
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_free(reply);
  }

  zcm_proc_free(proc);
  return 0;
}

static int run_req(const char *service, const char *self_name, int count, const char *request) {
  zcm_proc_t *proc = NULL;
  if (zcm_proc_init(self_name, ZCM_SOCK_REQ, 0, &proc, NULL) != 0) return 1;

  char ep[256] = {0};
  if (lookup_endpoint(proc, service, ep, sizeof(ep)) != 0) {
    zcm_proc_free(proc);
    return 1;
  }

  zcm_socket_t *req = zcm_socket_new(zcm_proc_context(proc), ZCM_SOCK_REQ);
  if (!req) {
    zcm_proc_free(proc);
    return 1;
  }
  if (zcm_socket_connect(req, ep) != 0) {
    fprintf(stderr, "connect failed\n");
    zcm_socket_free(req);
    zcm_proc_free(proc);
    return 1;
  }

  for (int i = 0; i < count; i++) {
    zcm_msg_t *msg = zcm_msg_new();
    if (!msg) {
      zcm_socket_free(req);
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_set_type(msg, "QUERY");
    zcm_msg_put_text(msg, request);
    zcm_msg_put_int(msg, 42 + i);

    if (zcm_socket_send_msg(req, msg) != 0) {
      fprintf(stderr, "send failed\n");
      zcm_msg_free(msg);
      zcm_socket_free(req);
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_free(msg);

    zcm_msg_t *reply = zcm_msg_new();
    if (!reply) {
      zcm_socket_free(req);
      zcm_proc_free(proc);
      return 1;
    }
    if (zcm_socket_recv_msg(req, reply) == 0) {
      const char *text = NULL;
      int32_t code = 0;
      if (zcm_msg_get_text(reply, &text, NULL) == 0 &&
          zcm_msg_get_int(reply, &code) == 0) {
        printf("received reply: type=%s text=%s code=%d\n",
               zcm_msg_get_type(reply), text, code);
      } else {
        printf("reply decode error: %s\n", zcm_msg_last_error(reply));
      }
    }
    zcm_msg_free(reply);
    usleep(200 * 1000);
  }

  zcm_socket_free(req);
  zcm_proc_free(proc);
  return 0;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "usage:\n"
          "  %s daemon    [name]\n"
          "  %s pub-msg   [name] [count|-1]\n"
          "  %s sub-msg   [target] [self_name] [count|-1]\n"
          "  %s pub-bytes [name] [count|-1] [payload]\n"
          "  %s sub-bytes [target] [self_name] [count|-1]\n"
          "  %s req       [service] [self_name] [count] [request]\n"
          "  %s rep       [name] (alias for daemon)\n",
          prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    return run_daemon("zcmproc");
  }

  if (strcmp(argv[1], "daemon") == 0 || strcmp(argv[1], "rep") == 0) {
    const char *name = (argc > 2) ? argv[2] : "zcmproc";
    return run_daemon(name);
  }

  if (strcmp(argv[1], "pub-msg") == 0) {
    const char *name = (argc > 2) ? argv[2] : "procpub";
    int count = parse_int((argc > 3) ? argv[3] : NULL, -1);
    return run_pub_msg(name, count);
  }

  if (strcmp(argv[1], "sub-msg") == 0) {
    const char *target = (argc > 2) ? argv[2] : "procpub";
    const char *self_name = (argc > 3) ? argv[3] : "procsub";
    int count = parse_int((argc > 4) ? argv[4] : NULL, -1);
    return run_sub_msg(target, self_name, count);
  }

  if (strcmp(argv[1], "pub-bytes") == 0) {
    const char *name = (argc > 2) ? argv[2] : "procbytes";
    int count = parse_int((argc > 3) ? argv[3] : NULL, -1);
    const char *payload = (argc > 4) ? argv[4] : "raw-bytes-proc";
    return run_pub_bytes(name, count, payload);
  }

  if (strcmp(argv[1], "sub-bytes") == 0) {
    const char *target = (argc > 2) ? argv[2] : "procbytes";
    const char *self_name = (argc > 3) ? argv[3] : "procbytesub";
    int count = parse_int((argc > 4) ? argv[4] : NULL, -1);
    return run_sub_bytes(target, self_name, count);
  }

  if (strcmp(argv[1], "req") == 0) {
    const char *service = (argc > 2) ? argv[2] : "zcmproc";
    const char *self_name = (argc > 3) ? argv[3] : "echoclient";
    int count = parse_int((argc > 4) ? argv[4] : NULL, 1);
    const char *request = (argc > 5) ? argv[5] : "PING";
    if (count < 0) count = 1;
    return run_req(service, self_name, count, request);
  }

  usage(argv[0]);
  return 1;
}
