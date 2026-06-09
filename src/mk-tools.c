/* mcp-kodi — tool table: schemas + handlers. See mk-tools.h and ../TODO.md §5. */

#include "config.h"

#include "mk-tools.h"

struct _MkTools
{
  MkConfig *config; /* borrowed; must outlive the table */
  MkKodi   *kodi;   /* borrowed; must outlive the table */
};

G_DEFINE_QUARK (mk-tools-error-quark, mk_tools_error)

typedef struct _MkToolDef MkToolDef;

/* A handler runs a tool: it reads @args (may be NULL) and its own table row
 * @def, drives @self->kodi, and returns the result payload as a newly allocated
 * JsonNode, or NULL with @error set on failure. Receiving @def lets one handler
 * serve a whole family of data-driven rows — e.g. every Button shares
 * handler_button() and reads its action from @def (§11.6.1). A NULL handler in
 * the table marks a tool not yet implemented. */
typedef JsonNode *(*MkToolHandler) (MkTools         *self,
                                    const MkToolDef *def,
                                    JsonObject      *args,
                                    GError         **error);

/* Builds a tool's `inputSchema` (a JSON Schema object) against @self, so the
 * schema can name the configured instances (§5.1). */
typedef JsonNode *(*MkToolSchema) (MkTools *self);

struct _MkToolDef
{
  const char   *name;
  const char   *description;
  MkToolSchema  schema;
  MkToolHandler handler; /* NULL until the tool is implemented */
  /* Per-handler payload string carried by data-driven rows (§11.6.1), or NULL.
   * handler_button() reads it as the Input.ExecuteAction action ("play",
   * "pause", "stop"); handler_mute() reads it as the SetMute value ("true" to
   * mute, "false" to unmute). */
  const char   *action;
};

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
 * prop_enum:
 * @b: the builder, positioned inside a `properties` object.
 * @name: the property name.
 * @values: NULL-terminated array of allowed string values.
 * @desc: a human description, or NULL to omit it.
 *
 * Adds `"<name>": { "type": "string", "enum": [ ... ][, "description": …] }` —
 * a closed set of string choices (e.g. an `action` selector).
 */
static void
prop_enum (JsonBuilder *b, const char *name, const char *const *values,
           const char *desc)
{
  json_builder_set_member_name (b, name);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "string");
  json_builder_set_member_name (b, "enum");
  json_builder_begin_array (b);
  for (gsize i = 0; values[i] != NULL; i++)
    json_builder_add_string_value (b, values[i]);
  json_builder_end_array (b);
  if (desc != NULL)
    {
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, desc);
    }
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
        {
          const char *key = l->data;
          MkInstance *inst = mk_config_get_instance (self->config, key);
          /* Show the human-readable name alongside the key the tool expects,
           * e.g. `mini02 ("Living Room TV")`, when the instance has one. */
          if (inst != NULL && inst->name != NULL)
            g_string_append_printf (desc, "%s (\"%s\")", key, inst->name);
          else
            g_string_append (desc, key);
          if (l->next != NULL)
            g_string_append (desc, ", ");
        }
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
 * schema_instance_only:
 * @self: the table.
 *
 * Schema for tools whose only argument is the optional `instance` — every
 * Button (§11.6.1) plus other device-targeted, argument-free tools.
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
 * schema_instances:
 * @self: the table (unused; the schema is static).
 *
 * Schema for the `instances` config tool (§11.6.3): an `action` selector plus
 * the per-instance fields used by `set`. Only `action` is required; `key` is
 * required by `set`/`remove` but that is enforced in the handler so the schema
 * stays a flat property list.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_instances (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  static const char *const actions[] = { "get", "set", "remove", NULL };
  prop_enum (b, "action", actions,
             "Operation: \"get\" lists configured instances, \"set\" "
             "creates/updates one, \"remove\" deletes one.");
  prop_typed (b, "key", "string",
              "Instance key — the short id tools target. Required for \"set\" "
              "and \"remove\".");
  prop_typed (b, "name", "string",
              "Human-readable display label (\"set\").");
  prop_typed (b, "host", "string",
              "Kodi host as \"host[:port]\" (\"set\").");
  prop_typed (b, "auth", "string",
              "HTTP Basic credentials as \"user:pass\" (\"set\"). Write-only: "
              "never returned by \"get\".");
  prop_typed (b, "scheme", "string",
              "URL scheme, \"http\" or \"https\" (\"set\"; default \"https\").");
  prop_typed (b, "insecure", "boolean",
              "Accept a self-signed TLS certificate, like curl -k (\"set\").");
  prop_typed (b, "default", "boolean",
              "When true on \"set\", make this instance the default.");

  static const char *const required[] = { "action", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/* ---- Shared argument resolution -------------------------------------------- */

/**
 * arg_instance:
 * @args: the tool-call arguments object, or NULL.
 *
 * Reads the optional `instance` argument (§5.1). The returned string is
 * borrowed from @args.
 *
 * @return the instance name, or NULL to mean "the configured default".
 */
static const char *
arg_instance (JsonObject *args)
{
  if (args == NULL || !json_object_has_member (args, "instance"))
    return NULL;
  return json_object_get_string_member_with_default (args, "instance", NULL);
}

/* ---- Per-tool handlers ----------------------------------------------------
 *
 * Each handler reads @args and its row @def, drives @self->kodi, and returns
 * the result payload as a newly allocated JsonNode (or NULL with @error set).
 */

/**
 * copy_member:
 * @b: the builder, positioned inside an object.
 * @src: the source object to copy from, or NULL.
 * @name: the member to copy.
 *
 * If @src has member @name, deep-copies it (value and key) into @b; otherwise
 * does nothing. Used to forward selected fields out of Kodi replies.
 */
static void
copy_member (JsonBuilder *b, JsonObject *src, const char *name)
{
  if (src == NULL || !json_object_has_member (src, name))
    return;
  json_builder_set_member_name (b, name);
  json_builder_add_value (b, json_node_copy (json_object_get_member (src, name)));
}

/**
 * player_props:
 * @playerid: the active player's id.
 * @fields: NULL-terminated array of property/field names to request.
 *
 * Builds a `{ "playerid": <id>, "properties": [ ... ] }` params node shared by
 * `Player.GetProperties` and `Player.GetItem`.
 *
 * @return a newly allocated JsonNode; free with json_node_unref().
 */
static JsonNode *
player_props (gint64 playerid, const char *const *fields)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "playerid");
  json_builder_add_int_value (b, playerid);
  json_builder_set_member_name (b, "properties");
  json_builder_begin_array (b);
  for (gsize i = 0; fields[i] != NULL; i++)
    json_builder_add_string_value (b, fields[i]);
  json_builder_end_array (b);
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/**
 * player_state:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @error: return location for a GError, or NULL.
 *
 * Builds the server's canonical now-playing snapshot for @instance — the
 * **shared response shape** returned by the many tools whose effect is a
 * change in player state (every transport Button — play/pause/stop (§11.6.1) —
 * plus seek, skip, speed, handoff, status, …). Reusing one builder keeps that
 * response identical across all of them, so a client learns the shape once and
 * any of those calls answers "what is loaded, is it playing, and how far in".
 *
 * There is no single Kodi method for it, so it combines three calls:
 * `Player.GetActivePlayers` to find the active player, then
 * `Player.GetProperties` (speed/time/totaltime) and `Player.GetItem`
 * (file/label/title) on it (§5.4). Progress is reported as `time`/`totaltime`
 * only; a percentage is just `time / totaltime` and is left for the caller to
 * compute rather than returned redundantly.
 *
 * @return a newly allocated object node — `{ "state": "stopped" }` when nothing
 *         is active, else `{ "state": "playing"|"paused", "type"?, "file"?,
 *         "label"?, "title"?, "time"?, "totaltime"? }` — or NULL with @error set
 *         if a call fails. Example, a video paused 15:38 into 46:40 (≈33%):
 * @code{.json}
 * {
 *   "state": "paused",
 *   "type": "video",
 *   "file": "/storage/Serials/Law & Order/Season 4/Law & Order 4x01 Sweeps.avi",
 *   "label": "Law & Order 4x01 Sweeps.avi",
 *   "title": "",
 *   "time":      { "hours": 0, "minutes": 15, "seconds": 38, "milliseconds": 507 },
 *   "totaltime": { "hours": 0, "minutes": 46, "seconds": 40, "milliseconds": 633 }
 * }
 * @endcode
 */
static JsonNode *
player_state (MkTools *self, const char *instance, GError **error)
{
  g_autoptr (JsonNode) active =
    mk_kodi_call (self->kodi, instance, "Player.GetActivePlayers", NULL, error);
  if (active == NULL)
    return NULL;

  JsonArray *players =
    JSON_NODE_HOLDS_ARRAY (active) ? json_node_get_array (active) : NULL;
  if (players == NULL || json_array_get_length (players) == 0)
    {
      g_autoptr (JsonBuilder) b = json_builder_new ();
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "state");
      json_builder_add_string_value (b, "stopped");
      json_builder_end_object (b);
      return json_builder_get_root (b);
    }

  JsonObject *p0 = json_array_get_object_element (players, 0);
  gint64 playerid = json_object_get_int_member_with_default (p0, "playerid", 0);
  const char *ptype =
    json_object_get_string_member_with_default (p0, "type", NULL);

  static const char *const prop_fields[] = { "speed", "time", "totaltime",
                                             NULL };
  g_autoptr (JsonNode) pparams = player_props (playerid, prop_fields);
  g_autoptr (JsonNode) pres = mk_kodi_call (self->kodi, instance,
                                            "Player.GetProperties", pparams,
                                            error);
  if (pres == NULL)
    return NULL;
  JsonObject *props = json_node_get_object (pres);

  static const char *const item_fields[] = { "title", "file", NULL };
  g_autoptr (JsonNode) iparams = player_props (playerid, item_fields);
  g_autoptr (JsonNode) ires =
    mk_kodi_call (self->kodi, instance, "Player.GetItem", iparams, error);
  if (ires == NULL)
    return NULL;
  JsonObject *item =
    json_object_get_object_member (json_node_get_object (ires), "item");

  gint64 speed = json_object_get_int_member_with_default (props, "speed", 0);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "state");
  json_builder_add_string_value (b, speed != 0 ? "playing" : "paused");
  if (ptype != NULL)
    {
      json_builder_set_member_name (b, "type");
      json_builder_add_string_value (b, ptype);
    }
  copy_member (b, item, "file");
  copy_member (b, item, "label");
  copy_member (b, item, "title");
  copy_member (b, props, "time");
  copy_member (b, props, "totaltime");
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/**
 * app_audio_state:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @error: return location for a GError, or NULL.
 *
 * Builds the server's canonical audio snapshot for @instance — the **shared
 * response shape** returned by the tools whose effect is a change in the
 * application's audio output: mute/unmute (§11.6.1) and, later, the volume Knob
 * (§11.6.2.1). As with player_state(), reusing one builder keeps that response
 * identical across all of them, so a client learns the shape once and any of
 * those calls answers "is it muted, and how loud".
 *
 * Reads it back with a single `Application.GetProperties` for `muted` and
 * `volume`, rather than trusting the bare reply of the setter, so the snapshot
 * reflects Kodi's settled state.
 *
 * @return a newly allocated object node — `{ "muted": <bool>, "volume": <int> }`
 *         — or NULL with @error set if the call fails.
 */
static JsonNode *
app_audio_state (MkTools *self, const char *instance, GError **error)
{
  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  json_builder_set_member_name (pb, "properties");
  json_builder_begin_array (pb);
  json_builder_add_string_value (pb, "muted");
  json_builder_add_string_value (pb, "volume");
  json_builder_end_array (pb);
  json_builder_end_object (pb);
  g_autoptr (JsonNode) params = json_builder_get_root (pb);

  g_autoptr (JsonNode) res = mk_kodi_call (self->kodi, instance,
                                           "Application.GetProperties", params,
                                           error);
  if (res == NULL)
    return NULL;
  JsonObject *props = json_node_get_object (res);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  copy_member (b, props, "muted");
  copy_member (b, props, "volume");
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/**
 * handler_button:
 * @self: the tool table.
 * @def: this Button's table row; @def->action names the action to fire.
 * @args: the call arguments (optional `instance`), or NULL.
 * @error: return location for a GError, or NULL.
 *
 * Implements a Button (§11.6.1): an argument-free remote keypress. Fires
 * `Input.ExecuteAction` with `{ "action": <def->action> }` on the target
 * instance — no playerid, no active-player resolution — then returns the
 * resulting player_state() snapshot (what is loaded, playing vs paused, and how
 * far in) rather than Kodi's bare "OK", so the caller sees the action's effect.
 *
 * `Input.ExecuteAction` replies "OK" the moment the action is queued, before
 * the player's `speed` settles, so a snapshot taken immediately can still read
 * the pre-action state (e.g. "paused" right after play). A brief settle delay
 * before snapshotting makes the reported state reflect the action.
 *
 * @return the post-action player-state object, or NULL with @error set.
 */
#define MK_BUTTON_SETTLE_US (200 * 1000) /* let the action take effect first */

static JsonNode *
handler_button (MkTools *self, const MkToolDef *def, JsonObject *args,
                GError **error)
{
  const char *instance = arg_instance (args);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "action");
  json_builder_add_string_value (b, def->action);
  json_builder_end_object (b);
  g_autoptr (JsonNode) params = json_builder_get_root (b);

  g_autoptr (JsonNode) acted = mk_kodi_call (self->kodi, instance,
                                             "Input.ExecuteAction", params,
                                             error);
  if (acted == NULL)
    return NULL;

  g_usleep (MK_BUTTON_SETTLE_US);
  return player_state (self, instance, error);
}

/**
 * handler_noop:
 * @self: the tool table.
 * @def: this Button's table row (unused; it fires no action).
 * @args: the call arguments (optional `instance`), or NULL.
 * @error: return location for a GError, or NULL.
 *
 * Implements the noop Button (§11.6.1.6): unlike handler_button() it sends no
 * `Input.ExecuteAction` and changes nothing — it simply returns the
 * player_state() snapshot for the target instance. Because building that
 * snapshot already calls Kodi (`Player.GetActivePlayers` and friends), a clean
 * result doubles as a reachability check and a settle delay; a comms failure
 * surfaces as the usual categorised tool error. The caller uses it to learn
 * whether Kodi answers and what is currently loaded/playing, without poking the
 * player.
 *
 * @return the current player-state object, or NULL with @error set.
 */
static JsonNode *
handler_noop (MkTools *self, const MkToolDef *def, JsonObject *args,
              GError **error)
{
  (void) def;
  return player_state (self, arg_instance (args), error);
}

/**
 * handler_mute:
 * @self: the tool table.
 * @def: this row; @def->action is "true" to mute or "false" to unmute.
 * @args: the call arguments (optional `instance`), or NULL.
 * @error: return location for a GError, or NULL.
 *
 * Implements the mute and unmute Buttons (§11.6.1). Unlike the transport
 * Buttons these are not remote keypresses: they set the audio output directly
 * with `Application.SetMute` `{ "mute": true|false }` (an absolute set, not a
 * toggle, so "mute" is always on and "unmute" always off regardless of the
 * current state). Returns the app_audio_state() snapshot so the caller sees the
 * resulting mute/volume, mirroring how the transport Buttons return
 * player_state().
 *
 * @return the post-action audio-state object, or NULL with @error set.
 */
static JsonNode *
handler_mute (MkTools *self, const MkToolDef *def, JsonObject *args,
              GError **error)
{
  const char *instance = arg_instance (args);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "mute");
  json_builder_add_boolean_value (b, g_str_equal (def->action, "true"));
  json_builder_end_object (b);
  g_autoptr (JsonNode) params = json_builder_get_root (b);

  g_autoptr (JsonNode) acted = mk_kodi_call (self->kodi, instance,
                                             "Application.SetMute", params,
                                             error);
  if (acted == NULL)
    return NULL;

  return app_audio_state (self, instance, error);
}

/**
 * instances_list:
 * @self: the tool table.
 *
 * Builds the **shared response shape** of the `instances` config tool (§11.6.3),
 * returned by all three actions so the caller always sees the resulting config
 * state. Lists each configured instance — `{ key, name?, host, scheme, insecure,
 * has_auth }` in sorted key order — alongside the `default` key. Credentials are
 * deliberately reduced to the boolean `has_auth`: the password is write-only and
 * never leaves the server.
 *
 * @return a newly allocated object node
 *         `{ "default": <key>|null, "instances": [ … ] }`.
 */
static JsonNode *
instances_list (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "default");
  const char *def = mk_config_get_default (self->config);
  if (def != NULL)
    json_builder_add_string_value (b, def);
  else
    json_builder_add_null_value (b);

  json_builder_set_member_name (b, "instances");
  json_builder_begin_array (b);
  GList *keys = mk_config_instance_names (self->config);
  for (GList *l = keys; l != NULL; l = l->next)
    {
      const char *key = l->data;
      MkInstance *inst = mk_config_get_instance (self->config, key);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "key");
      json_builder_add_string_value (b, key);
      if (inst->name != NULL)
        {
          json_builder_set_member_name (b, "name");
          json_builder_add_string_value (b, inst->name);
        }
      json_builder_set_member_name (b, "host");
      json_builder_add_string_value (b, inst->host);
      json_builder_set_member_name (b, "scheme");
      json_builder_add_string_value (b, inst->scheme ? inst->scheme : "https");
      json_builder_set_member_name (b, "insecure");
      json_builder_add_boolean_value (b, inst->insecure);
      json_builder_set_member_name (b, "has_auth");
      json_builder_add_boolean_value (b,
                                      inst->auth != NULL && inst->auth[0] != '\0');
      json_builder_end_object (b);
    }
  g_list_free (keys);
  json_builder_end_array (b);

  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/**
 * arg_str:
 * @args: the call arguments object (may be NULL).
 * @name: the member to read.
 * @fallback: value to return when @name is absent.
 *
 * Reads an optional string argument: when @args has member @name its string
 * value is returned (NULL if the member is JSON null), otherwise @fallback. The
 * returned string is borrowed from @args (or is @fallback). Used by
 * handler_instances() to merge a `set` request over the existing instance —
 * "absent" means keep the current value, an explicit null clears it.
 */
static const char *
arg_str (JsonObject *args, const char *name, const char *fallback)
{
  if (args == NULL || !json_object_has_member (args, name))
    return fallback;
  return json_object_get_string_member_with_default (args, name, NULL);
}

/**
 * handler_instances:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; `action` selects the operation.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `instances` config tool (§11.6.3): read/write the server's own
 * instance config (§7), making no Kodi call. `get` lists instances; `set`
 * upserts one by `key` (provided fields override, omitted fields keep the
 * current value or take defaults) and may make it the default; `remove` deletes
 * one, refusing the current default so the config is never left defaultless.
 * `set`/`remove` persist atomically (mk_config_save(), §7.4) — the save trigger
 * of §7.5. Every action returns instances_list().
 *
 * @return the resulting instance list, or NULL with @error set (bad arguments,
 *         or a save failure).
 */
static JsonNode *
handler_instances (MkTools *self, const MkToolDef *def, JsonObject *args,
                   GError **error)
{
  (void) def;
  const char *action = arg_str (args, "action", NULL);
  if (action == NULL)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "instances: \"action\" is required (get/set/remove)");
      return NULL;
    }

  if (g_str_equal (action, "get"))
    return instances_list (self);

  if (g_str_equal (action, "set"))
    {
      const char *key = arg_str (args, "key", NULL);
      if (key == NULL || key[0] == '\0')
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "instances set: \"key\" is required");
          return NULL;
        }

      /* Merge the request over any existing instance: absent fields are kept. */
      MkInstance *cur = mk_config_get_instance (self->config, key);
      const char *name   = arg_str (args, "name",   cur ? cur->name   : NULL);
      const char *host   = arg_str (args, "host",   cur ? cur->host   : NULL);
      const char *auth   = arg_str (args, "auth",   cur ? cur->auth   : NULL);
      const char *scheme = arg_str (args, "scheme", cur ? cur->scheme : NULL);
      gboolean insecure  = (args != NULL && json_object_has_member (args, "insecure"))
                             ? json_object_get_boolean_member_with_default (args, "insecure", FALSE)
                             : (cur ? cur->insecure : FALSE);

      /* mk_instance_new() copies every string, so reading from @cur here is safe
       * even though mk_config_set_instance() then frees @cur. */
      MkInstance *inst = mk_instance_new (name, host, auth, scheme, insecure);
      mk_config_set_instance (self->config, key, inst); /* takes ownership */

      if (args != NULL && json_object_has_member (args, "default")
          && json_object_get_boolean_member_with_default (args, "default", FALSE))
        mk_config_set_default (self->config, key);

      if (!mk_config_save (self->config, NULL, error))
        return NULL;
      return instances_list (self);
    }

  if (g_str_equal (action, "remove"))
    {
      const char *key = arg_str (args, "key", NULL);
      if (key == NULL || key[0] == '\0')
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "instances remove: \"key\" is required");
          return NULL;
        }
      if (g_strcmp0 (mk_config_get_default (self->config), key) == 0)
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "instances remove: cannot remove the default instance "
                       "\"%s\"; set a different default first", key);
          return NULL;
        }
      if (!mk_config_remove_instance (self->config, key))
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "instances remove: no instance named \"%s\"", key);
          return NULL;
        }
      if (!mk_config_save (self->config, NULL, error))
        return NULL;
      return instances_list (self);
    }

  g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
               "instances: unknown action \"%s\" (use get/set/remove)", action);
  return NULL;
}

/* ---- The tool table -------------------------------------------------------
 *
 * Being rebuilt from the comprehensive Kodi JSON-RPC inventory (../TODO.md §12)
 * grouped by the §11.6 taxonomy. Each tool is a
 * `{ name, description, schema, handler, action }` row; a NULL handler yields a
 * clean "not implemented" result.
 */
static const MkToolDef mk_tool_defs[] = {
  /* ---- Buttons — argument-free remote keypresses via Input.ExecuteAction
   *      (§11.6.1). Each fires handler_button() with the `action` field. ---- */

  /**
   * play (Button):
   *
   * Press Play on the Kodi remote — resume a paused player or begin playback of
   * the focused/active item on the target instance. A remote keypress, not a
   * player command: no playerid and no active-player resolution (§11.6.1).
   *
   * Call:  Input.ExecuteAction { "action": "play" }, then player_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the resulting player-state snapshot: `{ "state":
   *         "playing"|"paused"|"stopped", "file", "label", "title", "time",
   *         "totaltime" }` (fields present when a player is active). With
   *         nothing loaded the action is a no-op and the reply is
   *         `{ "state": "stopped" }`.
   */
  { "play", "Press Play on the Kodi remote: resume/begin playback on the "
            "target instance.",
    schema_instance_only, handler_button, "play" },

  /**
   * pause (Button):
   *
   * Press Pause on the Kodi remote — pause the active player on the target
   * instance. A remote keypress, not a player command: no playerid and no
   * active-player resolution (§11.6.1).
   *
   * Call:  Input.ExecuteAction { "action": "pause" }, then player_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the resulting player-state snapshot (player_state()). With nothing
   *         loaded the action is a no-op and the reply is `{ "state": "stopped" }`.
   */
  { "pause", "Press Pause on the Kodi remote: pause the active player on the "
             "target instance.",
    schema_instance_only, handler_button, "pause" },

  /**
   * stop (Button):
   *
   * Press Stop on the Kodi remote — stop the active player on the target
   * instance. A remote keypress, not a player command: no playerid and no
   * active-player resolution (§11.6.1).
   *
   * Call:  Input.ExecuteAction { "action": "stop" }, then player_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the resulting player-state snapshot (player_state()); after a
   *         successful stop nothing is loaded, so `{ "state": "stopped" }`.
   */
  { "stop", "Press Stop on the Kodi remote: stop the active player on the "
            "target instance.",
    schema_instance_only, handler_button, "stop" },

  /**
   * mute (Button):
   *
   * Mute the target instance's audio output. Not a remote keypress: sets the
   * mute state directly with Application.SetMute (§11.6.1), so it is always on
   * regardless of the current state.
   *
   * Call:  Application.SetMute { "mute": true }, then app_audio_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the resulting audio-state snapshot: `{ "muted": true, "volume": <int> }`
   *         (app_audio_state()).
   */
  { "mute", "Mute the target instance's audio output.",
    schema_instance_only, handler_mute, "true" },

  /**
   * unmute (Button):
   *
   * Unmute the target instance's audio output. Not a remote keypress: sets the
   * mute state directly with Application.SetMute (§11.6.1), so it is always off
   * regardless of the current state.
   *
   * Call:  Application.SetMute { "mute": false }, then app_audio_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the resulting audio-state snapshot: `{ "muted": false, "volume": <int> }`
   *         (app_audio_state()).
   */
  { "unmute", "Unmute the target instance's audio output.",
    schema_instance_only, handler_mute, "false" },

  /**
   * noop (Button):
   *
   * Do nothing to the player, just report its state. Fires no action and
   * changes nothing (§11.6.1.6); it only builds and returns the player_state()
   * snapshot. Because that snapshot is read from Kodi, a successful call also
   * confirms the instance is reachable — a reachability + state probe.
   *
   * Call:  no Kodi action; player_state() to report what is loaded/playing.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the current player-state snapshot (player_state()), identical in
   *         shape to what the transport Buttons return. With nothing loaded the
   *         reply is `{ "state": "stopped" }`. A communication failure instead
   *         yields the categorised tool error, telling the caller Kodi is
   *         unreachable and why.
   */
  { "noop", "Report the target instance's player state without changing "
            "anything — a reachability and state probe.",
    schema_instance_only, handler_noop, NULL },

  /* ---- Config tools — read/write the server's own instance config (§11.6.3).
   *      These make no Kodi call. ---- */

  /**
   * instances (Config tool):
   *
   * Read or modify the set of configured Kodi instances (§7) — the boxes every
   * other tool targets by key. Makes no Kodi call; `set`/`remove` persist the
   * config file atomically (§7.4). The `action` argument selects the operation:
   *
   *   - "get":    list instances and the default (no password is ever returned).
   *   - "set":    create/update the instance named by `key` (`name`/`host`/
   *               `auth`/`scheme`/`insecure`); omitted fields keep their current
   *               value. `default: true` makes it the default.
   *   - "remove": delete the instance named by `key` (not the default).
   *
   * @param action (required): "get" | "set" | "remove".
   * @param key: instance key; required for "set" and "remove".
   * @param name|host|auth|scheme|insecure|default: instance fields for "set".
   * @return the resulting instance list (instances_list()): `{ "default": <key>,
   *         "instances": [ { "key", "name"?, "host", "scheme", "insecure",
   *         "has_auth" } ] }`.
   */
  { "instances", "Read or modify the configured Kodi instances "
                 "(action: get/set/remove). Manages the MCP server's own "
                 "config, not a Kodi device.",
    schema_instances, handler_instances, NULL },
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
 * kodi_comms_hint:
 * @error: the GError a handler failed with, or NULL.
 * @category: out; set to a short category token on a communication failure.
 * @hint: out; set to a remedy hint the AI can relay to the user.
 *
 * Classifies @error for inline troubleshooting (Option A). A *communication*
 * failure — the MCP server and the Kodi player could not talk (instance not
 * configured, unreachable, HTTP-rejected, or a non-JSON-RPC reply) — yields a
 * category token and a specific setup/connectivity remedy, so the AI on the
 * other end can help the user fix their system. A Kodi `RPC` error (the player
 * understood the request and refused it) and any non-Kodi error are *not*
 * communication failures: they carry no setup hint, since the wiring is fine.
 *
 * @return TRUE and sets @category/@hint (both static strings) for a
 *         communication failure; FALSE otherwise (leaving the outputs unset).
 */
static gboolean
kodi_comms_hint (const GError *error, const char **category, const char **hint)
{
  if (error == NULL || error->domain != MK_KODI_ERROR)
    return FALSE;

  switch (error->code)
    {
    case MK_KODI_ERROR_NO_INSTANCE:
      *category = "no_instance";
      *hint = "The target Kodi instance is not defined in the MCP server "
              "configuration, or has no host. Check that the server config "
              "names this instance and gives it a host (§7).";
      return TRUE;
    case MK_KODI_ERROR_TRANSPORT:
      *category = "transport";
      *hint = "Could not reach Kodi. Check that Kodi is running and that the "
              "configured host/port is correct and reachable — DNS, firewall, "
              "and any reverse proxy in front of Kodi all need to be up.";
      return TRUE;
    case MK_KODI_ERROR_HTTP:
      *category = "http";
      *hint = "Kodi answered but rejected the request at the HTTP layer. For "
              "401/403, check the username/password and that Kodi's Settings > "
              "Services > Control > 'Allow remote control via HTTP' is enabled "
              "with authentication. Other statuses usually mean a wrong path or "
              "port, or a misconfigured reverse proxy.";
      return TRUE;
    case MK_KODI_ERROR_PROTOCOL:
      *category = "protocol";
      *hint = "Got a response, but it was not a valid Kodi JSON-RPC reply — "
              "something other than Kodi may be answering at that address. "
              "Check the host/port and that any reverse proxy routes to Kodi's "
              "/jsonrpc endpoint rather than to a web page.";
      return TRUE;
    case MK_KODI_ERROR_RPC:
    default:
      return FALSE; /* Kodi refused a well-formed request: not a setup problem. */
    }
}

/**
 * error_text:
 * @message: the failure detail; always emitted as "error".
 * @category: a communication-failure category token, or NULL.
 * @hint: a remedy hint paired with @category, or NULL.
 *
 * Shapes a tool failure as machine-readable JSON text (§2.2), the single source
 * of truth for what a tool error looks like. Two shapes:
 *
 *   - Generic (RPC error, bad args, not-implemented; @category NULL):
 *       `{ "error": "<message>" }`
 *   - Communication failure (@category non-NULL; see kodi_comms_hint()):
 *       `{ "error": "<message>", "category": "<token>", "hint": "<remedy>" }`
 *     so the AI can recognise a server↔player connectivity problem and walk the
 *     user through Kodi/MCP setup (Option A).
 *
 * This text becomes the `isError: true` content block in the `tools/call`
 * result (make_result()); it is never a protocol-level error (§3.4). @message
 * (and @hint) are escaped by the JSON builder.
 *
 * @return a newly allocated JSON string; free with g_free().
 */
static char *
error_text (const char *message, const char *category, const char *hint)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "error");
  json_builder_add_string_value (b, message);
  if (category != NULL)
    {
      json_builder_set_member_name (b, "category");
      json_builder_add_string_value (b, category);
      json_builder_set_member_name (b, "hint");
      json_builder_add_string_value (b, hint);
    }
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
 * `isError: true` (§3.4) — see error_text() for the shape. A server↔player
 * communication failure additionally carries a `category` and a setup `hint`
 * (kodi_comms_hint()) so the AI can help the user fix connectivity. Only an
 * unknown @name returns NULL, with @error set to MK_TOOLS_ERROR_UNKNOWN_TOOL so
 * the caller can raise a protocol error.
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
      g_autofree char *text = error_text (msg, NULL, NULL);
      return make_result (text, TRUE);
    }

  GError *local = NULL;
  g_autoptr (JsonNode) result = def->handler (self, def, arguments, &local);
  if (result == NULL)
    {
      const char *category = NULL, *hint = NULL;
      kodi_comms_hint (local, &category, &hint);
      g_autofree char *text =
        error_text (local ? local->message : "tool failed", category, hint);
      g_clear_error (&local);
      return make_result (text, TRUE);
    }

  g_autofree char *text = node_to_string (result);
  return make_result (text, FALSE);
}
