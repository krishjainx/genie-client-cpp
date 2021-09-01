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

#include <stdlib.h>
#include <glib.h>
#include <glib-unix.h>
#include <glib-object.h>
#include <libsoup/soup.h>
#include <string.h>
#include<fstream>
#include "wsclient.hpp"
#include "tts.hpp"

gboolean genie::wsClient::checkIsConnected() {
    if (wconn == NULL) {
        g_printerr("GENIE websocket connection is NULL\n");
        return false;
    }
    
    SoupWebsocketState wconnState = soup_websocket_connection_get_state(wconn);
    
    if (wconnState != SOUP_WEBSOCKET_STATE_OPEN) {
        g_print("WS connection not open (state %d)\n", wconnState);
        return false;
    }
    
    return true;
}

void genie::wsClient::sendJSON(JsonBuilder *builder) {
    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    gchar *str = json_generator_to_data(gen, NULL);
    
    PROF_PRINT("[SERVER WS] sending: %s", str);
    soup_websocket_connection_send_text(wconn, str);
    
    json_node_free(root);
    g_object_unref(gen);
    g_free(str);
}

void genie::wsClient::sendCommand(gchar *data)
{
    if (!checkIsConnected()) {
        return;
    }

    JsonBuilder *builder = json_builder_new();

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "command");

    json_builder_set_member_name(builder, "text");
    json_builder_add_string_value(builder, data);

    json_builder_end_object(builder);

    sendJSON(builder);
    g_object_unref(builder);
    
    gettimeofday(&tStart, NULL);
    app->track_processing_event(PROCESSING_START_GENIE);
    tInit = true;

    return;
}

void genie::wsClient::sendThingtalk(gchar *data)
{
    if (!checkIsConnected()) {
        return;
    }

    JsonBuilder *builder = json_builder_new();

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "tt");

    json_builder_set_member_name(builder, "code");
    json_builder_add_string_value(builder, data);

    json_builder_set_member_name(builder, "id");
    seq++;
    json_builder_add_int_value(builder, seq);

    json_builder_end_object(builder);
    
    sendJSON(builder);
    g_object_unref(builder);

    return;
}

void genie::wsClient::handleConversationID(JsonReader *reader) {
    json_reader_read_member(reader, "id");
    const gchar *text = json_reader_get_string_value(reader);
    json_reader_end_member(reader);
    
    if (conversationId) {
        g_free(conversationId);
    }
    conversationId = g_strdup(text);
    g_print("[SERVER WS] Set conversation id: %s\n", conversationId);
    acceptStream = true;
}

void genie::wsClient::handleText(gint64 id, JsonReader *reader) {
    if (id <= lastSaidTextID) {
        g_message(
            "Skipping message ID=%lld, already said ID=%lld\n",
            id,
            lastSaidTextID
        );
        return;
    }
    
    json_reader_read_member(reader, "text");
    const gchar *text = json_reader_get_string_value(reader);
    json_reader_end_member(reader);
    
    if (tInit) {
        app->track_processing_event(PROCESSING_END_GENIE);
        tInit = false;
    }

    app->m_audioPlayer->say(g_strdup(text));
    
    lastSaidTextID = id;
}

void genie::wsClient::handleSound(gint64 id, JsonReader *reader) {
    json_reader_read_member(reader, "name");
    const gchar *name = json_reader_get_string_value(reader);
    json_reader_end_member(reader);
    
    if (!strncmp(name, "news-intro", 10)) {
        g_message("Playing sound message id=%lld name=%s\n", id, name);
        app->m_audioPlayer->playSound(SOUND_NEWS_INTRO, true);
    } else {
        g_warning("Sound not recognized id=%lld name=%s\n", id, name);
    }
}

void genie::wsClient::handleAudio(gint64 id, JsonReader *reader) {
    json_reader_read_member(reader, "url");
    const gchar *url = json_reader_get_string_value(reader);
    json_reader_end_member(reader);
    
    g_message("Playing audio message id=%lld url=%s\n", id, url);
    app->m_audioPlayer->playLocation((gchar *)url);
}

void genie::wsClient::handleError(gint64 id, JsonReader *reader) {
    json_reader_read_member(reader, "error");
    const gchar *error = json_reader_get_string_value(reader);
    json_reader_end_member(reader);
    
    g_warning("Handling id=%lld type=error error=%s\n", error);
}

void genie::wsClient::handleAskSpecial(gint64 id, JsonReader *reader) {
    // Agent state -- asking a follow up or not
    json_reader_read_member(reader, "ask");
    const gchar *ask = json_reader_get_string_value(reader);
    json_reader_end_member(reader);
    g_debug("TODO Ignoring id=%lld type=askSpecial ask=%s\n", id, ask);
}

void genie::wsClient::handlePing(gint64 id, JsonReader *reader) {
    if (!checkIsConnected()) {
        return;
    }

    JsonBuilder *builder = json_builder_new();

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "pong");

    json_builder_end_object(builder);
    
    sendJSON(builder);
    g_object_unref(builder);
}

void genie::wsClient::on_message(
    SoupWebsocketConnection *conn,
    gint data_type,
    GBytes *message,
    gpointer data
) {
    if (data_type != SOUP_WEBSOCKET_DATA_TEXT) {
        g_warning("Invalid message data type: %d\n", data_type);
        return;
    }
    
    wsClient *obj = reinterpret_cast<wsClient *>(data);
    gsize sz;
    const gchar *ptr;

    ptr = (const gchar *)g_bytes_get_data(message, &sz);
    g_message("Received message: %s\n", ptr);

    JsonParser *parser = json_parser_new();
    json_parser_load_from_data(parser, ptr, -1, NULL);

    JsonReader *reader = json_reader_new(json_parser_get_root(parser));

    json_reader_read_member(reader, "type");
    const char *type = json_reader_get_string_value(reader);
    json_reader_end_member(reader);

    if (strncmp(type, "id", 2) == 0) {
        obj->handleConversationID(reader);
    } else {
        json_reader_read_member(reader, "id");
        gint64 id = json_reader_get_int_value(reader);
        json_reader_end_member(reader);
        
        g_debug("Handling message id=%lld, setting this->seq", id);
        obj->seq = id;

        if (obj->acceptStream) {
            if (strncmp(type, "text", 4) == 0) {
                obj->handleText(id, reader);
            } else if (strncmp(type, "sound", 5) == 0) {
                obj->handleSound(id, reader);
            } else if (strncmp(type, "audio", 5) == 0) {
                obj->handleAudio(id, reader);
            } else if (strncmp(type, "error", 5) == 0) {
                obj->handleError(id, reader);
            } else if (strncmp(type, "askSpecial", 10) == 0) {
                obj->handleAskSpecial(id, reader);
            } else if (strncmp(type, "ping", 4) == 0) {
                obj->handlePing(id, reader);
            } else if (
                strncmp(type, "command", 7) == 0 // Parrot commands back
                || strncmp(type, "new-program", 11) == 0 // ThingTalk stuff
                || strncmp(type, "rdl", 3) == 0 // External link
                || strncmp(type, "link", 4) == 0 // Internal link (skill conf)
                || strncmp(type, "button", 6) == 0 // Clickable command
                || strncmp(type, "video", 5) == 0 
                || strncmp(type, "picture", 7) == 0
                || strncmp(type, "choice", 5) == 0
            ) {
                g_debug("Ignored message id=%lld type=%s", id, type);
            } else {
                g_warning("Unhandled message id=%lld type=%s\n", id, type);
            }
        } else {
            g_warning(
                "Ignored message id=%lld type=%s -- not accepting stream\n",
                id,
                type
            );
        }
    }

    g_object_unref(reader);
    g_object_unref(parser);
}

void genie::wsClient::on_close(SoupWebsocketConnection *conn, gpointer data)
{
    wsClient *obj = reinterpret_cast<wsClient *>(data);
    // soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);

    const char *close_data = soup_websocket_connection_get_close_data(conn);

    gushort code = soup_websocket_connection_get_close_code(conn);
    g_print("Genie WebSocket connection closed: %d %s\n", code, close_data);
    
    obj->connect();
}

void genie::wsClient::on_connection(SoupSession *session, GAsyncResult *res, gpointer data)
{
    wsClient *obj = reinterpret_cast<wsClient *>(data);

    SoupWebsocketConnection *conn;
    GError *error = NULL;

    conn = soup_session_websocket_connect_finish(session, res, &error);
    if (error) {
        g_print("Error: %s\n", error->message);
        g_error_free(error);
        return;
    }

    soup_websocket_connection_set_max_incoming_payload_size(conn, 512000);
    obj->setConnection(conn);
    obj->acceptStream = false;

    g_signal_connect(conn, "message", G_CALLBACK(genie::wsClient::on_message), data);
    g_signal_connect(conn, "closed",  G_CALLBACK(genie::wsClient::on_close),   data);
}

void genie::wsClient::setConnection(SoupWebsocketConnection *conn)
{
    if (!conn) return;
    wconn = conn;
}

genie::wsClient::wsClient(App *appInstance)
{
    app = appInstance;
    conversationId = NULL;
    accessToken = g_strdup(app->m_config->genieAccessToken);
    url = g_strdup(app->m_config->genieURL);

    tInit = false;
    lastSaidTextID = -1;
}

genie::wsClient::~wsClient()
{
}

int genie::wsClient::init()
{
    connect();
    return true;
}

void genie::wsClient::connect()
{
    SoupSession *session;
    SoupMessage *msg;

    session = soup_session_new();
    if (strncmp(url, "wss", 3) == 0) {
        // enable the wss support
        gchar *wss_aliases[] = { (gchar *)"wss", NULL };
        g_object_set(session, SOUP_SESSION_HTTPS_ALIASES, wss_aliases, NULL);
    }

    gchar *convId = (gchar *)"";
    if (app->m_config->conversationId) {
        convId = g_strdup_printf("?id=%s", app->m_config->conversationId);
    }

    gchar *uri = g_strdup_printf("%s%s", url, convId);
    msg = soup_message_new(SOUP_METHOD_GET, uri);
    g_free(uri);

    if (app->m_config->conversationId) {
        g_free(convId);
    }

    if (accessToken) {
        gchar *auth = g_strdup_printf("Bearer %s", accessToken);
        soup_message_headers_append(msg->request_headers, "Authorization", auth);
        g_free(auth);
    }

    soup_session_websocket_connect_async(
        session,
        msg,
        NULL, NULL, NULL,
        (GAsyncReadyCallback)genie::wsClient::on_connection,
        this
    );

    return;
}
