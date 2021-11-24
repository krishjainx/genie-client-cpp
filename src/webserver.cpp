// -*- mode: cpp; indent-tabs-mode: nil; c-basic-offset: 2 -*-
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

#include "webserver.hpp"
#include "app.hpp"
#include "string.h"
#include <fcntl.h>

#include "config.h"
#define GETTEXT_PACKAGE "genie-client"
#include <glib/gi18n-lib.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "genie::WebServer"

static const char *html_template_1 = "<!DOCTYPE html>"
                                     "<html>"
                                     "<head>"
                                     "<title>";
static const char *html_template_2 =
    "</title>"
    "<meta charset=\"utf-8\" />"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />"
    "<link "
    "href=\"https://cdn.jsdelivr.net/npm/bootstrap@5.1.3/dist/css/"
    "bootstrap.min.css\" rel=\"stylesheet\" "
    "integrity=\"sha384-"
    "1BmE4kWBq78iYhFldvKuhfTAU6auU8tT94WrHftjDbrCEXSU1oBoqyl2QvZ6jIW3\" "
    "crossorigin=\"anonymous\" />"
    "<link href=\"/css/style.css\" rel=\"stylesheet\" />"
    "</head>"
    "<body>"
    "<div class=\"container\">";
static const char *html_template_3 =
    "</div>"
    "<script "
    "src=\"https://cdn.jsdelivr.net/npm/bootstrap@5.1.3/dist/js/"
    "bootstrap.bundle.min.js\" "
    "integrity=\"sha384-ka7Sk0Gln4gmtz2MlQnikT1wXgYsOg+OMhuP+IlRH9sENBO0LRn5q+"
    "8nbTov4+1p\" crossorigin=\"anonymous\"></script>"
    "<script src=\"/js/shared.js\"></script>"
    "</body>"
    "</html>";

static const char *title_error = N_("Genie - Error");
static const char *title_normal = N_("Genie Configuration");

static const char *reply_403 =
    N_("<h1>Forbidden</h1><p>The requested page is not accessible.</p>");
static const char *reply_404 =
    N_("<h1>Not Found</h1><p>The requested page does not exist.</p><p><a "
       "href=\"/\">Home page</a></p>");
static const char *reply_405 = N_("<h1>Method Not Allowed</h1>");

static gchar *gen_random(size_t size) {
  guchar *buffer = (guchar *)g_malloc(size);
  int fd = open("/dev/urandom", O_RDONLY);
  read(fd, buffer, size);
  close(fd);
  char *base64 = g_base64_encode(buffer, size);
  g_free(buffer);
  return base64;
}

genie::WebServer::WebServer(App *app)
    : app(app),
      server(soup_server_new("server-header",
                             PACKAGE_NAME "/" PACKAGE_VERSION " ", nullptr),
             adopt_mode::owned) {

  gchar *tmp = gen_random(32);
  csrf_token = tmp;
  g_free(tmp);

  GError *error = nullptr;
  soup_server_listen_all(server.get(), app->config->webui_port,
                         (SoupServerListenOptions)0, &error);
  if (error) {
    g_critical("Failed to initialize Web UI HTTP server: %s", error->message);
    g_error_free(error);
    return;
  }

  soup_server_add_handler(
      server.get(), "/favicon.ico",
      [](SoupServer *server, SoupMessage *msg, const char *path,
         GHashTable *query, SoupClientContext *context, gpointer data) {
        WebServer *self = static_cast<WebServer *>(data);
        self->handle_asset(msg, path);
      },
      this, nullptr);
  soup_server_add_handler(
      server.get(), "/css",
      [](SoupServer *server, SoupMessage *msg, const char *path,
         GHashTable *query, SoupClientContext *context, gpointer data) {
        WebServer *self = static_cast<WebServer *>(data);
        self->handle_asset(msg, path);
      },
      this, nullptr);
  soup_server_add_handler(
      server.get(), "/js",
      [](SoupServer *server, SoupMessage *msg, const char *path,
         GHashTable *query, SoupClientContext *context, gpointer data) {
        WebServer *self = static_cast<WebServer *>(data);
        self->handle_asset(msg, path);
      },
      this, nullptr);
  soup_server_add_handler(
      server.get(), "/",
      [](SoupServer *server, SoupMessage *msg, const char *path,
         GHashTable *query, SoupClientContext *context, gpointer data) {
        WebServer *self = static_cast<WebServer *>(data);
        if (strcmp(path, "/") == 0)
          self->handle_index(msg);
        else
          self->handle_404(msg, path);
      },
      this, nullptr);

  g_message("Web UI listening on port %d", app->config->webui_port);
}

void genie::WebServer::handle_asset(SoupMessage *msg, const char *path) {
  if (check_method(msg, path, (int)AllowedMethod::GET) != AllowedMethod::GET)
    return;

  // security check against path traversal attacks
  if (g_str_has_prefix(path, ".") || g_str_has_prefix(path, "/.") ||
      strstr(path, "..")) {
    handle_404(msg, path);
    return;
  }

  char *filename =
      g_build_filename(app->config->asset_dir, "webui", path, nullptr);
  g_debug("Serving asset from %s", filename);
  GError *error = nullptr;
  gchar *contents;
  gsize length;
  if (!g_file_get_contents(filename, &contents, &length, &error)) {
    g_free(filename);

    if (error->code == G_FILE_ERROR_ACCES || error->code == G_FILE_ERROR_PERM) {
      log_request(msg, path, 403);
      send_html(msg, 403, title_error, reply_403);
    } else {
      handle_404(msg, path);
    }
    return;
  }
  g_free(filename);

  const char *content_type = "application/octet-stream";
  if (g_str_has_prefix(path, "/css"))
    content_type = "text/css";
  else if (g_str_has_prefix(path, "/js"))
    content_type = "application/javascript";
  else if (strcmp(path, "/favicon.ico") == 0)
    content_type = "image/png";

  log_request(msg, path, 200);
  soup_message_set_status(msg, 200);
  soup_message_set_response(msg, content_type, SOUP_MEMORY_TAKE, contents,
                            length);
}

void genie::WebServer::handle_404(SoupMessage *msg, const char *path) {
  log_request(msg, path, 404);
  send_html(msg, 404, title_error, reply_404);
}

genie::WebServer::AllowedMethod
genie::WebServer::check_method(SoupMessage *msg, const char *path,
                               int allowed_methods) {
  char *method;
  g_object_get(msg, "method", &method, nullptr);
  if (strcmp(method, "GET") == 0) {
    g_free(method);
    if (allowed_methods & (int)AllowedMethod::GET)
      return AllowedMethod::GET;
    handle_405(msg, path);
    return AllowedMethod::NONE;
  } else if (strcmp(method, "POST") == 0) {
    g_free(method);
    if (allowed_methods & (int)AllowedMethod::POST)
      return AllowedMethod::POST;
    handle_405(msg, path);
    return AllowedMethod::NONE;
  } else {
    g_free(method);
    handle_405(msg, path);
    return AllowedMethod::NONE;
  }
}

void genie::WebServer::handle_405(SoupMessage *msg, const char *path) {
  log_request(msg, path, 405);
  send_html(msg, 405, title_error, reply_405);
}

void genie::WebServer::handle_index(SoupMessage *msg) {
  auto method = check_method(
      msg, "/", (int)AllowedMethod::GET | (int)AllowedMethod::POST);
  if (method == AllowedMethod::GET)
    handle_index_get(msg);
  else if (method == AllowedMethod::POST)
    handle_index_post(msg);
}

void genie::WebServer::handle_index_post(SoupMessage *msg) {
  SoupMessageHeaders *headers;
  GBytes *request_body;
  gsize body_size;
  GHashTable *fields = nullptr;
  bool any_change = false;

  g_object_get(msg, "request-headers", &headers, "request-body-data",
               &request_body, nullptr);
  if (g_strcmp0(soup_message_headers_get_content_type(headers, nullptr),
                "application/x-www-form-urlencoded") != 0) {
    log_request(msg, "/", 406);
    send_html(msg, 406, title_error, "<h1>Not Acceptable</h1>");
    goto out;
  }

  fields = soup_form_decode(
      (const char *)g_bytes_get_data(request_body, &body_size));

  if (g_strcmp0((const char *)g_hash_table_lookup(fields, "_csrf"),
                csrf_token.c_str()) != 0) {
    log_request(msg, "/", 403);
    send_html(msg, 403, title_error, "<h1>Invalid CSRF token</h1>");
    goto out;
  }

  {
    const char *url = (const char *)g_hash_table_lookup(fields, "url");
    if (url && *url && g_strcmp0(url, app->config->genie_url) != 0) {
      app->config->set_genie_url(url);
      any_change = true;
    }
  }
  {
    const char *access_token =
        (const char *)g_hash_table_lookup(fields, "access_token");
    if (access_token && *access_token &&
        g_strcmp0(access_token, app->config->genie_access_token) != 0) {
      app->config->set_genie_access_token(access_token);
      any_change = true;
    }
  }
  {
    const char *conversation_id =
        (const char *)g_hash_table_lookup(fields, "conversation_id");
    if (conversation_id && *conversation_id &&
        g_strcmp0(conversation_id, app->config->conversation_id) != 0) {
      app->config->set_conversation_id(conversation_id);
      any_change = true;
    }
  }

  if (any_change)
    app->config->save();

  handle_index_get(msg);

out:
  if (fields)
    g_hash_table_unref(fields);
  soup_message_headers_free(headers);
  g_bytes_unref(request_body);
}

void genie::WebServer::handle_index_get(SoupMessage *msg) {
  SoupURI *genie_url = soup_uri_new(app->config->genie_url);

  gchar *body = g_strdup_printf(
      "<h1>%s</h1>"
      "<form method=\"POST\" action=\"/\">"
      "<input type=\"hidden\" name=\"_csrf\" value=\"%s\" />"
      "<div class=\"mb-3\">"
      "<label for=\"config-url\" class=\"form-label\">%s</label>"
      "<input type=\"text\" id=\"config-url\" name=\"url\" value=\"%s\" "
      "class=\"form-control\" />"
      "</div>"
      "<div class=\"mb-3\">"
      "<label for=\"config-access-token\" class=\"form-label\">%s</label>"
      "<input type=\"text\" id=\"config-access-token\" name=\"access_token\" "
      "value=\"%s\" class=\"form-control\" />"
      "</div>"
      "<div class=\"mb-3\">"
      "<label for=\"config-conversation-id\" class=\"form-label\">%s</label>"
      "<input type=\"text\" id=\"config-conversation-id\" "
      "name=\"conversation_id\" "
      "value=\"%s\" class=\"form-control\" />"
      "</div>"
      "<button type=\"submit\" class=\"btn btn-primary\">Save</button>",
      _("Genie Configuration"), csrf_token.c_str(), _("URL"),
      app->config->genie_url, _("Access Token"),
      app->config->genie_access_token ? app->config->genie_access_token : "",
      _("Conversation ID"), app->config->conversation_id);

  log_request(msg, "/", 200);
  send_html(msg, 200, title_normal, body);
  g_free(body);
}

void genie::WebServer::send_html(SoupMessage *msg, int status,
                                 const char *page_title,
                                 const char *page_body) {
  gchar *buffer = (gchar *)g_malloc(
      strlen(html_template_1) + strlen(page_title) + strlen(html_template_2) +
      strlen(page_body) + strlen(html_template_3) + 1);

  gchar *write_at = buffer;
  strcpy(write_at, html_template_1);
  write_at += strlen(html_template_1);
  strcpy(write_at, page_title);
  write_at += strlen(page_title);
  strcpy(write_at, html_template_2);
  write_at += strlen(html_template_2);
  strcpy(write_at, page_body);
  write_at += strlen(page_body);
  strcpy(write_at, html_template_3);
  write_at += strlen(html_template_3);

  soup_message_set_status(msg, status);
  soup_message_set_response(msg, "text/html", SOUP_MEMORY_TAKE, buffer,
                            write_at - buffer);
}

void genie::WebServer::log_request(SoupMessage *msg, const char *path,
                                   int status) {
  char *method;
  g_object_get(msg, "method", &method, nullptr);
  g_message("%s %s - %d", method, path, status);
  g_free(method);
}
