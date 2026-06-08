/* mcp-kodi — tool table: schemas + handlers. See mk-tools.h and ../TODO.md §5. */

#include "config.h"

#include "mk-tools.h"

struct _MkTools
{
  MkConfig *config; /* borrowed; must outlive the table */
  MkKodi   *kodi;   /* borrowed; must outlive the table */
};

G_DEFINE_QUARK (mk-tools-error-quark, mk_tools_error)

/* A handler runs a tool: it reads @args (may be NULL), drives @self->kodi, and
 * returns the result payload as a newly allocated JsonNode, or NULL with @error
 * set on failure. A NULL handler in the table marks a tool not yet implemented
 * (§11.6.1+). */
typedef JsonNode *(*MkToolHandler) (MkTools     *self,
                                    JsonObject  *args,
                                    GError     **error);

/* Builds a tool's `inputSchema` (a JSON Schema object) against @self, so the
 * schema can name the configured instances (§5.1). */
typedef JsonNode *(*MkToolSchema) (MkTools *self);

typedef struct
{
  const char   *name;
  const char   *description;
  MkToolSchema  schema;
  MkToolHandler handler; /* NULL until the tool is implemented */
} MkToolDef;

/* ---- JSON Schema builders -------------------------------------------------
 *
 * Small helpers so each tool's schema reads as a few declarative lines. Every
 * schema is `{ "type": "object", "properties": { ... }, "required": [ ... ] }`.
 */

/**
 * schema_begin:
 * @b: the builder to start a schema object in.
 *
 * Opens `{ "type": "object", "properties": {` ready for property members.
 */
static void
schema_begin (JsonBuilder *b)
{
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "object");
  json_builder_set_member_name (b, "properties");
  json_builder_begin_object (b);
}

/**
 * schema_end:
 * @b: the builder whose schema object to close.
 * @required: NULL-terminated array of required property names, or NULL.
 *
 * Closes the `properties` object, emits a `required` array when @required is
 * non-empty, and closes the schema object.
 */
static void
schema_end (JsonBuilder *b, const char *const *required)
{
  json_builder_end_object (b); /* properties */
  if (required != NULL && required[0] != NULL)
    {
      json_builder_set_member_name (b, "required");
      json_builder_begin_array (b);
      for (gsize i = 0; required[i] != NULL; i++)
        json_builder_add_string_value (b, required[i]);
      json_builder_end_array (b);
    }
  json_builder_end_object (b); /* schema */
}

/**
 * prop_typed:
 * @b: the builder, positioned inside a `properties` object.
 * @name: the property name.
 * @type: the JSON Schema `type` (e.g. "string", "integer").
 * @desc: a human description, or NULL to omit it.
 *
 * Adds `"<name>": { "type": <type>[, "description": <desc>] }`.
 */
static void
prop_typed (JsonBuilder *b, const char *name, const char *type,
            const char *desc)
{
  json_builder_set_member_name (b, name);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, type);
  if (desc != NULL)
    {
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, desc);
    }
  json_builder_end_object (b);
}

/**
 * prop_int_range:
 * @b: the builder, positioned inside a `properties` object.
 * @name: the property name.
 * @desc: a human description, or NULL.
 * @min: inclusive minimum.
 * @max: inclusive maximum.
 *
 * Adds an integer property constrained to [@min, @max].
 */
static void
prop_int_range (JsonBuilder *b, const char *name, const char *desc,
                gint64 min, gint64 max)
{
  json_builder_set_member_name (b, name);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "integer");
  if (desc != NULL)
    {
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, desc);
    }
  json_builder_set_member_name (b, "minimum");
  json_builder_add_int_value (b, min);
  json_builder_set_member_name (b, "maximum");
  json_builder_add_int_value (b, max);
  json_builder_end_object (b);
}

/**
 * prop_enum:
 * @b: the builder, positioned inside a `properties` object.
 * @name: the property name.
 * @desc: a human description, or NULL.
 * @values: NULL-terminated array of permitted string values.
 *
 * Adds a string property restricted to @values via JSON Schema `enum`.
 */
static void
prop_enum (JsonBuilder *b, const char *name, const char *desc,
           const char *const *values)
{
  json_builder_set_member_name (b, name);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "string");
  if (desc != NULL)
    {
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, desc);
    }
  json_builder_set_member_name (b, "enum");
  json_builder_begin_array (b);
  for (gsize i = 0; values[i] != NULL; i++)
    json_builder_add_string_value (b, values[i]);
  json_builder_end_array (b);
  json_builder_end_object (b);
}

/**
 * prop_instance:
 * @b: the builder, positioned inside a `properties` object.
 * @self: the table, used to list configured instance names.
 * @allow_star: whether to document the `"*"` (all instances) wildcard.
 *
 * Adds the optional `instance` property whose description enumerates the
 * configured instances and names the default (§5.1), so the client can target
 * a box by name. With @allow_star, also documents `"*"` for "every instance"
 * (used by `status`, §5.4).
 */
static void
prop_instance (JsonBuilder *b, MkTools *self, gboolean allow_star)
{
  g_autoptr (GString) desc = g_string_new ("Target Kodi instance");

  GList *names = mk_config_instance_names (self->config);
  if (names != NULL)
    {
      g_string_append (desc, " (one of: ");
      for (GList *l = names; l != NULL; l = l->next)
        g_string_append_printf (desc, "%s%s", (const char *) l->data,
                                l->next ? ", " : "");
      g_string_append_c (desc, ')');
    }
  g_list_free (names);

  g_string_append_printf (desc, ". Omitted uses the default (\"%s\").",
                          mk_config_get_default (self->config));
  if (allow_star)
    g_string_append (desc, " Use \"*\" for all instances.");

  prop_typed (b, "instance", "string", desc->str);
}

/* ---- Per-tool schemas -----------------------------------------------------
 *
 * Each returns a freshly built schema node (json_builder_get_root); the table
 * consumer takes ownership.
 */

/**
 * schema_none:
 * @self: the table (unused).
 *
 * Schema for a tool that takes no arguments.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_none (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * schema_instance_only:
 * @self: the table.
 *
 * Schema for tools whose only argument is the optional `instance` (ping, info,
 * players, playpause, stop).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_instance_only (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * schema_notify:
 * @self: the table.
 *
 * Schema for `notify`: `instance?`, `title`, `message`, `displaytime?`.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_notify (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_typed (b, "title", "string", "Notification title.");
  prop_typed (b, "message", "string", "Notification body text.");
  prop_int_range (b, "displaytime", "How long to show it, in milliseconds.",
                  1000, 60000);
  static const char *const req[] = { "title", "message", NULL };
  schema_end (b, req);
  return json_builder_get_root (b);
}

/**
 * schema_play:
 * @self: the table.
 *
 * Schema for `play`: `instance?`, `type` (media kind), `id` (library id).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_play (MkTools *self)
{
  static const char *const types[] = { "song",    "album",   "artist",
                                       "movie",   "episode", "musicvideo",
                                       NULL };
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_enum (b, "type", "Kind of library item to open.", types);
  prop_int_range (b, "id", "Library id of the item to play.", 0, G_MAXINT64);
  static const char *const req[] = { "type", "id", NULL };
  schema_end (b, req);
  return json_builder_get_root (b);
}

/**
 * schema_seek:
 * @self: the table.
 *
 * Schema for `seek`: `instance?` plus exactly one of `time` (h/m/s) or
 * `percentage` on the active player (§5.2). Neither is marked required at the
 * schema level; the handler enforces "exactly one".
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_seek (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_typed (b, "time", "string",
              "Absolute position as HH:MM:SS (or MM:SS). "
              "Provide this or percentage, not both.");
  prop_int_range (b, "percentage",
                  "Absolute position as a percentage (0-100). "
                  "Provide this or time, not both.",
                  0, 100);
  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * schema_handoff:
 * @self: the table.
 *
 * Schema for `handoff`: `from` and `to` instance names (§5.5). Both name
 * configured instances rather than using the shared `instance` arg.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_handoff (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_typed (b, "from", "string",
              "Instance to move playback off (must be playing something).");
  prop_typed (b, "to", "string", "Instance to resume playback on.");
  static const char *const req[] = { "from", "to", NULL };
  schema_end (b, req);
  return json_builder_get_root (b);
}

/**
 * schema_search:
 * @self: the table.
 *
 * Schema for `search`: `type` and `query` (a library lookup). Accepts an
 * optional `instance` for uniformity though any instance shares the library
 * (§5.1).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_search (MkTools *self)
{
  static const char *const types[] = { "tvshow", "movie", "album",
                                       "artist", "song",  NULL };
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_enum (b, "type", "Kind of library item to search for.", types);
  prop_typed (b, "query", "string", "Text to match against titles/names.");
  static const char *const req[] = { "type", "query", NULL };
  schema_end (b, req);
  return json_builder_get_root (b);
}

/**
 * schema_random:
 * @self: the table.
 *
 * Schema for `random`: `instance?`, `type`, optional `query` filter (§5.2).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_random (MkTools *self)
{
  static const char *const types[] = { "episode", "movie", "song", NULL };
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_enum (b, "type", "Kind of item to pick at random.", types);
  prop_typed (b, "query", "string",
              "Optional filter, e.g. a show name for a random episode.");
  static const char *const req[] = { "type", NULL };
  schema_end (b, req);
  return json_builder_get_root (b);
}

/**
 * schema_episodes:
 * @self: the table.
 *
 * Schema for `episodes`: `show`, optional `filter` (SxxExx or text). A library
 * lookup that accepts `instance` for uniformity (§5.1).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_episodes (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_typed (b, "show", "string", "TV show name to list episodes of.");
  prop_typed (b, "filter", "string",
              "Optional episode filter: SxxExx (e.g. S02E05) or free text.");
  static const char *const req[] = { "show", NULL };
  schema_end (b, req);
  return json_builder_get_root (b);
}

/**
 * schema_status:
 * @self: the table.
 *
 * Schema for `status`: `instance?` where `"*"` queries every instance (§5.4).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_status (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, TRUE);
  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * schema_volume:
 * @self: the table.
 *
 * Schema for `volume`: `instance?`, `level` (0-100).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_volume (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_int_range (b, "level", "Volume level, 0-100.", 0, 100);
  static const char *const req[] = { "level", NULL };
  schema_end (b, req);
  return json_builder_get_root (b);
}

/**
 * schema_mute:
 * @self: the table.
 *
 * Schema for `mute`: `instance?`, `state` (on/off/toggle).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_mute (MkTools *self)
{
  static const char *const states[] = { "on", "off", "toggle", NULL };
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_enum (b, "state", "Mute on, off, or toggle the current state.", states);
  static const char *const req[] = { "state", NULL };
  schema_end (b, req);
  return json_builder_get_root (b);
}

/**
 * schema_rpc:
 * @self: the table.
 *
 * Schema for the `rpc` escape hatch: `instance?`, `method`, optional `params`
 * object passed through to Kodi (§5.2).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_rpc (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_typed (b, "method", "string",
              "JSON-RPC method name, e.g. \"Input.Up\".");
  prop_typed (b, "params", "object", "Method parameters, if any.");
  static const char *const req[] = { "method", NULL };
  schema_end (b, req);
  return json_builder_get_root (b);
}

/* ---- The tool table -------------------------------------------------------
 *
 * Names and arguments mirror §5.2. Handlers are NULL until implemented
 * (§11.6.1+); a NULL handler yields a clean "not implemented" result.
 */
static const MkToolDef mk_tool_defs[] = {
  { "instances", "List the configured Kodi instances and which is the default.",
    schema_none, NULL },
  { "ping", "Check connectivity to a Kodi instance (expects a pong).",
    schema_instance_only, NULL },
  { "info", "Get a Kodi instance's name, version, volume, and mute state.",
    schema_instance_only, NULL },
  { "notify", "Show a popup notification on a Kodi screen.",
    schema_notify, NULL },
  { "players", "List the active players on a Kodi instance.",
    schema_instance_only, NULL },
  { "playpause", "Toggle play/pause on the active player.",
    schema_instance_only, NULL },
  { "stop", "Stop the active player.", schema_instance_only, NULL },
  { "play", "Play a library item by type and id.", schema_play, NULL },
  { "seek", "Seek the active player to a time or percentage.",
    schema_seek, NULL },
  { "handoff", "Move playback from one instance to another, resuming position.",
    schema_handoff, NULL },
  { "search", "Search the library for shows, movies, albums, artists, or songs.",
    schema_search, NULL },
  { "random", "Play a random episode, movie, or song, optionally filtered.",
    schema_random, NULL },
  { "episodes", "List a TV show's episodes and their ids.",
    schema_episodes, NULL },
  { "status", "Report what is currently playing on one or all instances.",
    schema_status, NULL },
  { "volume", "Set the volume (0-100) on a Kodi instance.",
    schema_volume, NULL },
  { "mute", "Mute, unmute, or toggle mute on a Kodi instance.",
    schema_mute, NULL },
  { "rpc", "Call any Kodi JSON-RPC method directly (escape hatch).",
    schema_rpc, NULL },
};

/**
 * find_tool:
 * @name: the tool name to look up.
 *
 * Linear search of the table (it is small and fixed).
 *
 * @return the matching MkToolDef, or NULL if @name is unknown.
 */
static const MkToolDef *
find_tool (const char *name)
{
  for (gsize i = 0; i < G_N_ELEMENTS (mk_tool_defs); i++)
    if (g_strcmp0 (mk_tool_defs[i].name, name) == 0)
      return &mk_tool_defs[i];
  return NULL;
}

/* ---- Shared argument resolution -------------------------------------------
 *
 * Helpers handlers (§11.6.1+) use to read tool arguments uniformly. Provided
 * by the scaffold; the unused attribute keeps the build warning-free until the
 * first handler calls them.
 */

/**
 * arg_instance:
 * @args: the tool-call arguments object, or NULL.
 *
 * Reads the optional `instance` argument (§5.1). The returned string is
 * borrowed from @args.
 *
 * @return the instance name, or NULL to mean "the configured default".
 */
G_GNUC_UNUSED static const char *
arg_instance (JsonObject *args)
{
  if (args == NULL || !json_object_has_member (args, "instance"))
    return NULL;
  return json_object_get_string_member_with_default (args, "instance", NULL);
}

/* ---- Result shaping -------------------------------------------------------- */

/**
 * node_to_string:
 * @node: the JSON node to serialise; borrowed.
 *
 * Renders @node as a compact JSON string.
 *
 * @return a newly allocated string; free with g_free().
 */
static char *
node_to_string (JsonNode *node)
{
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, node);
  return json_generator_to_data (gen, NULL);
}

/**
 * error_text:
 * @message: the failure detail.
 *
 * Shapes a tool failure as JSON text `{ "error": "<message>" }` so results stay
 * machine-readable rather than prose (§2.2), with @message correctly escaped.
 *
 * @return a newly allocated JSON string; free with g_free().
 */
static char *
error_text (const char *message)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "error");
  json_builder_add_string_value (b, message);
  json_builder_end_object (b);
  g_autoptr (JsonNode) node = json_builder_get_root (b);
  return node_to_string (node);
}

/**
 * make_result:
 * @text: the JSON text payload for the single text content block; borrowed.
 * @is_error: whether this result reports a tool-level failure.
 *
 * Builds the `tools/call` result envelope
 * `{ "content": [ { "type": "text", "text": <text> } ], "isError": <bool> }`
 * (§3.3.5).
 *
 * @return a newly allocated result node; free with json_node_unref().
 */
static JsonNode *
make_result (const char *text, gboolean is_error)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "content");
  json_builder_begin_array (b);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "text");
  json_builder_set_member_name (b, "text");
  json_builder_add_string_value (b, text);
  json_builder_end_object (b);
  json_builder_end_array (b);

  json_builder_set_member_name (b, "isError");
  json_builder_add_boolean_value (b, is_error);

  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/* ---- Public API ----------------------------------------------------------- */

/**
 * mk_tools_new:
 * @config: configuration for instance names/defaults; borrowed, not owned.
 * @kodi: the Kodi client handlers drive; borrowed, not owned.
 *
 * Creates the tool table. Both @config and @kodi must outlive it.
 *
 * @return a newly allocated MkTools; free with mk_tools_free().
 */
MkTools *
mk_tools_new (MkConfig *config, MkKodi *kodi)
{
  g_return_val_if_fail (config != NULL, NULL);
  g_return_val_if_fail (kodi != NULL, NULL);

  MkTools *self = g_new0 (MkTools, 1);
  self->config = config;
  self->kodi = kodi;
  return self;
}

/**
 * mk_tools_free:
 * @self: the table to free, or NULL.
 *
 * Frees the table. The borrowed config and Kodi client are left untouched.
 * Safe to call with NULL.
 */
void
mk_tools_free (MkTools *self)
{
  if (self == NULL)
    return;
  g_free (self);
}

/**
 * mk_tools_count:
 * @self: the table.
 *
 * @return the number of tools in the table.
 */
guint
mk_tools_count (MkTools *self)
{
  g_return_val_if_fail (self != NULL, 0);
  return G_N_ELEMENTS (mk_tool_defs);
}

/**
 * mk_tools_list:
 * @self: the table.
 *
 * Renders every tool as `{ "name", "description", "inputSchema" }` for
 * `tools/list` (§3.3.4).
 *
 * @return a newly allocated JSON array node; free with json_node_unref().
 */
JsonNode *
mk_tools_list (MkTools *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  for (gsize i = 0; i < G_N_ELEMENTS (mk_tool_defs); i++)
    {
      const MkToolDef *def = &mk_tool_defs[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "name");
      json_builder_add_string_value (b, def->name);
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, def->description);
      json_builder_set_member_name (b, "inputSchema");
      json_builder_add_value (b, def->schema (self)); /* takes ownership */
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  return json_builder_get_root (b);
}

/**
 * mk_tools_call:
 * @self: the table.
 * @name: the tool to invoke.
 * @arguments: the call's "arguments" object, or NULL.
 * @error: return location for a GError, or NULL.
 *
 * Dispatches a `tools/call` (§3.3.5). For a known tool, returns the result
 * envelope: on success the handler's JSON as text with `isError: false`; on a
 * handler failure (or a not-yet-implemented tool) the detail as JSON text with
 * `isError: true` (§3.4). Only an unknown @name returns NULL, with @error set
 * to MK_TOOLS_ERROR_UNKNOWN_TOOL so the caller can raise a protocol error.
 *
 * @return the result node (free with json_node_unref()), or NULL with @error
 *         set for an unknown tool.
 */
JsonNode *
mk_tools_call (MkTools     *self,
               const char  *name,
               JsonObject  *arguments,
               GError     **error)
{
  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (name != NULL, NULL);

  const MkToolDef *def = find_tool (name);
  if (def == NULL)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_UNKNOWN_TOOL,
                   "unknown tool: %s", name);
      return NULL;
    }

  if (def->handler == NULL)
    {
      g_autofree char *msg =
        g_strdup_printf ("tool \"%s\" is not implemented yet", name);
      g_autofree char *text = error_text (msg);
      return make_result (text, TRUE);
    }

  GError *local = NULL;
  g_autoptr (JsonNode) result = def->handler (self, arguments, &local);
  if (result == NULL)
    {
      g_autofree char *text =
        error_text (local ? local->message : "tool failed");
      g_clear_error (&local);
      return make_result (text, TRUE);
    }

  g_autofree char *text = node_to_string (result);
  return make_result (text, FALSE);
}
