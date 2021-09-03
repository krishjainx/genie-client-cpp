// -*- mode: cpp; indent-tabs-mode: nil; c-basic-offset: 4 -*-
//
// This file is part of Genie
//
// Copyright 2021 The Board of Trustees of the Leland Stanford Junior University
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "stt.hpp"
#include <glib-object.h>
#include <glib-unix.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

genie::STT::STT(App *appInstance) {
  app = appInstance;
  queue = g_queue_new();

  // if (g_str_has_prefix(url, "http")) url = g_str_join("ws", url, NULL);
  char **split = g_strsplit(app->m_config->nlURL, "http", -1);
  url = g_strjoinv("ws", split);
  g_strfreev(split);
}

genie::STT::~STT() { g_queue_free(queue); }

int genie::STT::init() {
  connected = false;
  acceptStream = false;
  wconn = NULL;

  return true;
}

void genie::STT::on_message(SoupWebsocketConnection *conn, gint type,
                            GBytes *message, gpointer data) {
  STT *obj = reinterpret_cast<STT *>(data);
  obj->app->track_processing_event(PROCESSING_END_STT);
  // PROF_TIME_DIFF(
  //     "STT response received (last frame -> on_message)",
  //     obj->tLastFrame
  // );

  if (type == SOUP_WEBSOCKET_DATA_TEXT) {
    gsize sz;
    const gchar *ptr;

    ptr = (const gchar *)g_bytes_get_data(message, &sz);
    g_debug("WS Received data: %s\n", ptr);

    JsonParser *parser = json_parser_new();
    json_parser_load_from_data(parser, ptr, -1, NULL);

    JsonReader *reader = json_reader_new(json_parser_get_root(parser));

    json_reader_read_member(reader, "status");
    int status = json_reader_get_int_value(reader);
    json_reader_end_member(reader);

    if (status == 0) {
      json_reader_read_member(reader, "result");
      const gchar *result = json_reader_get_string_value(reader);
      json_reader_end_member(reader);

      if (!memcmp(result, "ok", 2)) {
        obj->app->m_audioPlayer.get()->clean_queue();

        json_reader_read_member(reader, "text");
        const gchar *text = json_reader_get_string_value(reader);
        json_reader_end_member(reader);

        // PROF_TIME_DIFF("STT full", obj->app->m_audioInput->tStart);
        PROF_PRINT("STT text: %s\n", text);

        gboolean wakewordFound = false;
        if (!strncmp(text, "Computer,", 9) || !strncmp(text, "computer,", 9) ||
            !strncmp(text, "Computer.", 9) || !strncmp(text, "computer.", 9)) {
          text += 9;
          wakewordFound = true;
        } else if (!strncmp(text, "Computer", 8) ||
                   !strncmp(text, "computer", 8)) {
          text += 8;
          wakewordFound = true;
        }

        if (wakewordFound) {
          gchar *dtext = g_strchug(g_strdup(text));
          PROF_PRINT("STT mangled: %s\n", dtext);
          obj->app->m_wsClient.get()->sendCommand(dtext);
          g_free(dtext);
        } else {
          g_print("STT wakeword not found\n");
          obj->app->m_audioPlayer.get()->playSound(SOUND_NO_MATCH);
          obj->app->m_audioPlayer.get()->resume();
        }
      }
    } else {
      g_print("STT status %d\n", status);
      obj->app->m_audioPlayer.get()->playSound(SOUND_NO_MATCH);
      obj->app->m_audioPlayer.get()->resume();
    }

    g_object_unref(reader);
    g_object_unref(parser);
  } else {
    g_print("WS Invalid data type: %d\n", type);
  }

  soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
}

void genie::STT::on_close(SoupWebsocketConnection *conn, gpointer data) {
  STT *obj = reinterpret_cast<STT *>(data);

  gushort code = soup_websocket_connection_get_close_code(conn);
  g_print("STT WebSocket connection closed: %d\n", code);
  // PROF_TIME_DIFF(
  //     "STT total (connect -> on_close)",
  //     obj->tConnect
  // );
  // PROF_TIME_DIFF(
  //     "STT since last frame (last frame -> on_close)",
  //     obj->tLastFrame
  // );
  obj->acceptStream = false;
}

void genie::STT::flush_queue() {
  while (!g_queue_is_empty(queue)) {
    AudioFrame *queued_frame = (AudioFrame *)g_queue_pop_head(queue);
    dispatch_frame(queued_frame);
  }
}

void genie::STT::on_connection(SoupSession *session, GAsyncResult *res,
                               gpointer data) {
  STT *obj = reinterpret_cast<STT *>(data);

  // PROF_TIME_DIFF(
  //     "STT connect time (connect -> on_connection)",
  //     obj->tConnect
  // );

  SoupWebsocketConnection *conn;
  GError *error = NULL;

  conn = soup_session_websocket_connect_finish(session, res, &error);
  if (error) {
    g_print("Error: %s\n", error->message);
    g_error_free(error);
    // g_main_loop_quit(obj->main_loop);
    return;
  }
  obj->setConnection(conn);

  soup_websocket_connection_send_text(conn, "{ \"ver\": 1 }");
  obj->acceptStream = true;

  obj->flush_queue();

  g_signal_connect(conn, "message", G_CALLBACK(genie::STT::on_message), data);
  g_signal_connect(conn, "closed", G_CALLBACK(genie::STT::on_close), data);
}

void genie::STT::setConnection(SoupWebsocketConnection *conn) {
  if (!conn)
    return;
  wconn = conn;
}

int genie::STT::connect() {
  gettimeofday(&tConnect, NULL);
  g_print("SST connecting...\n");

  SoupSession *session;
  SoupMessage *msg;

  session = soup_session_new();
  if (!memcmp(url, "wss", 3)) {
    // enable the wss support
    gchar *wss_aliases[] = {(gchar *)"wss", NULL};
    g_object_set(session, SOUP_SESSION_HTTPS_ALIASES, wss_aliases, NULL);
  }

  gchar *uri = g_strdup_printf("%s/en-US/voice/stream", url);
  msg = soup_message_new(SOUP_METHOD_GET, uri);
  g_free(uri);

  wconn = NULL;

  soup_session_websocket_connect_async(
      session, msg, NULL, NULL, NULL,
      (GAsyncReadyCallback)genie::STT::on_connection, this);

  firstFrame = true;
  return true;
}

gboolean genie::STT::is_connection_open() {
  return (acceptStream && wconn &&
          soup_websocket_connection_get_state(wconn) ==
              SOUP_WEBSOCKET_STATE_OPEN);
}

/**
 * @brief Queue an audio input (speech) frame to be sent to the Speech-To-Text
 * (STT) service.
 *
 * @param frame Audio frame to send.
 */
void genie::STT::send_frame(AudioFrame *frame) {
  if (is_connection_open()) {
    // If we can send frames (connection is open) then send any queued ones
    // followed by the frame we just received.
    flush_queue();

    dispatch_frame(frame);
  } else {
    // The connection is not open yet, queue the frame to be sent when it does
    // open.
    g_queue_push_tail(queue, frame);
  }
}

void genie::STT::send_done() {
  if (is_connection_open()) {
    flush_queue();
    // Send an empty terminator?!?
    soup_websocket_connection_send_binary(wconn, 0, 0);
  } else {
    // queue the empty frame marker to be sent later
    AudioFrame *empty = g_new(AudioFrame, 1);
    empty->length = 0;
    empty->samples = nullptr;
    g_queue_push_tail(queue, empty);
  }
}

void genie::STT::dispatch_frame(AudioFrame *frame) {
  soup_websocket_connection_send_binary(wconn, frame->samples,
                                        frame->length * sizeof(int16_t));
  g_free(frame->samples);
  g_free(frame);
}