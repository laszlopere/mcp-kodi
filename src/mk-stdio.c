/* mcp-kodi — newline-delimited JSON-RPC transport. See mk-stdio.h and
 * ../TODO.md §3. */

#include "config.h"

#include "mk-stdio.h"

#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

struct _MkStdio
{
  MkStdioDispatch   dispatch;  /* borrowed message handler */
  gpointer          user_data; /* passed to @dispatch */
  GMainLoop        *loop;      /* owned; quit on EOF */
  GDataInputStream *in;        /* owned; line reader over stdin */
  GOutputStream    *out;       /* owned; raw writer over stdout */
};

/* ---- JSON-RPC envelopes --------------------------------------------------- */

/**
 * add_id_member:
 * @b: the builder, positioned to add a member.
 * @id: the request id to echo; borrowed (copied), or NULL.
 *
 * Adds the `"id"` member: a copy of @id, or `null` when @id is NULL.
 */
static void
add_id_member (JsonBuilder *b, JsonNode *id)
{
  json_builder_set_member_name (b, "id");
  if (id != NULL)
    json_builder_add_value (b, json_node_copy (id)); /* takes ownership */
  else
    json_builder_add_null_value (b);
}

/**
 * mk_jsonrpc_result:
 * @id: the request id to echo; borrowed, or NULL for a null id.
 * @result: the result payload; ownership transferred.
 *
 * Builds a JSON-RPC 2.0 success response (§3.3.5).
 *
 * @return a newly allocated response node; free with json_node_unref().
 */
JsonNode *
mk_jsonrpc_result (JsonNode *id, JsonNode *result)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "jsonrpc");
  json_builder_add_string_value (b, "2.0");
  add_id_member (b, id);
  json_builder_set_member_name (b, "result");
  json_builder_add_value (b, result); /* takes ownership */
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/**
 * mk_jsonrpc_error:
 * @id: the request id to echo; borrowed, or NULL for a null id.
 * @code: the JSON-RPC error code (§3.4).
 * @message: the error message; copied.
 *
 * Builds a JSON-RPC 2.0 error response (§3.4).
 *
 * @return a newly allocated response node; free with json_node_unref().
 */
JsonNode *
mk_jsonrpc_error (JsonNode *id, int code, const char *message)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "jsonrpc");
  json_builder_add_string_value (b, "2.0");
  add_id_member (b, id);
  json_builder_set_member_name (b, "error");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "code");
  json_builder_add_int_value (b, code);
  json_builder_set_member_name (b, "message");
  json_builder_add_string_value (b, message ? message : "");
  json_builder_end_object (b);
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/* ---- Writing -------------------------------------------------------------- */

/**
 * mk_stdio_write_message:
 * @self: the transport.
 * @message: the response node to send; borrowed.
 *
 * Serialises @message compactly and writes it as one newline-terminated line on
 * stdout (§3.1). A write failure is logged to stderr; the protocol stream stays
 * line-framed.
 */
static void
mk_stdio_write_message (MkStdio *self, JsonNode *message)
{
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, message);
  gsize len = 0;
  g_autofree char *json = json_generator_to_data (gen, &len);

  /* One object per line, no embedded newlines (the generator emits none in
   * compact mode), then a single framing newline. */
  g_autofree char *line = g_strconcat (json, "\n", NULL);

  GError *error = NULL;
  if (!g_output_stream_write_all (self->out, line, len + 1, NULL, NULL, &error))
    {
      g_printerr ("%s: failed to write response: %s\n", PACKAGE_NAME,
                  error->message);
      g_clear_error (&error);
      return;
    }
  if (!g_output_stream_flush (self->out, NULL, &error))
    {
      g_printerr ("%s: failed to flush response: %s\n", PACKAGE_NAME,
                  error->message);
      g_clear_error (&error);
    }
}

/* ---- Reading -------------------------------------------------------------- */

/**
 * mk_stdio_process_line:
 * @self: the transport.
 * @line: one input line (without its terminating newline); borrowed.
 *
 * Parses @line as a JSON-RPC message and routes it: a JSON syntax error becomes
 * a `-32700` response (id null, §3.4); a parsed message goes to the dispatch
 * callback, whose non-NULL response is written back. Blank lines are ignored.
 */
static void
mk_stdio_process_line (MkStdio *self, const char *line)
{
  /* Ignore blank/whitespace-only lines between messages. */
  if (line[0] == '\0')
    return;

  g_autoptr (JsonParser) parser = json_parser_new ();
  GError *error = NULL;
  if (!json_parser_load_from_data (parser, line, -1, &error))
    {
      g_autofree char *detail =
        g_strdup_printf ("parse error: %s", error->message);
      g_clear_error (&error);
      g_autoptr (JsonNode) resp =
        mk_jsonrpc_error (NULL, MK_JSONRPC_PARSE_ERROR, detail);
      mk_stdio_write_message (self, resp);
      return;
    }

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL)
    return; /* empty document — nothing to dispatch */

  g_autoptr (JsonNode) response = self->dispatch (root, self->user_data);
  if (response != NULL)
    mk_stdio_write_message (self, response);
}

/**
 * read_line_cb:
 * @source: the input stream (unused; @self holds it).
 * @res: the async read result.
 * @user_data: the MkStdio transport.
 *
 * Completes one async line read, processes the line, and queues the next read.
 * NULL line with no error is EOF → quit the loop cleanly (§3.2); a read error
 * is logged and also ends the loop.
 */
static void
read_line_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  MkStdio *self = user_data;

  GError *error = NULL;
  g_autofree char *line =
    g_data_input_stream_read_line_finish (self->in, res, NULL, &error);

  if (line == NULL)
    {
      if (error != NULL)
        {
          g_printerr ("%s: stdin read error: %s\n", PACKAGE_NAME,
                      error->message);
          g_clear_error (&error);
        }
      g_main_loop_quit (self->loop); /* EOF or error → done */
      return;
    }

  mk_stdio_process_line (self, line);

  g_data_input_stream_read_line_async (self->in, G_PRIORITY_DEFAULT, NULL,
                                       read_line_cb, self);
}

/* ---- Public API ----------------------------------------------------------- */

/**
 * mk_stdio_new:
 * @dispatch: callback invoked for each parsed message.
 * @user_data: opaque pointer passed to @dispatch.
 *
 * Creates the transport over fd 0 (stdin) and fd 1 (stdout). Neither fd is
 * closed when the transport is freed.
 *
 * @return a newly allocated MkStdio; free with mk_stdio_free().
 */
MkStdio *
mk_stdio_new (MkStdioDispatch dispatch, gpointer user_data)
{
  g_return_val_if_fail (dispatch != NULL, NULL);

  MkStdio *self = g_new0 (MkStdio, 1);
  self->dispatch = dispatch;
  self->user_data = user_data;
  self->loop = g_main_loop_new (NULL, FALSE);

  GInputStream *raw_in = g_unix_input_stream_new (STDIN_FILENO, FALSE);
  self->in = g_data_input_stream_new (raw_in);
  g_object_unref (raw_in);

  self->out = g_unix_output_stream_new (STDOUT_FILENO, FALSE);
  return self;
}

/**
 * mk_stdio_free:
 * @self: the transport to free, or NULL.
 *
 * Frees the transport and its streams (without closing the underlying fds) and
 * its main loop. Safe to call with NULL.
 */
void
mk_stdio_free (MkStdio *self)
{
  if (self == NULL)
    return;
  g_clear_object (&self->in);
  g_clear_object (&self->out);
  g_clear_pointer (&self->loop, g_main_loop_unref);
  g_free (self);
}

/**
 * mk_stdio_run:
 * @self: the transport.
 *
 * Queues the first async read and runs the main loop until stdin reaches EOF
 * (§3.2), then returns.
 */
void
mk_stdio_run (MkStdio *self)
{
  g_return_if_fail (self != NULL);

  g_data_input_stream_read_line_async (self->in, G_PRIORITY_DEFAULT, NULL,
                                       read_line_cb, self);
  g_main_loop_run (self->loop);
}
