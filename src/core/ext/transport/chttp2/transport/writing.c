/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/ext/transport/chttp2/transport/internal.h"

#include <limits.h>

#include <grpc/support/log.h>

#include "src/core/ext/transport/chttp2/transport/http2_errors.h"
#include "src/core/lib/profiling/timers.h"

static void add_to_write_list(grpc_chttp2_write_cb **list,
                              grpc_chttp2_write_cb *cb) {
  cb->next = *list;
  *list = cb;
}

static void finish_write_cb(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t,
                            grpc_chttp2_stream *s, grpc_chttp2_write_cb *cb,
                            grpc_error *error) {
  grpc_chttp2_complete_closure_step(exec_ctx, t, s, &cb->closure, error,
                                    "finish_write_cb");
  cb->next = t->write_cb_pool;
  t->write_cb_pool = cb;
}

static void update_list(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t,
                        grpc_chttp2_stream *s, int64_t send_bytes,
                        grpc_chttp2_write_cb **list, grpc_error *error) {
  grpc_chttp2_write_cb *cb = *list;
  *list = NULL;
  s->flow_controlled_bytes_written += send_bytes;
  while (cb) {
    grpc_chttp2_write_cb *next = cb->next;
    if (cb->call_at_byte <= s->flow_controlled_bytes_written) {
      finish_write_cb(exec_ctx, t, s, cb, GRPC_ERROR_REF(error));
    } else {
      add_to_write_list(list, cb);
    }
    cb = next;
  }
  GRPC_ERROR_UNREF(error);
}

bool grpc_chttp2_begin_write(grpc_exec_ctx *exec_ctx,
                             grpc_chttp2_transport *t) {
  grpc_chttp2_stream *s;

  GPR_TIMER_BEGIN("grpc_chttp2_begin_write", 0);

  if (t->dirtied_local_settings && !t->sent_local_settings) {
    grpc_slice_buffer_add(
        &t->outbuf,
        grpc_chttp2_settings_create(
            t->settings[GRPC_SENT_SETTINGS], t->settings[GRPC_LOCAL_SETTINGS],
            t->force_send_settings, GRPC_CHTTP2_NUM_SETTINGS));
    t->force_send_settings = 0;
    t->dirtied_local_settings = 0;
    t->sent_local_settings = 1;
  }

  /* simple writes are queued to qbuf, and flushed here */
  grpc_slice_buffer_move_into(&t->qbuf, &t->outbuf);
  GPR_ASSERT(t->qbuf.count == 0);

  grpc_chttp2_hpack_compressor_set_max_table_size(
      &t->hpack_compressor,
      t->settings[GRPC_PEER_SETTINGS][GRPC_CHTTP2_SETTINGS_HEADER_TABLE_SIZE]);

  if (t->outgoing_window > 0) {
    while (grpc_chttp2_list_pop_stalled_by_transport(t, &s)) {
      grpc_chttp2_become_writable(exec_ctx, t, s, false,
                                  "transport.read_flow_control");
    }
  }

  /* for each grpc_chttp2_stream that's become writable, frame it's data
     (according to available window sizes) and add to the output buffer */
  while (grpc_chttp2_list_pop_writable_stream(t, &s)) {
    bool sent_initial_metadata = s->sent_initial_metadata;
    bool now_writing = false;

    GRPC_CHTTP2_IF_TRACING(gpr_log(
        GPR_DEBUG, "W:%p %s[%d] im-(sent,send)=(%d,%d) announce=%d", t,
        t->is_client ? "CLIENT" : "SERVER", s->id, sent_initial_metadata,
        s->send_initial_metadata != NULL, s->announce_window));

    /* send initial metadata if it's available */
    if (!sent_initial_metadata && s->send_initial_metadata) {
      grpc_chttp2_encode_header(
          &t->hpack_compressor, s->id, s->send_initial_metadata, 0,
          t->settings[GRPC_ACKED_SETTINGS][GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
          &s->stats.outgoing, &t->outbuf);
      s->send_initial_metadata = NULL;
      s->sent_initial_metadata = true;
      sent_initial_metadata = true;
      now_writing = true;
    }
    /* send any window updates */
    if (s->announce_window > 0) {
      uint32_t announce = s->announce_window;
      grpc_slice_buffer_add(&t->outbuf,
                            grpc_chttp2_window_update_create(
                                s->id, s->announce_window, &s->stats.outgoing));
      GRPC_CHTTP2_FLOW_DEBIT_STREAM("write", t, s, announce_window, announce);
    }
    if (sent_initial_metadata) {
      /* send any body bytes, if allowed by flow control */
      if (s->flow_controlled_buffer.length > 0) {
        uint32_t max_outgoing =
            (uint32_t)GPR_MIN(t->settings[GRPC_ACKED_SETTINGS]
                                         [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
                              GPR_MIN(s->outgoing_window, t->outgoing_window));
        if (max_outgoing > 0) {
          uint32_t send_bytes =
              (uint32_t)GPR_MIN(max_outgoing, s->flow_controlled_buffer.length);
          bool is_last_data_frame =
              s->fetching_send_message == NULL &&
              send_bytes == s->flow_controlled_buffer.length;
          bool is_last_frame =
              is_last_data_frame && s->send_trailing_metadata != NULL &&
              grpc_metadata_batch_is_empty(s->send_trailing_metadata);
          grpc_chttp2_encode_data(s->id, &s->flow_controlled_buffer, send_bytes,
                                  is_last_frame, &s->stats.outgoing,
                                  &t->outbuf);
          GRPC_CHTTP2_FLOW_DEBIT_STREAM("write", t, s, outgoing_window,
                                        send_bytes);
          GRPC_CHTTP2_FLOW_DEBIT_TRANSPORT("write", t, outgoing_window,
                                           send_bytes);
          if (is_last_frame) {
            s->send_trailing_metadata = NULL;
            s->sent_trailing_metadata = true;
            if (!t->is_client && !s->read_closed) {
              grpc_slice_buffer_add(&t->outbuf, grpc_chttp2_rst_stream_create(
                                                    s->id, GRPC_CHTTP2_NO_ERROR,
                                                    &s->stats.outgoing));
            }
          }
          s->sending_bytes += send_bytes;
          now_writing = true;
          if (s->flow_controlled_buffer.length > 0) {
            GRPC_CHTTP2_STREAM_REF(s, "chttp2_writing:fork");
            grpc_chttp2_list_add_writable_stream(t, s);
          }
        } else if (t->outgoing_window == 0) {
          grpc_chttp2_list_add_stalled_by_transport(t, s);
          now_writing = true;
        }
      }
      if (s->send_trailing_metadata != NULL &&
          s->fetching_send_message == NULL &&
          s->flow_controlled_buffer.length == 0) {
        if (grpc_metadata_batch_is_empty(s->send_trailing_metadata)) {
          grpc_chttp2_encode_data(s->id, &s->flow_controlled_buffer, 0, true,
                                  &s->stats.outgoing, &t->outbuf);
        } else {
          grpc_chttp2_encode_header(
              &t->hpack_compressor, s->id, s->send_trailing_metadata, true,
              t->settings[GRPC_ACKED_SETTINGS]
                         [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
              &s->stats.outgoing, &t->outbuf);
        }
        s->send_trailing_metadata = NULL;
        s->sent_trailing_metadata = true;
        if (!t->is_client && !s->read_closed) {
          grpc_slice_buffer_add(
              &t->outbuf, grpc_chttp2_rst_stream_create(
                              s->id, GRPC_CHTTP2_NO_ERROR, &s->stats.outgoing));
        }
        now_writing = true;
      }
    }

    if (now_writing) {
      if (!grpc_chttp2_list_add_writing_stream(t, s)) {
        /* already in writing list: drop ref */
        GRPC_CHTTP2_STREAM_UNREF(exec_ctx, s, "chttp2_writing:already_writing");
      }
    } else {
      grpc_chttp2_leave_writing_lists(exec_ctx, t, s);
      GRPC_CHTTP2_STREAM_UNREF(exec_ctx, s, "chttp2_writing:no_write");
    }
  }

  /* if the grpc_chttp2_transport is ready to send a window update, do so here
     also; 3/4 is a magic number that will likely get tuned soon */
  if (t->announce_incoming_window > 0) {
    uint32_t announced =
        (uint32_t)GPR_MIN(t->announce_incoming_window, UINT32_MAX);
    GRPC_CHTTP2_FLOW_DEBIT_TRANSPORT("write", t, announce_incoming_window,
                                     announced);
    grpc_transport_one_way_stats throwaway_stats;
    grpc_slice_buffer_add(&t->outbuf, grpc_chttp2_window_update_create(
                                          0, announced, &throwaway_stats));
  }

  GPR_TIMER_END("grpc_chttp2_begin_write", 0);

  return t->outbuf.count > 0;
}

void grpc_chttp2_end_write(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t,
                           grpc_error *error) {
  GPR_TIMER_BEGIN("grpc_chttp2_end_write", 0);
  grpc_chttp2_stream *s;

  while (grpc_chttp2_list_pop_writing_stream(t, &s)) {
    if (s->sent_initial_metadata) {
      grpc_chttp2_complete_closure_step(
          exec_ctx, t, s, &s->send_initial_metadata_finished,
          GRPC_ERROR_REF(error), "send_initial_metadata_finished");
    }
    if (s->sending_bytes != 0) {
      update_list(exec_ctx, t, s, (int64_t)s->sending_bytes,
                  &s->on_write_finished_cbs, GRPC_ERROR_REF(error));
      s->sending_bytes = 0;
    }
    if (s->sent_trailing_metadata) {
      grpc_chttp2_complete_closure_step(
          exec_ctx, t, s, &s->send_trailing_metadata_finished,
          GRPC_ERROR_REF(error), "send_trailing_metadata_finished");
      grpc_chttp2_mark_stream_closed(exec_ctx, t, s, !t->is_client, 1,
                                     GRPC_ERROR_REF(error));
    }
    grpc_chttp2_leave_writing_lists(exec_ctx, t, s);
    GRPC_CHTTP2_STREAM_UNREF(exec_ctx, s, "chttp2_writing:end");
  }
  grpc_slice_buffer_reset_and_unref(&t->outbuf);
  GRPC_ERROR_UNREF(error);
  GPR_TIMER_END("grpc_chttp2_end_write", 0);
}
