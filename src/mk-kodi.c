/* mcp-kodi — Kodi JSON-RPC client over libsoup. See mk-kodi.h.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 */

#include "config.h"

#include "mk-kodi.h"

#include <string.h>

#include <libsoup/soup.h>

/* Per-call HTTP timeout, in seconds. Kodi is on the LAN and calls are
 * serialised by the AI, so this only guards against a hung box. */
#define MK_KODI_TIMEOUT_SECONDS 15

struct _MkKodi
{
  MkConfig    *config;  /* borrowed; must outlive the client */
  SoupSession *session; /* owned; reused across all instances */
  gint64       next_id; /* JSON-RPC request id counter */
};

G_DEFINE_QUARK (mk-kodi-error-quark, mk_kodi_error)

/**
 * mk_kodi_new:
 * @config: the configuration to resolve instances against; borrowed, not owned.
 *
 * Creates a client with one SoupSession reused for every instance.
 *
 * @return a newly allocated MkKodi; free with mk_kodi_free().
 */
MkKodi *
mk_kodi_new (MkConfig *config)
{
  g_return_val_if_fail (config != NULL, NULL);

  MkKodi *self = g_new0 (MkKodi, 1);
  self->config = config;
  self->session = soup_session_new ();
  soup_session_set_timeout (self->session, MK_KODI_TIMEOUT_SECONDS);
  self->next_id = 0;
  return self;
}

/**
 * mk_kodi_free:
 * @self: the client to free, or NULL.
 *
 * Frees the client and its SoupSession. The borrowed config is left untouched.
 * Safe to call with NULL.
 */
void
mk_kodi_free (MkKodi *self)
{
  if (self == NULL)
    return;
  g_clear_object (&self->session);
  g_free (self);
}

/**
 * accept_certificate_cb:
 * @msg: the message whose TLS peer failed verification.
 * @certificate: the peer certificate (unused).
 * @errors: the verification errors (unused).
 * @user_data: unused.
 *
 * Accepts an otherwise-untrusted certificate. Connected only to messages bound
 * for an instance configured `insecure`, so acceptance is scoped to that
 * instance's host rather than applied session-wide.
 *
 * @return TRUE, always, to accept the certificate.
 */
static gboolean
accept_certificate_cb (SoupMessage          *msg,
                       GTlsCertificate      *certificate,
                       GTlsCertificateFlags  errors,
                       gpointer              user_data)
{
  (void) msg;
  (void) certificate;
  (void) errors;
  (void) user_data;
  return TRUE;
}

/**
 * build_request:
 * @id: the JSON-RPC request id.
 * @method: the JSON-RPC method name.
 * @params: the params value, or NULL to omit the member; borrowed (copied in).
 *
 * Serialises a JSON-RPC 2.0 request object.
 *
 * @return a newly allocated JSON string; free with g_free().
 */
static char *
build_request (gint64 id, const char *method, JsonNode *params)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "jsonrpc");
  json_builder_add_string_value (b, "2.0");
  json_builder_set_member_name (b, "id");
  json_builder_add_int_value (b, id);
  json_builder_set_member_name (b, "method");
  json_builder_add_string_value (b, method);
  if (params != NULL)
    {
      json_builder_set_member_name (b, "params");
      json_builder_add_value (b, json_node_copy (params)); /* takes ownership */
    }
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  return json_generator_to_data (gen, NULL);
}

/**
 * parse_response:
 * @data: the raw HTTP response body.
 * @size: length of @data in bytes.
 * @error: return location for a GError, or NULL.
 *
 * Parses a JSON-RPC reply: surfaces an "error" member as MK_KODI_ERROR_RPC
 * (with Kodi's code and message), and on success returns a copy of the "result"
 * member. A reply with neither member is a protocol error.
 *
 * @return a newly allocated "result" JsonNode (free with json_node_unref()), or
 *         NULL with @error set.
 */
static JsonNode *
parse_response (const char *data, gsize size, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  GError *local = NULL;

  if (!json_parser_load_from_data (parser, data, size, &local))
    {
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_PROTOCOL,
                   "invalid JSON in Kodi response: %s", local->message);
      g_clear_error (&local);
      return NULL;
    }

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_PROTOCOL,
                   "Kodi response is not a JSON object");
      return NULL;
    }

  JsonObject *obj = json_node_get_object (root);

  if (json_object_has_member (obj, "error"))
    {
      JsonObject *err = json_object_get_object_member (obj, "error");
      gint64 code = 0;
      const char *message = "(no message)";
      if (err != NULL)
        {
          code = json_object_get_int_member_with_default (err, "code", 0);
          message = json_object_get_string_member_with_default (err, "message",
                                                                "(no message)");
        }
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_RPC,
                   "Kodi error %" G_GINT64_FORMAT ": %s", code, message);
      return NULL;
    }

  if (!json_object_has_member (obj, "result"))
    {
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_PROTOCOL,
                   "Kodi response has neither \"result\" nor \"error\"");
      return NULL;
    }

  return json_node_copy (json_object_get_member (obj, "result"));
}

/**
 * mk_kodi_call:
 * @self: the client.
 * @instance: instance name to target, or NULL for the config default.
 * @method: the JSON-RPC method (e.g. "JSONRPC.Ping").
 * @params: the params value, or NULL; borrowed (not freed by this call).
 * @error: return location for a GError, or NULL.
 *
 * Resolves @instance to its scheme/host/auth/insecure, POSTs a JSON-RPC 2.0
 * request to <scheme>://<host>/jsonrpc with `application/json` and (when
 * configured) preemptive HTTP Basic auth, then parses the reply. For an
 * instance marked `insecure`, the message accepts the self-signed
 * certificate. The send is synchronous.
 *
 * @return the "result" as a newly allocated JsonNode (free with
 *         json_node_unref()), or NULL with @error set.
 */
JsonNode *
mk_kodi_call (MkKodi      *self,
              const char  *instance,
              const char  *method,
              JsonNode    *params,
              GError     **error)
{
  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (method != NULL, NULL);

  MkInstance *inst = mk_config_get_instance (self->config, instance);
  if (inst == NULL)
    {
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_NO_INSTANCE,
                   "no such Kodi instance: %s",
                   instance ? instance : "(default)");
      return NULL;
    }
  if (inst->host == NULL || inst->host[0] == '\0')
    {
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_NO_INSTANCE,
                   "instance \"%s\" has no host configured",
                   instance ? instance : "(default)");
      return NULL;
    }

  const char *scheme = inst->scheme ? inst->scheme : "https";
  g_autofree char *url = g_strdup_printf ("%s://%s/jsonrpc", scheme, inst->host);

  g_autoptr (SoupMessage) msg = soup_message_new (SOUP_METHOD_POST, url);
  if (msg == NULL)
    {
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_TRANSPORT,
                   "invalid Kodi URL: %s", url);
      return NULL;
    }

  /* Preemptive HTTP Basic auth from the instance's "user:pass" string. */
  if (inst->auth != NULL && inst->auth[0] != '\0')
    {
      g_autofree char *b64 =
        g_base64_encode ((const guchar *) inst->auth, strlen (inst->auth));
      g_autofree char *value = g_strconcat ("Basic ", b64, NULL);
      soup_message_headers_replace (soup_message_get_request_headers (msg),
                                    "Authorization", value);
    }

  /* Accept the self-signed cert for this message only when so configured. */
  if (inst->insecure)
    g_signal_connect (msg, "accept-certificate",
                      G_CALLBACK (accept_certificate_cb), NULL);

  g_autofree char *body = build_request (++self->next_id, method, params);
  g_autoptr (GBytes) request = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", request);

  GError *local = NULL;
  g_autoptr (GBytes) response =
    soup_session_send_and_read (self->session, msg, NULL, &local);
  if (response == NULL)
    {
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_TRANSPORT,
                   "request to %s failed: %s", url, local->message);
      g_clear_error (&local);
      return NULL;
    }

  guint status = soup_message_get_status (msg);
  if (status < 200 || status >= 300)
    {
      const char *reason = soup_message_get_reason_phrase (msg);
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_HTTP,
                   "Kodi returned HTTP %u %s", status,
                   reason ? reason : "");
      return NULL;
    }

  gsize size = 0;
  const char *data = g_bytes_get_data (response, &size);
  return parse_response (data, size, error);
}
