#include "zcm/zcm.h"
#include "zcm/zcm_msg.h"
#include "zcm/zcm_proc_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int text_equals_nocase(const char *text, uint32_t len, const char *lit) {
  size_t n = strlen(lit);
  if (len != n) return 0;
  return strncasecmp(text, lit, n) == 0;
}

/*
 * User hook: customize this to process PUB bytes received by SUB sockets.
 */
static void app_on_sub_payload(const char *self_name,
                               const char *source_name,
                               const void *payload,
                               size_t payload_len,
                               void *user) {
  (void)self_name;
  (void)source_name;
  (void)payload;
  (void)payload_len;
  (void)user;
}

/*
 * User hook: customize this to inspect/manipulate each incoming REQ message.
 * If you consume fields via zcm_msg_get_*, the caller rewinds the message after this hook.
 */
static void app_on_req_message(const char *self_name, zcm_msg_t *req, void *user) {
  (void)self_name;
  (void)req;
  (void)user;
}

static int run_daemon(const char *cfg_path) {
  zcm_proc_runtime_cfg_t cfg;
  zcm_proc_t *proc = NULL;
  zcm_socket_t *rep = NULL;
  if (zcm_proc_runtime_bootstrap(cfg_path, &cfg, &proc, &rep) != 0) return 1;

  printf("zcm_proc daemon started: %s\n", cfg.name);
  printf("ping handler: %s -> %s (default=%s)\n",
         cfg.ping_request, cfg.ping_reply, cfg.default_reply);
  if (cfg.type_handler_count > 0) {
    printf("type handlers loaded: %zu\n", cfg.type_handler_count);
  }
  if (cfg.data_socket_count > 0) {
    printf("data sockets configured: %zu\n", cfg.data_socket_count);
  }
  zcm_proc_runtime_start_data_workers(&cfg, proc, app_on_sub_payload, NULL);

  for (;;) {
    zcm_msg_t *req = zcm_msg_new();
    if (!req) {
      zcm_proc_free(proc);
      return 1;
    }
    if (zcm_socket_recv_msg(rep, req) != 0) {
      zcm_msg_free(req);
      continue;
    }
    app_on_req_message(cfg.name, req, NULL);
    zcm_msg_rewind(req);

    int32_t req_code = 200;
    int malformed = 0;
    const char *cmd = NULL;
    uint32_t cmd_len = 0;
    const char *req_type = zcm_msg_get_type(req);
    char err_text[512] = {0};
    char dynamic_reply[64] = {0};
    char parsed_summary[512] = {0};
    const char *reply_text = cfg.default_reply;

    if (!req_type) req_type = "";

    const zcm_proc_type_handler_cfg_t *handler =
        zcm_proc_runtime_find_type_handler(&cfg, req_type);
    if (handler) {
      if (zcm_proc_runtime_decode_type_payload(req, handler,
                                               parsed_summary, sizeof(parsed_summary)) != 0) {
        malformed = 1;
        req_code = 400;
        snprintf(err_text, sizeof(err_text),
                 "ERR malformed %s expected %s", req_type, handler->format);
        reply_text = err_text;
        printf("[REP %s] received malformed request: msgType=%s expected=%s\n",
               cfg.name, req_type, handler->format);
      } else {
        reply_text = handler->reply;
        printf("[REP %s] received request: msgType=%s payload={%s}\n",
               cfg.name, req_type, parsed_summary[0] ? parsed_summary : "<no-args>");
      }
    } else {
      zcm_msg_rewind(req);
      if (zcm_msg_get_text(req, &cmd, &cmd_len) == 0 &&
          zcm_msg_get_int(req, &req_code) == 0 &&
          zcm_msg_remaining(req) == 0) {
        printf("[REP %s] received request: msgType=%s cmd=%.*s code=%d\n",
               cfg.name, req_type, (int)cmd_len, cmd, req_code);
      } else {
        zcm_msg_rewind(req);
        if (zcm_msg_get_text(req, &cmd, &cmd_len) == 0 &&
            zcm_msg_remaining(req) == 0) {
          printf("[REP %s] received request: msgType=%s cmd=%.*s\n",
                 cfg.name, req_type, (int)cmd_len, cmd);
        } else {
          double req_d = 0.0;
          float req_f = 0.0f;
          int32_t req_i = 0;
          cmd = NULL;
          cmd_len = 0;

          zcm_msg_rewind(req);
          if (zcm_msg_get_double(req, &req_d) == 0 && zcm_msg_remaining(req) == 0) {
            printf("[REP %s] received request: msgType=%s double=%f\n",
                   cfg.name, req_type, req_d);
          } else {
            zcm_msg_rewind(req);
            if (zcm_msg_get_float(req, &req_f) == 0 && zcm_msg_remaining(req) == 0) {
              printf("[REP %s] received request: msgType=%s float=%f\n",
                     cfg.name, req_type, req_f);
            } else {
              zcm_msg_rewind(req);
              if (zcm_msg_get_int(req, &req_i) == 0 && zcm_msg_remaining(req) == 0) {
                printf("[REP %s] received request: msgType=%s int=%d\n",
                       cfg.name, req_type, req_i);
              } else {
                malformed = 1;
                req_code = 400;
                snprintf(err_text, sizeof(err_text),
                         "ERR malformed request for type %s", req_type[0] ? req_type : "<none>");
                reply_text = err_text;
                printf("[REP %s] received malformed request: msgType=%s\n",
                       cfg.name, req_type[0] ? req_type : "<none>");
              }
            }
          }
        }
      }

      if (!malformed) {
        if (cmd && text_equals_nocase(cmd, cmd_len, "DATA_PORT")) {
          int pub_port = 0;
          if (zcm_proc_runtime_first_pub_port(&cfg, &pub_port) == 0) {
            snprintf(dynamic_reply, sizeof(dynamic_reply), "%d", pub_port);
            reply_text = dynamic_reply;
          } else {
            malformed = 1;
            req_code = 404;
            snprintf(err_text, sizeof(err_text), "ERR no PUB dataSocket configured");
            reply_text = err_text;
          }
        } else if (cmd && text_equals_nocase(cmd, cmd_len, cfg.ping_request)) {
          reply_text = cfg.ping_reply;
        } else {
          reply_text = cfg.default_reply;
        }
      }
    }

    zcm_msg_t *reply = zcm_msg_new();
    if (!reply) {
      zcm_msg_free(req);
      zcm_proc_free(proc);
      return 1;
    }
    zcm_msg_set_type(reply, malformed ? "ERROR" : "REPLY");
    zcm_msg_put_text(reply, reply_text);
    zcm_msg_put_int(reply, req_code);
    if (zcm_socket_send_msg(rep, reply) != 0) {
      fprintf(stderr, "reply send failed\n");
      zcm_msg_free(reply);
      zcm_msg_free(req);
      zcm_proc_free(proc);
      return 1;
    }
    printf("[REP %s] sent reply: msgType=%s text=%s code=%d\n",
           cfg.name, malformed ? "ERROR" : "REPLY", reply_text, req_code);
    fflush(stdout);
    zcm_msg_free(reply);
    zcm_msg_free(req);
  }

  zcm_proc_free(proc);
  return 0;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "usage:\n"
          "  %s <proc-config.cfg>\n",
          prog);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    usage(argv[0]);
    return 1;
  }

  return run_daemon(argv[1]);
}
