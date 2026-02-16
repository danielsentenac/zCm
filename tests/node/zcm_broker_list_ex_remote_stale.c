#include "zcm/zcm.h"
#include "zcm/zcm_node.h"

#include <stdio.h>
#include <string.h>

#include <zmq.h>

static int recv_text_frame(void *sock, char *out, size_t out_size) {
  int n;
  if (!sock || !out || out_size == 0) return -1;
  n = zmq_recv(sock, out, out_size - 1, 0);
  if (n < 0) return -1;
  out[n] = '\0';
  return 0;
}

int main(void) {
  int rc = 1;
  zcm_context_t *ctx = zcm_context_new();
  zcm_broker_t *broker = NULL;
  zcm_node_t *node = NULL;
  void *req = NULL;
  const char *broker_ep = "inproc://zcm-broker-list-ex-remote-stale";

  if (!ctx) {
    fprintf(stderr, "zcm_broker_list_ex_remote_stale: context init failed\n");
    return 1;
  }

  broker = zcm_broker_start(ctx, broker_ep);
  if (!broker) {
    fprintf(stderr, "zcm_broker_list_ex_remote_stale: broker start failed\n");
    goto cleanup;
  }

  node = zcm_node_new(ctx, broker_ep);
  if (!node) {
    fprintf(stderr, "zcm_broker_list_ex_remote_stale: node init failed\n");
    goto cleanup;
  }

  /*
   * Register a remote-host entry with a dead control endpoint.
   * LIST_EX must still reply within socket timeout and not block the broker.
   */
  if (zcm_node_register_ex(node,
                           "stale-remote",
                           "tcp://127.0.0.1:65100",
                           "tcp://127.0.0.1:65101",
                           "remote-host.example",
                           12345) != 0) {
    fprintf(stderr, "zcm_broker_list_ex_remote_stale: register_ex failed\n");
    goto cleanup;
  }

  req = zmq_socket(zcm_context_zmq(ctx), ZMQ_REQ);
  if (!req) {
    fprintf(stderr, "zcm_broker_list_ex_remote_stale: req socket init failed\n");
    goto cleanup;
  }

  {
    int timeout_ms = 800;
    int linger = 0;
    int immediate = 1;
    zmq_setsockopt(req, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
    zmq_setsockopt(req, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
    zmq_setsockopt(req, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(req, ZMQ_IMMEDIATE, &immediate, sizeof(immediate));
  }

  if (zmq_connect(req, broker_ep) != 0) {
    fprintf(stderr, "zcm_broker_list_ex_remote_stale: connect failed\n");
    goto cleanup;
  }
  if (zmq_send(req, "LIST_EX", 7, 0) < 0) {
    fprintf(stderr, "zcm_broker_list_ex_remote_stale: send LIST_EX failed\n");
    goto cleanup;
  }

  {
    char status[16] = {0};
    int count = 0;
    int found_stale = 0;
    int n = 0;

    if (recv_text_frame(req, status, sizeof(status)) != 0) {
      fprintf(stderr, "zcm_broker_list_ex_remote_stale: recv status timeout/failure\n");
      goto cleanup;
    }
    if (strcmp(status, "OK") != 0) {
      fprintf(stderr, "zcm_broker_list_ex_remote_stale: expected OK status, got: %s\n", status);
      goto cleanup;
    }

    n = zmq_recv(req, &count, (int)sizeof(count), 0);
    if (n != (int)sizeof(count) || count <= 0) {
      fprintf(stderr, "zcm_broker_list_ex_remote_stale: invalid count frame\n");
      goto cleanup;
    }

    for (int i = 0; i < count; i++) {
      char name[256] = {0};
      char endpoint[512] = {0};
      char host[256] = {0};
      char role[64] = {0};
      char pub_port[32] = {0};
      char push_port[32] = {0};
      char pub_bytes[32] = {0};
      char sub_bytes[32] = {0};
      char push_bytes[32] = {0};
      char pull_bytes[32] = {0};
      if (recv_text_frame(req, name, sizeof(name)) != 0 ||
          recv_text_frame(req, endpoint, sizeof(endpoint)) != 0 ||
          recv_text_frame(req, host, sizeof(host)) != 0 ||
          recv_text_frame(req, role, sizeof(role)) != 0 ||
          recv_text_frame(req, pub_port, sizeof(pub_port)) != 0 ||
          recv_text_frame(req, push_port, sizeof(push_port)) != 0 ||
          recv_text_frame(req, pub_bytes, sizeof(pub_bytes)) != 0 ||
          recv_text_frame(req, sub_bytes, sizeof(sub_bytes)) != 0 ||
          recv_text_frame(req, push_bytes, sizeof(push_bytes)) != 0 ||
          recv_text_frame(req, pull_bytes, sizeof(pull_bytes)) != 0) {
        fprintf(stderr, "zcm_broker_list_ex_remote_stale: failed while reading LIST_EX row %d\n", i);
        goto cleanup;
      }
      if (strcmp(name, "stale-remote") == 0) found_stale = 1;
    }

    if (!found_stale) {
      fprintf(stderr, "zcm_broker_list_ex_remote_stale: stale-remote entry missing\n");
      goto cleanup;
    }
  }

  rc = 0;
  printf("zcm_broker_list_ex_remote_stale: PASS\n");

cleanup:
  if (req) zmq_close(req);
  if (node) zcm_node_free(node);
  if (broker) zcm_broker_stop(broker);
  if (ctx) zcm_context_free(ctx);
  return rc;
}
