/* mcp-kodi — tool table: schemas + handlers. See mk-tools.h and ../TODO.md §5.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 */

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
 * has_auth, allow_rpc }` in sorted key order — alongside the `default` key.
 * Credentials are deliberately reduced to the boolean `has_auth`: the password
 * is write-only and never leaves the server. `allow_rpc` is reported read-only
 * (§7.7) so the caller can see which boxes permit the `rpc` escape hatch, but it
 * cannot be changed here — only by hand-editing the config file.
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
      json_builder_set_member_name (b, "allow_rpc");
      json_builder_add_boolean_value (b, inst->allow_rpc);
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
      /* allow_rpc (§7.7) is deliberately never read from @args: the escape-hatch
       * gate is hand-edit-only, so the model cannot grant itself rpc. Preserve
       * the existing value (FALSE for a brand-new instance). */
      gboolean allow_rpc = cur ? cur->allow_rpc : FALSE;

      /* mk_instance_new() copies every string, so reading from @cur here is safe
       * even though mk_config_set_instance() then frees @cur. */
      MkInstance *inst = mk_instance_new (name, host, auth, scheme, insecure,
                                          allow_rpc);
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

/* ---- search (§11.6.4.1) ---------------------------------------------------
 *
 * One generic find-by-name tool that drills each media type's hierarchy down to
 * playable **leaf files** (§5.10). The key design choice is to descend to the
 * album/season level only when the user names one, so every *wide* query (bare
 * artist, bare show) is a single Kodi call whose `limits {start, end, total}`
 * maps straight onto the tool's `total`/`limit`/`offset`. The one multi-call
 * case — a substring album `title` matching several albums — is collected
 * app-side and flagged `approximate`.
 */

#define MK_SEARCH_DEFAULT_LIMIT 50  /* default leaf cap when `limit` omitted */
#define MK_SEARCH_MAX_LIMIT     500 /* hard ceiling on `limit` */
#define MK_SEARCH_RESOLVE_SCAN  25  /* candidates fetched when resolving a name */

/* Leaf-row members forwarded from a Kodi item into a result row, when present.
 * A superset across the three types — copy_member() skips the ones a given item
 * lacks, so one list serves songs, episodes, and movies. `file` is the playfile
 * input; the per-type id (`songid`/`episodeid`/`movieid`) is the library handle. */
static const char *const mk_search_row_fields[] = {
  "songid", "episodeid", "movieid", "label", "title", "file",
  "track", "season", "episode", "year", "artist", "album",
  "duration", "runtime", "firstaired", NULL
};

/* Song leaf properties requested from AudioLibrary.GetSongs. The `artist`
 * display field is deliberately omitted: it is redundant with the resolved
 * artist echoed in `resolved`, and — fatally — Kodi 19.4 turns a `GetSongs`
 * `{artistid}` query that both sorts and requests `artist` into a ~20s join over
 * a large discography (either alone stays milliseconds), which would time the
 * call out. */
static const char *const mk_search_song_props[] = {
  "title", "track", "duration", "album", "file", NULL
};

/**
 * arg_int:
 * @args: the call arguments object, or NULL.
 * @name: the member to read.
 * @out: return location for the value when present.
 *
 * Reads an optional integer argument. The returned @out is set only when @name
 * is present, so the caller can tell "omitted" from an explicit value.
 *
 * @return TRUE and sets @out when @name is present; FALSE otherwise.
 */
static gboolean
arg_int (JsonObject *args, const char *name, gint64 *out)
{
  if (args == NULL || !json_object_has_member (args, name))
    return FALSE;
  *out = json_object_get_int_member_with_default (args, name, 0);
  return TRUE;
}

/**
 * arg_bool:
 * @args: the call arguments object, or NULL.
 * @name: the member to read.
 *
 * Reads an optional boolean argument, defaulting to FALSE when absent.
 *
 * @return the boolean value, or FALSE when @name is absent.
 */
static gboolean
arg_bool (JsonObject *args, const char *name)
{
  if (args == NULL || !json_object_has_member (args, name))
    return FALSE;
  return json_object_get_boolean_member_with_default (args, name, FALSE);
}

/**
 * ci_equal:
 * @a: first string, or NULL.
 * @b: second string, or NULL.
 *
 * Case-insensitive (Unicode case-folded) full-string equality.
 *
 * @return TRUE when both are non-NULL and equal ignoring case.
 */
static gboolean
ci_equal (const char *a, const char *b)
{
  if (a == NULL || b == NULL)
    return FALSE;
  g_autofree char *x = g_utf8_casefold (a, -1);
  g_autofree char *y = g_utf8_casefold (b, -1);
  return g_strcmp0 (x, y) == 0;
}

/**
 * ci_contains:
 * @haystack: string to search in, or NULL.
 * @needle: substring to look for, or NULL.
 *
 * Case-insensitive (Unicode case-folded) substring test, mirroring Kodi's
 * `contains` operator so app-side album matching agrees with server-side
 * filtering.
 *
 * @return TRUE when @needle occurs in @haystack ignoring case.
 */
static gboolean
ci_contains (const char *haystack, const char *needle)
{
  if (haystack == NULL || needle == NULL)
    return FALSE;
  g_autofree char *x = g_utf8_casefold (haystack, -1);
  g_autofree char *y = g_utf8_casefold (needle, -1);
  return g_strstr_len (x, -1, y) != NULL;
}

/**
 * add_field_filter:
 * @b: the builder, positioned inside a params object.
 * @field: the field name to match (e.g. "title", "artist", "episode").
 * @op: the operator ("contains" for substring, "is" for exact).
 * @value: the value to match (always a string, even for numeric fields).
 *
 * Adds a `"filter": { "field", "operator", "value" }` rule (§12.16.12/§12.3.6).
 */
static void
add_field_filter (JsonBuilder *b, const char *field, const char *op,
                  const char *value)
{
  json_builder_set_member_name (b, "filter");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "field");
  json_builder_add_string_value (b, field);
  json_builder_set_member_name (b, "operator");
  json_builder_add_string_value (b, op);
  json_builder_set_member_name (b, "value");
  json_builder_add_string_value (b, value);
  json_builder_end_object (b);
}

/**
 * add_id_filter:
 * @b: the builder, positioned inside a params object.
 * @key: the special filter key ("artistid" or "albumid").
 * @id: the library id to filter by.
 *
 * Adds Kodi's special id `"filter": { "<key>": <id> }` form (§12.3.6 steps 2-3)
 * — distinct from a field/operator rule.
 */
static void
add_id_filter (JsonBuilder *b, const char *key, gint64 id)
{
  json_builder_set_member_name (b, "filter");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, key);
  json_builder_add_int_value (b, id);
  json_builder_end_object (b);
}

/**
 * add_properties:
 * @b: the builder, positioned inside a params object.
 * @props: NULL-terminated array of property names, or NULL to omit.
 *
 * Adds the `"properties": [ ... ]` request when @props is non-NULL.
 */
static void
add_properties (JsonBuilder *b, const char *const *props)
{
  if (props == NULL)
    return;
  json_builder_set_member_name (b, "properties");
  json_builder_begin_array (b);
  for (gsize i = 0; props[i] != NULL; i++)
    json_builder_add_string_value (b, props[i]);
  json_builder_end_array (b);
}

/**
 * add_sort:
 * @b: the builder, positioned inside a params object.
 * @method: the Kodi sort method ("label", "track", "title", "episode", …).
 *
 * Adds an ascending `"sort": { "method", "order": "ascending" }`.
 */
static void
add_sort (JsonBuilder *b, const char *method)
{
  json_builder_set_member_name (b, "sort");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "method");
  json_builder_add_string_value (b, method);
  json_builder_set_member_name (b, "order");
  json_builder_add_string_value (b, "ascending");
  json_builder_end_object (b);
}

/**
 * add_limits:
 * @b: the builder, positioned inside a params object.
 * @start: first row (inclusive).
 * @end: one past the last row (exclusive).
 *
 * Adds Kodi's `"limits": { "start", "end" }` window. The reply still reports the
 * full `limits.total` regardless of the window.
 */
static void
add_limits (JsonBuilder *b, gint64 start, gint64 end)
{
  json_builder_set_member_name (b, "limits");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "start");
  json_builder_add_int_value (b, start);
  json_builder_set_member_name (b, "end");
  json_builder_add_int_value (b, end);
  json_builder_end_object (b);
}

/**
 * add_page_limits:
 * @b: the builder, positioned inside a params object.
 * @offset: rows to skip.
 * @limit: max rows to return.
 * @count: when TRUE, request a one-row window (just to read `total`).
 *
 * Translates the tool's paging into a Kodi `limits` window: `{offset,
 * offset+limit}` normally, or `{0, 1}` for a `count` request (the rows are
 * discarded; only `total` is reported).
 */
static void
add_page_limits (JsonBuilder *b, gint64 offset, gint64 limit, gboolean count)
{
  if (count)
    add_limits (b, 0, 1);
  else
    add_limits (b, offset, offset + limit);
}

/**
 * list_total:
 * @result: a Kodi list `result` object.
 *
 * Reads `result.limits.total` — the full match count, independent of the window
 * that was fetched.
 *
 * @return the total, or 0 when absent.
 */
static gint64
list_total (JsonNode *result)
{
  JsonObject *o = json_node_get_object (result);
  if (!json_object_has_member (o, "limits"))
    return 0;
  JsonObject *lim = json_object_get_object_member (o, "limits");
  return json_object_get_int_member_with_default (lim, "total", 0);
}

/**
 * collect_items:
 * @result: a Kodi list `result` object.
 * @arrname: the array member to read ("songs"/"episodes"/"movies"/…).
 *
 * Gathers the items of @result[@arrname] into a pointer array. The JsonObject
 * elements are **borrowed** from @result, which must outlive the returned array.
 *
 * @return a newly allocated GPtrArray of JsonObject* (no element free func);
 *         free with g_ptr_array_unref().
 */
static GPtrArray *
collect_items (JsonNode *result, const char *arrname)
{
  GPtrArray *out = g_ptr_array_new ();
  JsonObject *o = json_node_get_object (result);
  if (!json_object_has_member (o, arrname))
    return out;
  JsonArray *arr = json_object_get_array_member (o, arrname);
  guint n = json_array_get_length (arr);
  for (guint i = 0; i < n; i++)
    g_ptr_array_add (out, json_array_get_object_element (arr, i));
  return out;
}

/**
 * slice_items:
 * @items: the full pointer array.
 * @offset: rows to skip.
 * @limit: max rows to take.
 *
 * Returns the `[offset, offset+limit)` window of @items, used for app-side
 * paging of the multi-call music path. Elements are borrowed from @items.
 *
 * @return a newly allocated GPtrArray; free with g_ptr_array_unref().
 */
static GPtrArray *
slice_items (GPtrArray *items, gint64 offset, gint64 limit)
{
  GPtrArray *page = g_ptr_array_new ();
  for (gint64 i = offset; i >= 0 && i < (gint64) items->len
                          && i < offset + limit;
       i++)
    g_ptr_array_add (page, g_ptr_array_index (items, (guint) i));
  return page;
}

/**
 * filter_track:
 * @items: song items to filter.
 * @number: the track number to keep.
 *
 * Keeps only the songs whose `track` equals @number — the app-side track filter
 * used when a music `number` is given (Kodi's special id filters can't carry it).
 *
 * @return a newly allocated GPtrArray of the matching (borrowed) items; free
 *         with g_ptr_array_unref().
 */
static GPtrArray *
filter_track (GPtrArray *items, gint64 number)
{
  GPtrArray *out = g_ptr_array_new ();
  for (guint i = 0; i < items->len; i++)
    {
      JsonObject *it = g_ptr_array_index (items, i);
      if (json_object_get_int_member_with_default (it, "track", -1) == number)
        g_ptr_array_add (out, it);
    }
  return out;
}

/**
 * build_row:
 * @b: the builder, positioned inside the `rows` array.
 * @item: a Kodi leaf item.
 *
 * Emits one result row, copying every mk_search_row_fields member @item has.
 */
static void
build_row (JsonBuilder *b, JsonObject *item)
{
  json_builder_begin_object (b);
  for (gsize i = 0; mk_search_row_fields[i] != NULL; i++)
    copy_member (b, item, mk_search_row_fields[i]);
  json_builder_end_object (b);
}

/**
 * build_search_result:
 * @type: the searched media type ("music"/"tv-show"/"movie").
 * @res_key: resolved-container label key ("artist"/"show"), or NULL.
 * @res_label: the resolved container's display name, or NULL.
 * @res_idkey: resolved-container id key ("artistid"/"tvshowid"), or NULL.
 * @res_id: the resolved container's library id.
 * @have_resolved: whether to emit the `resolved` object.
 * @total: full match count (Kodi `limits.total`, or app-side length).
 * @offset: the paging offset echoed back.
 * @approximate: whether @total/paging are app-side estimates (multi-album).
 * @rows: leaf items to emit, or NULL; ignored when @count.
 * @count: when TRUE, emit zero rows (a count-only response).
 *
 * Builds the shared `search` response shape (§5.10): `{ type, total, returned,
 * offset, truncated, approximate?, resolved?, rows[] }`. `truncated` is true
 * when rows remain beyond this page (`offset + returned < total`).
 *
 * @return a newly allocated result object node; free with json_node_unref().
 */
static JsonNode *
build_search_result (const char *type, const char *res_key,
                     const char *res_label, const char *res_idkey,
                     gint64 res_id, gboolean have_resolved, gint64 total,
                     gint64 offset, gboolean approximate, GPtrArray *rows,
                     gboolean count)
{
  guint returned = (count || rows == NULL) ? 0 : rows->len;

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, type);
  json_builder_set_member_name (b, "total");
  json_builder_add_int_value (b, total);
  json_builder_set_member_name (b, "returned");
  json_builder_add_int_value (b, returned);
  json_builder_set_member_name (b, "offset");
  json_builder_add_int_value (b, offset);
  json_builder_set_member_name (b, "truncated");
  json_builder_add_boolean_value (b, offset + (gint64) returned < total);
  if (approximate)
    {
      json_builder_set_member_name (b, "approximate");
      json_builder_add_boolean_value (b, TRUE);
    }
  if (have_resolved)
    {
      json_builder_set_member_name (b, "resolved");
      json_builder_begin_object (b);
      if (res_label != NULL)
        {
          json_builder_set_member_name (b, res_key);
          json_builder_add_string_value (b, res_label);
        }
      json_builder_set_member_name (b, res_idkey);
      json_builder_add_int_value (b, res_id);
      json_builder_end_object (b);
    }

  json_builder_set_member_name (b, "rows");
  json_builder_begin_array (b);
  if (!count && rows != NULL)
    for (guint i = 0; i < rows->len; i++)
      build_row (b, g_ptr_array_index (rows, i));
  json_builder_end_array (b);

  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/**
 * resolve_container:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @method: the resolving list method (GetArtists/GetTVShows).
 * @field: the name field to match ("artist"/"title").
 * @value: the user's query (substring, case-insensitive).
 * @arrname: the result array member ("artists"/"tvshows").
 * @idkey: the id member to extract ("artistid"/"tvshowid").
 * @out_id: out; the resolved library id (0 when none).
 * @out_label: out; newly-allocated display name (NULL when none), or NULL to
 *             skip; free with g_free().
 * @error: return location for a GError, or NULL.
 *
 * Resolves a container name down to a single library id by a `contains` list
 * query, scanning up to MK_SEARCH_RESOLVE_SCAN candidates and preferring an
 * exact case-insensitive match over the first hit (so "Earth 2" wins over
 * "Earth: Final Conflict").
 *
 * @return 1 on a match (sets @out_id/@out_label), 0 when nothing matched, or -1
 *         with @error set on a call failure.
 */
static int
resolve_container (MkTools *self, const char *instance, const char *method,
                   const char *field, const char *value, const char *arrname,
                   const char *idkey, gint64 *out_id, char **out_label,
                   GError **error)
{
  *out_id = 0;
  if (out_label != NULL)
    *out_label = NULL;

  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  add_field_filter (pb, field, "contains", value);
  add_sort (pb, "label");
  add_limits (pb, 0, MK_SEARCH_RESOLVE_SCAN);
  json_builder_end_object (pb);
  g_autoptr (JsonNode) params = json_builder_get_root (pb);

  g_autoptr (JsonNode) result =
    mk_kodi_call (self->kodi, instance, method, params, error);
  if (result == NULL)
    return -1;

  JsonObject *o = json_node_get_object (result);
  if (!json_object_has_member (o, arrname))
    return 0;
  JsonArray *arr = json_object_get_array_member (o, arrname);
  guint n = json_array_get_length (arr);
  if (n == 0)
    return 0;

  gint best = -1;
  for (guint i = 0; i < n; i++)
    {
      JsonObject *it = json_array_get_object_element (arr, i);
      const char *lab = json_object_get_string_member_with_default (
        it, "label",
        json_object_get_string_member_with_default (it, "title", NULL));
      if (ci_equal (lab, value))
        {
          best = (gint) i;
          break;
        }
    }
  if (best < 0)
    best = 0;

  JsonObject *chosen = json_array_get_object_element (arr, (guint) best);
  *out_id = json_object_get_int_member_with_default (chosen, idkey, 0);
  if (out_label != NULL)
    {
      const char *lab = json_object_get_string_member_with_default (
        chosen, "label",
        json_object_get_string_member_with_default (chosen, "title", NULL));
      *out_label = g_strdup (lab);
    }
  return (*out_id != 0) ? 1 : 0;
}

/**
 * match_albums:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @artistid: the resolved artist.
 * @title: the album-title substring to match (case-insensitive).
 * @out_ids: out; appended with each matching `albumid` (gint64).
 * @error: return location for a GError, or NULL.
 *
 * Lists the artist's albums (`GetAlbums {artistid}`) and appends the ids of
 * those whose title contains @title. Matching is app-side because Kodi's special
 * `{artistid}` filter can't be combined with a title rule; an artist's album
 * count is small (tens), so this is one cheap call.
 *
 * @return TRUE on success (even with zero matches), FALSE with @error set on a
 *         call failure.
 */
static gboolean
match_albums (MkTools *self, const char *instance, gint64 artistid,
              const char *title, GArray *out_ids, GError **error)
{
  static const char *const props[] = { "title", NULL };

  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  add_id_filter (pb, "artistid", artistid);
  add_properties (pb, props);
  add_sort (pb, "label");
  json_builder_end_object (pb);
  g_autoptr (JsonNode) params = json_builder_get_root (pb);

  g_autoptr (JsonNode) result =
    mk_kodi_call (self->kodi, instance, "AudioLibrary.GetAlbums", params, error);
  if (result == NULL)
    return FALSE;

  JsonObject *o = json_node_get_object (result);
  if (!json_object_has_member (o, "albums"))
    return TRUE;
  JsonArray *arr = json_object_get_array_member (o, "albums");
  guint n = json_array_get_length (arr);
  for (guint i = 0; i < n; i++)
    {
      JsonObject *it = json_array_get_object_element (arr, i);
      const char *t = json_object_get_string_member_with_default (
        it, "title",
        json_object_get_string_member_with_default (it, "label", NULL));
      if (ci_contains (t, title))
        {
          gint64 id = json_object_get_int_member_with_default (it, "albumid", 0);
          if (id != 0)
            g_array_append_val (out_ids, id);
        }
    }
  return TRUE;
}

/**
 * query_songs:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @idkey: the special filter key ("artistid" or "albumid").
 * @id: the library id to filter by.
 * @paged: when TRUE, apply the @offset/@limit/@count window; when FALSE, fetch
 *         all songs (for the app-side collected paths).
 * @offset: paging offset (used when @paged).
 * @limit: paging limit (used when @paged).
 * @count: count-only request (used when @paged).
 * @error: return location for a GError, or NULL.
 *
 * Runs `AudioLibrary.GetSongs` filtered by @idkey/@id, sorted by track, with the
 * shared song properties.
 *
 * @return the Kodi `result` node (free with json_node_unref()), or NULL with
 *         @error set.
 */
static JsonNode *
query_songs (MkTools *self, const char *instance, const char *idkey, gint64 id,
             gboolean paged, gint64 offset, gint64 limit, gboolean count,
             GError **error)
{
  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  add_id_filter (pb, idkey, id);
  add_properties (pb, mk_search_song_props);
  add_sort (pb, "track");
  if (paged)
    add_page_limits (pb, offset, limit, count);
  json_builder_end_object (pb);
  g_autoptr (JsonNode) params = json_builder_get_root (pb);

  return mk_kodi_call (self->kodi, instance, "AudioLibrary.GetSongs", params,
                       error);
}

/**
 * search_movie:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @title: movie-title substring, or NULL/"" for all movies.
 * @limit: max rows.
 * @offset: rows to skip.
 * @count: count-only request.
 * @error: return location for a GError, or NULL.
 *
 * Resolves the movie type directly: `VideoLibrary.GetMovies` returns the
 * playable `file` with no sublevels, so this is one paged call (§12.16.12).
 *
 * @return the search-result node, or NULL with @error set.
 */
static JsonNode *
search_movie (MkTools *self, const char *instance, const char *title,
              gint64 limit, gint64 offset, gboolean count, GError **error)
{
  static const char *const props[] = { "title",   "year",      "genre",
                                       "rating",  "runtime",   "playcount",
                                       "file",    NULL };

  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  if (title != NULL && title[0] != '\0')
    add_field_filter (pb, "title", "contains", title);
  add_properties (pb, props);
  add_sort (pb, "title");
  add_page_limits (pb, offset, limit, count);
  json_builder_end_object (pb);
  g_autoptr (JsonNode) params = json_builder_get_root (pb);

  g_autoptr (JsonNode) result =
    mk_kodi_call (self->kodi, instance, "VideoLibrary.GetMovies", params, error);
  if (result == NULL)
    return NULL;

  gint64 total = list_total (result);
  g_autoptr (GPtrArray) rows = count ? NULL : collect_items (result, "movies");
  return build_search_result ("movie", NULL, NULL, NULL, 0, FALSE, total, offset,
                              FALSE, rows, count);
}

/**
 * search_tv:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @title: show-name substring (required).
 * @have_season: whether @season was given.
 * @season: season number to narrow to.
 * @have_number: whether @number was given.
 * @number: episode number to pin.
 * @limit: max rows.
 * @offset: rows to skip.
 * @count: count-only request.
 * @error: return location for a GError, or NULL.
 *
 * Resolves the show (`GetTVShows`) then drills to episode leaves
 * (`GetEpisodes {tvshowid, season?}` plus an exact `episode` filter when a
 * number is given — its value is a string, §12.16.21/§12.16.6). One paged call
 * past the resolve.
 *
 * @return the search-result node, or NULL with @error set (missing title or a
 *         call failure); an unresolved show yields a clean zero-total result.
 */
static JsonNode *
search_tv (MkTools *self, const char *instance, const char *title,
           gboolean have_season, gint64 season, gboolean have_number,
           gint64 number, gint64 limit, gint64 offset, gboolean count,
           GError **error)
{
  static const char *const props[] = { "title",      "season",  "episode",
                                       "file",       "firstaired",
                                       "runtime",    NULL };

  if (title == NULL || title[0] == '\0')
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "search tv-show: \"title\" (show name) is required");
      return NULL;
    }

  gint64 showid = 0;
  g_autofree char *show = NULL;
  int r = resolve_container (self, instance, "VideoLibrary.GetTVShows", "title",
                             title, "tvshows", "tvshowid", &showid, &show, error);
  if (r < 0)
    return NULL;
  if (r == 0)
    return build_search_result ("tv-show", NULL, NULL, NULL, 0, FALSE, 0, offset,
                                FALSE, NULL, count);

  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  json_builder_set_member_name (pb, "tvshowid");
  json_builder_add_int_value (pb, showid);
  if (have_season)
    {
      json_builder_set_member_name (pb, "season");
      json_builder_add_int_value (pb, season);
    }
  if (have_number)
    {
      g_autofree char *ns = g_strdup_printf ("%" G_GINT64_FORMAT, number);
      add_field_filter (pb, "episode", "is", ns);
    }
  add_properties (pb, props);
  add_sort (pb, "episode");
  add_page_limits (pb, offset, limit, count);
  json_builder_end_object (pb);
  g_autoptr (JsonNode) params = json_builder_get_root (pb);

  g_autoptr (JsonNode) result = mk_kodi_call (self->kodi, instance,
                                              "VideoLibrary.GetEpisodes", params,
                                              error);
  if (result == NULL)
    return NULL;

  gint64 total = list_total (result);
  g_autoptr (GPtrArray) rows = count ? NULL : collect_items (result, "episodes");
  return build_search_result ("tv-show", "show", show, "tvshowid", showid, TRUE,
                              total, offset, FALSE, rows, count);
}

/**
 * search_music:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @artist: performer substring (required).
 * @title: album-name substring, or NULL/"" to span every album.
 * @have_number: whether @number (track) was given.
 * @number: track number to keep.
 * @limit: max rows.
 * @offset: rows to skip.
 * @count: count-only request.
 * @error: return location for a GError, or NULL.
 *
 * Resolves the artist, then drills to song leaves. The wide paths — artist-only,
 * or a single matched album, with no track filter — are one paged
 * `GetSongs {artistid|albumid}` call, so `total`/`limit`/`offset` come straight
 * from Kodi. A track filter, or an album substring matching several albums,
 * falls back to fetching the songs and paging app-side; the multi-album case is
 * flagged `approximate` (§5.10).
 *
 * @return the search-result node, or NULL with @error set (missing artist or a
 *         call failure); an unresolved artist yields a clean zero-total result.
 */
static JsonNode *
search_music (MkTools *self, const char *instance, const char *artist,
              const char *title, gboolean have_number, gint64 number,
              gint64 limit, gint64 offset, gboolean count, GError **error)
{
  if (artist == NULL || artist[0] == '\0')
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "search music: \"artist\" is required");
      return NULL;
    }

  gint64 artistid = 0;
  g_autofree char *aname = NULL;
  int r = resolve_container (self, instance, "AudioLibrary.GetArtists", "artist",
                             artist, "artists", "artistid", &artistid, &aname,
                             error);
  if (r < 0)
    return NULL;
  if (r == 0)
    return build_search_result ("music", NULL, NULL, NULL, 0, FALSE, 0, offset,
                                FALSE, NULL, count);

  gboolean have_album = (title != NULL && title[0] != '\0');

  /* Wide single-call path: artist-only or one album, no track filter. */
  if (!have_number && !have_album)
    {
      g_autoptr (JsonNode) result = query_songs (self, instance, "artistid",
                                                 artistid, TRUE, offset, limit,
                                                 count, error);
      if (result == NULL)
        return NULL;
      gint64 total = list_total (result);
      g_autoptr (GPtrArray) rows = count ? NULL : collect_items (result, "songs");
      return build_search_result ("music", "artist", aname, "artistid", artistid,
                                  TRUE, total, offset, FALSE, rows, count);
    }

  /* Resolve the named album(s). */
  g_autoptr (GArray) albumids = g_array_new (FALSE, FALSE, sizeof (gint64));
  if (have_album)
    {
      if (!match_albums (self, instance, artistid, title, albumids, error))
        return NULL;
      if (albumids->len == 0)
        return build_search_result ("music", "artist", aname, "artistid",
                                    artistid, TRUE, 0, offset, FALSE, NULL,
                                    count);
    }

  if (have_album && albumids->len == 1 && !have_number)
    {
      gint64 albumid = g_array_index (albumids, gint64, 0);
      g_autoptr (JsonNode) result = query_songs (self, instance, "albumid",
                                                 albumid, TRUE, offset, limit,
                                                 count, error);
      if (result == NULL)
        return NULL;
      gint64 total = list_total (result);
      g_autoptr (GPtrArray) rows = count ? NULL : collect_items (result, "songs");
      return build_search_result ("music", "artist", aname, "artistid", artistid,
                                  TRUE, total, offset, FALSE, rows, count);
    }

  /* App-side collected path: a track filter, or several matched albums. Fetch
   * the candidate songs, optionally keep one track, then page over the set. */
  gboolean approximate = (have_album && albumids->len > 1);
  g_autoptr (GPtrArray) results =
    g_ptr_array_new_with_free_func ((GDestroyNotify) json_node_unref);
  g_autoptr (GPtrArray) all = g_ptr_array_new ();

  if (have_album)
    {
      for (guint i = 0; i < albumids->len; i++)
        {
          gint64 albumid = g_array_index (albumids, gint64, i);
          JsonNode *res = query_songs (self, instance, "albumid", albumid, FALSE,
                                       0, 0, FALSE, error);
          if (res == NULL)
            return NULL;
          g_ptr_array_add (results, res);
          g_autoptr (GPtrArray) items = collect_items (res, "songs");
          for (guint j = 0; j < items->len; j++)
            g_ptr_array_add (all, g_ptr_array_index (items, j));
        }
    }
  else
    {
      JsonNode *res = query_songs (self, instance, "artistid", artistid, FALSE, 0,
                                   0, FALSE, error);
      if (res == NULL)
        return NULL;
      g_ptr_array_add (results, res);
      g_autoptr (GPtrArray) items = collect_items (res, "songs");
      for (guint j = 0; j < items->len; j++)
        g_ptr_array_add (all, g_ptr_array_index (items, j));
    }

  GPtrArray *eff = all;
  g_autoptr (GPtrArray) filtered = NULL;
  if (have_number)
    {
      filtered = filter_track (all, number);
      eff = filtered;
    }

  gint64 total = eff->len;
  g_autoptr (GPtrArray) page = count ? NULL : slice_items (eff, offset, limit);
  return build_search_result ("music", "artist", aname, "artistid", artistid,
                              TRUE, total, offset, approximate, page, count);
}

/**
 * schema_search:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `search` tool (§11.6.4.1): the `type` selector plus the
 * optional drill fields and paging controls.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_search (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);
  static const char *const types[] = { "music", "tv-show", "movie", NULL };
  prop_enum (b, "type", types,
             "Media kind to search: music | tv-show | movie.");
  prop_typed (b, "artist", "string",
              "Music only: performer name (substring, case-insensitive). "
              "Resolves the artist; required for music.");
  prop_typed (b, "title", "string",
              "Container/title to match (substring, case-insensitive): album "
              "for music, show for tv-show, the movie title for movie. Required "
              "for tv-show.");
  prop_typed (b, "season", "integer",
              "tv-show only: season number to narrow the episodes.");
  prop_typed (b, "number", "integer",
              "Position within the container: track number (music) or episode "
              "number (tv-show).");
  prop_typed (b, "limit", "integer",
              "Max leaf rows to return (default 50, max 500). Page with offset.");
  prop_typed (b, "offset", "integer",
              "Number of leaf rows to skip — paginate together with limit.");
  prop_typed (b, "count", "boolean",
              "When true, return only the total match count (zero rows) — a "
              "cheap count.");

  static const char *const required[] = { "type", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * handler_search:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; `type` selects the per-type chain.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `search` tool (§11.6.4.1): reads the drill fields and paging
 * controls, clamps `limit`/`offset`, and dispatches to the per-type resolver.
 * Makes no Kodi state change.
 *
 * @return the search-result node, or NULL with @error set (bad arguments or a
 *         call failure).
 */
static JsonNode *
handler_search (MkTools *self, const MkToolDef *def, JsonObject *args,
                GError **error)
{
  (void) def;
  const char *instance = arg_instance (args);
  const char *type = arg_str (args, "type", NULL);
  if (type == NULL || type[0] == '\0')
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "search: \"type\" is required (music/tv-show/movie)");
      return NULL;
    }

  const char *title = arg_str (args, "title", NULL);
  const char *artist = arg_str (args, "artist", NULL);
  gint64 season = 0, number = 0, v = 0;
  gboolean have_season = arg_int (args, "season", &season);
  gboolean have_number = arg_int (args, "number", &number);

  gint64 limit = MK_SEARCH_DEFAULT_LIMIT, offset = 0;
  if (arg_int (args, "limit", &v))
    limit = v;
  if (arg_int (args, "offset", &v))
    offset = v;
  limit = CLAMP (limit, 0, MK_SEARCH_MAX_LIMIT);
  if (offset < 0)
    offset = 0;
  gboolean count = arg_bool (args, "count");

  if (g_str_equal (type, "movie"))
    return search_movie (self, instance, title, limit, offset, count, error);
  if (g_str_equal (type, "tv-show"))
    return search_tv (self, instance, title, have_season, season, have_number,
                      number, limit, offset, count, error);
  if (g_str_equal (type, "music"))
    return search_music (self, instance, artist, title, have_number, number,
                         limit, offset, count, error);

  g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
               "search: unknown type \"%s\" (use music/tv-show/movie)", type);
  return NULL;
}

/* ---- playfile (§11.6.5) ---------------------------------------------------
 *
 * Play one file by path. The natural partner to `search` (§5.10): the caller
 * picks a `file` out of a multi-leaf search result and hands it here. Unlike the
 * library `play` (by id), this opens an arbitrary path, so it works for items
 * that are not in the library at all.
 */

#define MK_PLAYFILE_SETTLE_US (500 * 1000) /* let the new player start first */

/**
 * schema_playfile:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `playfile` tool (§11.6.5): the required `file` path plus the
 * optional `instance`.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_playfile (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);
  prop_typed (b, "file", "string",
              "Path of the file to play — the `file` field of a `search` result "
              "row. Any path Kodi can reach works, in-library or not.");

  static const char *const required[] = { "file", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * handler_playfile:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; `file` is the path to play.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `playfile` tool (§11.6.5): opens one file by path with
 * `Player.Open { "item": { "file": <file> } }` (§12.10.9). Kodi auto-selects the
 * audio vs video player from the file type and, when the path matches a scanned
 * item, enriches the now-playing state back to the library id. Like the
 * transport Buttons, it then returns the player_state() snapshot rather than
 * Kodi's bare "OK", so the caller sees what is now loaded and playing.
 *
 * `Player.Open` replies "OK" the moment the open is queued, before the new
 * player has started and its `speed`/item settle, so — as with handler_button()
 * — a brief settle delay before snapshotting makes the reported state reflect
 * the file that is now playing.
 *
 * @return the post-open player-state object, or NULL with @error set (missing
 *         `file` or a call failure).
 */
static JsonNode *
handler_playfile (MkTools *self, const MkToolDef *def, JsonObject *args,
                  GError **error)
{
  (void) def;
  const char *instance = arg_instance (args);
  const char *file = arg_str (args, "file", NULL);
  if (file == NULL || file[0] == '\0')
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "playfile: \"file\" is required");
      return NULL;
    }

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "item");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "file");
  json_builder_add_string_value (b, file);
  json_builder_end_object (b);
  json_builder_end_object (b);
  g_autoptr (JsonNode) params = json_builder_get_root (b);

  g_autoptr (JsonNode) opened = mk_kodi_call (self->kodi, instance, "Player.Open",
                                              params, error);
  if (opened == NULL)
    return NULL;

  g_usleep (MK_PLAYFILE_SETTLE_US);
  return player_state (self, instance, error);
}

/* ---- rpc (§11.6.6) --------------------------------------------------------
 *
 * The escape hatch: send any JSON-RPC method to a box and return Kodi's raw
 * reply unshaped, for anything §5/§11.6 don't model. Powerful and unconstrained,
 * so it is gated per instance behind the hand-edit-only `allow_rpc` flag (§7.7):
 * an instance that has not opted in refuses the call before it reaches Kodi, and
 * the flag can never be set through the `instances` tool — only the model's
 * operator can grant it.
 */

/**
 * schema_rpc:
 * @self: the table (used to name the instances that permit the hatch).
 *
 * Schema for the `rpc` tool (§11.6.6): the required `method`, an optional
 * `params` object, and the optional `instance`. The `method` description names
 * which configured instances currently permit `rpc` (those with `allow_rpc`),
 * so the caller knows where it can be used without trial and error.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_rpc (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);

  /* List the boxes that have opted in (§7.7), so the schema documents where the
   * hatch is live. */
  g_autoptr (GString) allowed = g_string_new (NULL);
  GList *names = mk_config_instance_names (self->config);
  for (GList *l = names; l != NULL; l = l->next)
    {
      MkInstance *inst = mk_config_get_instance (self->config, l->data);
      if (inst != NULL && inst->allow_rpc)
        {
          if (allowed->len > 0)
            g_string_append (allowed, ", ");
          g_string_append (allowed, l->data);
        }
    }
  g_list_free (names);

  g_autoptr (GString) mdesc = g_string_new (
    "JSON-RPC method to invoke verbatim, e.g. \"GUI.ActivateWindow\" or "
    "\"Input.Up\". Returns Kodi's raw result unchanged. Escape hatch for methods "
    "the dedicated tools don't model — disabled unless the target instance has "
    "opted in (\"allow_rpc\" in the server config, set by hand only). ");
  if (allowed->len > 0)
    g_string_append_printf (mdesc, "Currently permitted on: %s.", allowed->str);
  else
    g_string_append (mdesc, "No configured instance currently permits it.");
  prop_typed (b, "method", "string", mdesc->str);

  prop_typed (b, "params", "object",
              "Parameters object passed straight through to the method. Omit for "
              "a method that takes none.");

  static const char *const required[] = { "method", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * handler_rpc:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; `method` and optional `params`/`instance`.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `rpc` escape hatch (§11.6.6): builds the JSON-RPC envelope and
 * POSTs it to the resolved instance (§4.1/§4.2), returning Kodi's `result`
 * **verbatim** — no shaping, unlike every other tool. A Kodi `error` surfaces as
 * a tool error (§3.4/§4.5).
 *
 * Gated per instance (§7.7): unless the target instance's config sets
 * `allow_rpc: true` the call is refused here, before any Kodi request, with a
 * clean tool error naming the instance and the flag. The gate is hand-edit-only
 * — it cannot be flipped through the `instances` tool — so the model can never
 * grant itself the hatch. An instance that is not configured at all is left to
 * mk_kodi_call(), which reports the usual no-instance comms error.
 *
 * @return Kodi's raw result node, or NULL with @error set (missing `method`,
 *         the hatch disabled, ill-typed `params`, or a call failure).
 */
static JsonNode *
handler_rpc (MkTools *self, const MkToolDef *def, JsonObject *args,
             GError **error)
{
  (void) def;
  const char *instance = arg_instance (args);
  const char *method = arg_str (args, "method", NULL);
  if (method == NULL || method[0] == '\0')
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "rpc: \"method\" is required");
      return NULL;
    }

  /* The escape-hatch gate (§7.7): a configured instance must have opted in. An
   * unconfigured instance falls through to mk_kodi_call()'s no-instance error. */
  MkInstance *inst = mk_config_get_instance (self->config, instance);
  if (inst != NULL && !inst->allow_rpc)
    {
      const char *name = instance ? instance
                                  : mk_config_get_default (self->config);
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "rpc: the escape hatch is disabled for instance \"%s\". "
                   "Enable it by setting \"allow_rpc\": true on that instance in "
                   "the server config (§7.7); it cannot be enabled via the "
                   "instances tool.",
                   name ? name : "(default)");
      return NULL;
    }

  /* Optional params: pass the object straight through (borrowed). An explicit
   * null or an absent member means "no params"; any other type is a usage
   * error. */
  JsonNode *params = NULL;
  if (args != NULL && json_object_has_member (args, "params"))
    {
      JsonNode *p = json_object_get_member (args, "params");
      if (p != NULL && JSON_NODE_HOLDS_OBJECT (p))
        params = p;
      else if (p != NULL && !JSON_NODE_HOLDS_NULL (p))
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "rpc: \"params\" must be an object");
          return NULL;
        }
    }

  return mk_kodi_call (self->kodi, instance, method, params, error);
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

  /* ---- Search tools — resolve the library by name down to playable leaf
   *      files; no Kodi state change (§11.6.4). ---- */

  /**
   * search (Search tool):
   *
   * Find playable leaf files by name across the three media types (§11.6.4.1,
   * full design §5.10). Drills each type's hierarchy to leaf rows, descending to
   * the album/season level only when the user names one — so a wide query (bare
   * artist, bare show) is a single Kodi call:
   *
   *   - music:   GetArtists(artist) → GetSongs {artistid} directly, or
   *              → GetAlbums(+title) → GetSongs {albumid} when an album is named.
   *   - tv-show: GetTVShows(title) → GetEpisodes {tvshowid, season?} (+ episode
   *              number filter).
   *   - movie:   GetMovies(title) → file directly (no sublevels).
   *
   * Always reports `total` (Kodi's full match count); `count: true` returns it
   * with zero rows. `limit`/`offset` page the leaves; the default cap (50)
   * applies when `limit` is omitted, flagging `truncated`. A substring album
   * `title` matching several albums is collected app-side and flagged
   * `approximate`.
   *
   * @param type (required): "music" | "tv-show" | "movie".
   * @param artist: music performer (substring); required for music.
   * @param title: album/show/movie name (substring); required for tv-show.
   * @param season|number: narrow tv episodes / pin a track or episode.
   * @param limit|offset|count: paging and count-only controls.
   * @param instance (optional): the Kodi instance whose library to search; omitted
   *        uses the configured default (§5.1).
   * @return `{ "type", "total", "returned", "offset", "truncated",
   *         "approximate"?, "resolved"?, "rows": [ { "file", "<id>", "label",
   *         "title", … } ] }`. Each row's `file` is the `playfile` input (§11.6.5).
   */
  { "search", "Find playable files by name: music/tv-show/movie, drilled to "
              "leaf files with paging (limit/offset) and a total count.",
    schema_search, handler_search, NULL },

  /* ---- Playback tools — open content on a player (§11.6.5). ---- */

  /**
   * playfile (Playback tool):
   *
   * Play one file by path — the partner to `search` (§11.6.5, design §5.10).
   * Takes a `file` (typically the `file` field of a `search` result row) and
   * opens it with Player.Open `{ "item": { "file": <file> } }` (§12.10.9). Kodi
   * auto-selects the audio vs video player from the file type and, when the path
   * matches a scanned library item, enriches the now-playing state back to its
   * library id. Plays a single file; for a multi-leaf `search` result the caller
   * picks which row to play. Works for any path Kodi can reach, in-library or
   * not.
   *
   * Call:  Player.Open { "item": { "file": <file> } }, then player_state() to
   *        report what is now loaded/playing.
   * @param file (required): path of the file to play.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the resulting player-state snapshot (player_state()): `{ "state":
   *         "playing"|"paused"|"stopped", "type", "file", "label", "title",
   *         "time", "totaltime" }` (fields present when a player is active).
   */
  { "playfile", "Play one file by path (e.g. a `search` result's `file`): "
                "Player.Open auto-selects the audio/video player. Works for any "
                "reachable path, in-library or not.",
    schema_playfile, handler_playfile, NULL },

  /* ---- Escape hatch — raw JSON-RPC passthrough, opt-in per instance
   *      (§11.6.6, gate §7.7). ---- */

  /**
   * rpc (Escape hatch):
   *
   * Send any JSON-RPC method to a Kodi box and get its reply back unchanged —
   * the catch-all for anything the dedicated tools don't model (§11.6.6, §2.3).
   * Builds the JSON-RPC envelope and POSTs it to the target instance, returning
   * Kodi's raw `result` verbatim; a Kodi error becomes a tool error.
   *
   * Powerful and unconstrained, so it is OFF by default and gated per instance:
   * a box permits it only when its server config sets `allow_rpc: true` (§7.7),
   * which is set by hand-editing the config file and cannot be enabled through
   * the `instances` tool. Calling `rpc` on an instance that has not opted in
   * returns a clean tool error and makes no Kodi request.
   *
   * Call:  <method> with the given <params>, returning Kodi's raw result.
   * @param method (required): the JSON-RPC method, e.g. "GUI.ActivateWindow".
   * @param params: parameters object passed straight through (optional).
   * @param instance (optional): the Kodi instance to target; omitted uses the
   *        configured default (§5.1). The instance must have `allow_rpc` set.
   * @return Kodi's raw `result` for the method, unshaped. On a disabled instance
   *         or a Kodi error, an `isError` result with the detail instead.
   */
  { "rpc", "Escape hatch: send a raw JSON-RPC method to Kodi and return its "
           "reply unchanged. Disabled unless the target instance has opted in "
           "(allow_rpc in the server config, set by hand only).",
    schema_rpc, handler_rpc, NULL },
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
