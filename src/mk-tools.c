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
 * set on failure. A NULL handler in the table marks a tool not yet implemented. */
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

/* ---- The tool table -------------------------------------------------------
 *
 * Intentionally empty: the tool surface is being rebuilt from the comprehensive
 * Kodi JSON-RPC inventory (../TODO.md §12) once the taxonomy that groups those
 * methods into tools is settled. Each tool lands here as a
 * `{ name, description, schema, handler }` row, with its schema builder and
 * handler defined above. Until then `tools/list` reports no tools and every
 * `tools/call` is an unknown tool.
 */
static const MkToolDef mk_tool_defs[] = {
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
  const MkToolDef *end = mk_tool_defs + G_N_ELEMENTS (mk_tool_defs);
  for (const MkToolDef *def = mk_tool_defs; def < end; def++)
    if (g_strcmp0 (def->name, name) == 0)
      return def;
  return NULL;
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
 * `tools/list` (§3.3.4). With no tools wired yet this is an empty array.
 *
 * @return a newly allocated JSON array node; free with json_node_unref().
 */
JsonNode *
mk_tools_list (MkTools *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  const MkToolDef *end = mk_tool_defs + G_N_ELEMENTS (mk_tool_defs);
  for (const MkToolDef *def = mk_tool_defs; def < end; def++)
    {
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
