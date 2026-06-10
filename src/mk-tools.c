/* mcp-kodi — tool table: schemas + handlers. See mk-tools.h and ../TODO.md §5.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 */

#include "config.h"

#include "mk-tools.h"

#include "mk-history.h"

struct _MkTools
{
  MkConfig  *config;  /* borrowed; must outlive the table */
  MkKodi    *kodi;    /* borrowed; must outlive the table */
  MkHistory *history; /* owned; the playback-history log (§13) */
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
           * e.g. `living-room ("Living Room TV")`, when the instance has one. */
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
 * copy_member_nonempty:
 * @b: the builder, positioned inside an object.
 * @src: the source object to copy from, or NULL.
 * @name: the member to copy.
 *
 * Like copy_member(), but skips values that carry no information: an empty
 * string, an empty array, or a negative integer. Kodi returns every requested
 * `Player.GetItem` property regardless of media type, filling the inapplicable
 * ones with sentinels — `""`, `[]`, and `-1` (confirmed live: an off-library
 * file reports `season`/`episode`/`track` as -1 and `showtitle`/`album` as ""
 * with `type:"unknown"`, §13.4.1). This omits exactly those, so the snapshot
 * carries a per-media field only where it applies (§13.4 "omit what's empty").
 * A zero is kept on purpose — `season` 0 is the legitimate "specials" season.
 */
static void
copy_member_nonempty (JsonBuilder *b, JsonObject *src, const char *name)
{
  if (src == NULL || !json_object_has_member (src, name))
    return;
  JsonNode *node = json_object_get_member (src, name);
  if (JSON_NODE_HOLDS_VALUE (node))
    {
      GType vt = json_node_get_value_type (node);
      if (vt == G_TYPE_STRING)
        {
          const char *s = json_node_get_string (node);
          if (s == NULL || *s == '\0')
            return;
        }
      else if (vt == G_TYPE_INT64 && json_node_get_int (node) < 0)
        return;
    }
  else if (JSON_NODE_HOLDS_ARRAY (node)
           && json_array_get_length (json_node_get_array (node)) == 0)
    return;
  json_builder_set_member_name (b, name);
  json_builder_add_value (b, json_node_copy (node));
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
 * This snapshot is also the **raw material the playback-history log reuses**
 * (§13.3): when the write path lands (§13.9) the history record is composed
 * straight from this object plus the instance key and a capture timestamp, with
 * **no extra Kodi round-trip** beyond the calls below. The one exception is the
 * `rpc` escape hatch, which takes no snapshot of its own and so will issue one
 * post-call purely to feed history (§13.3.1).
 *
 * To be that single source of truth, the `Player.GetItem` call is widened
 * (§13.4.3.1) to also fetch the per-media identifiers history records (§13.4.2):
 * `showtitle`/`season`/`episode` for an episode, `album`/`artist`/`track` for a
 * song — in the **same call**, no extra round-trip. Each is surfaced only where
 * it applies (copy_member_nonempty drops the `""`/`[]`/`-1` sentinels Kodi
 * returns for the other media types), so a movie carries none of them and every
 * status/transport reply reads richer ("playing S01E02 of …") for free. The
 * real media type and library id (`media`/`id`, §13.4.1) are surfaced too,
 * under keys distinct from `type` (which is spent on the player kind): both are
 * identity fields Kodi auto-injects into the same GetItem reply, so they cost
 * nothing extra. An off-library file reports `media` "unknown" and `id` -1
 * (§13.4.1.1); both are kept as-is rather than dropped, so the history log can
 * record an item as unenriched.
 *
 * There is no single Kodi method for it, so it combines three calls:
 * `Player.GetActivePlayers` to find the active player, then
 * `Player.GetProperties` (speed/time/totaltime) and `Player.GetItem`
 * (file/label/title) on it (§5.4). Progress is reported as `time`/`totaltime`
 * only; a percentage is just `time / totaltime` and is left for the caller to
 * compute rather than returned redundantly.
 *
 * @return a newly allocated object node — `{ "state": "stopped" }` when nothing
 *         is active, else `{ "state": "playing"|"paused", "type"?, "media"?,
 *         "id"?, "file"?, "label"?, "title"?, "showtitle"?, "season"?,
 *         "episode"?, "album"?, "artist"?, "track"?, "time"?, "totaltime"? }`
 *         (`media`/`id` whenever an item is loaded, §13.4.1; the per-media
 *         fields present only where they apply, §13.4.2) — or NULL with @error
 *         set if a call fails. Example, a video paused 15:38 into 46:40 (≈33%):
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

  static const char *const item_fields[] = { "title", "file",
                                             /* §13.4.2 enrichment (§13.4.3.1):
                                              * episode and song identifiers,
                                              * fetched in this same call. */
                                             "showtitle", "season", "episode",
                                             "album", "artist", "track", NULL };
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
  /* §13.4.1: the real media type and library id, surfaced under keys distinct
   * from `type` (which the snapshot spends on the player kind above). Both are
   * identity fields Kodi auto-injects into every Player.GetItem reply — free,
   * not requestable properties — so player_state() now carries them too: status
   * and transport replies read richer ("episode 4838"), and the history log
   * (§13.9) copies them straight from this snapshot. An off-library file (a
   * `playfile` of a path absent from the library) reports media "unknown" and
   * id -1 (§13.4.1.1); both are recorded as-is so the log can mark it
   * unenriched, so unlike the §13.4.2 fields these are not dropped when empty. */
  if (json_object_has_member (item, "type"))
    {
      json_builder_set_member_name (b, "media");
      json_builder_add_string_value (
        b, json_object_get_string_member_with_default (item, "type", "unknown"));
    }
  if (json_object_has_member (item, "id"))
    {
      json_builder_set_member_name (b, "id");
      json_builder_add_int_value (
        b, json_object_get_int_member_with_default (item, "id", -1));
    }
  copy_member (b, item, "file");
  copy_member (b, item, "label");
  copy_member (b, item, "title");
  /* §13.4.2 per-media enrichment: present only where it applies — empties and
   * -1 sentinels are dropped (copy_member_nonempty). */
  copy_member_nonempty (b, item, "showtitle");
  copy_member_nonempty (b, item, "season");
  copy_member_nonempty (b, item, "episode");
  copy_member_nonempty (b, item, "album");
  copy_member_nonempty (b, item, "artist");
  copy_member_nonempty (b, item, "track");
  copy_member (b, props, "time");
  copy_member (b, props, "totaltime");
  json_builder_end_object (b);
  JsonNode *root = json_builder_get_root (b);

  /* §13.3/§13.9.1: feed the playback-history log from this very snapshot — the
   * single point every playback-affecting tool funnels through, so it is the
   * one place history needs to hook. Best-effort and dedup'd (§13.5/§13.9.2):
   * status/noop re-observing the same item add nothing, and a logging failure
   * never fails this call. The stored `instance` is the resolved config key
   * (never NULL) with the box's display label as `name`. */
  if (self->history != NULL)
    {
      const char *key = instance ? instance : mk_config_get_default (self->config);
      MkInstance *inst = mk_config_get_instance (self->config, key);
      mk_history_record (self->history, key, inst ? inst->name : NULL, root);
    }

  return root;
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
/* Kodi's Application volume is a fixed 0–100 percentage, and the API exposes no
 * way to query the scale, so the server reports the bounds as constants in the
 * volume snapshot (§11.6.2.1) — letting the AI nudge relatively without
 * hardcoding the range or guessing when a step would be clamped. */
#define MK_VOLUME_MIN 0
#define MK_VOLUME_MAX 100

/**
 * app_audio_read:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @volume: return location for the current volume (0–100), or NULL.
 * @muted: return location for the current mute flag, or NULL.
 * @error: return location for a GError, or NULL.
 *
 * Reads the application audio state with a single `Application.GetProperties`
 * for `volume` and `muted` — the one read every audio tool funnels through, so
 * a snapshot reflects Kodi's settled state rather than a setter's bare reply.
 *
 * @return TRUE and fills the requested out-params, or FALSE with @error set.
 */
static gboolean
app_audio_read (MkTools *self, const char *instance, gint64 *volume,
                gboolean *muted, GError **error)
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
    return FALSE;
  JsonObject *props = json_node_get_object (res);
  if (volume != NULL)
    *volume = json_object_get_int_member_with_default (props, "volume", 0);
  if (muted != NULL)
    *muted = json_object_get_boolean_member_with_default (props, "muted", FALSE);
  return TRUE;
}

/**
 * audio_snapshot:
 * @volume: the volume to report (0–100).
 * @muted: the mute flag to report.
 * @with_bounds: also emit the fixed `min`/`max` volume bounds (§11.6.2.1).
 *
 * Builds the shared audio response object `{ "muted": <bool>, "volume": <int>
 * }`, optionally extended with `"min"`/`"max"` for the volume Knob so the AI
 * sees where it landed and how much room remains.
 *
 * @return a newly allocated object node.
 */
static JsonNode *
audio_snapshot (gint64 volume, gboolean muted, gboolean with_bounds)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "muted");
  json_builder_add_boolean_value (b, muted);
  json_builder_set_member_name (b, "volume");
  json_builder_add_int_value (b, volume);
  if (with_bounds)
    {
      json_builder_set_member_name (b, "min");
      json_builder_add_int_value (b, MK_VOLUME_MIN);
      json_builder_set_member_name (b, "max");
      json_builder_add_int_value (b, MK_VOLUME_MAX);
    }
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/**
 * app_audio_state:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @error: return location for a GError, or NULL.
 *
 * The shared audio snapshot `{ "muted": <bool>, "volume": <int> }` returned by
 * mute/unmute (§11.6.1): read the settled state, shape it. The volume Knob
 * (§11.6.2.1) uses app_audio_read()/audio_snapshot() directly so it can also
 * report the bounds and avoid a redundant round-trip.
 *
 * @return a newly allocated object node, or NULL with @error set.
 */
static JsonNode *
app_audio_state (MkTools *self, const char *instance, GError **error)
{
  gint64 volume = 0;
  gboolean muted = FALSE;
  if (!app_audio_read (self, instance, &volume, &muted, error))
    return NULL;
  return audio_snapshot (volume, muted, FALSE);
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
 * handler_stop:
 * @self: the tool table.
 * @def: this Button's table row; @def->action is "stop".
 * @args: the call arguments (optional `instance`), or NULL.
 * @error: return location for a GError, or NULL.
 *
 * Implements the stop Button (§11.6.1.3): the handler_button() keypress plus an
 * automatic playlist clear. Kodi's stop ends *playback* but leaves the *queue*
 * intact (confirmed live: a stopped 2-item playlist still lists both entries),
 * so a bare stop would strand stale items that only `dropplaylists` could
 * remove. To the caller "stop" means "done with this", so the playlist the
 * player was consuming is cleared too:
 *
 *   1. Resolve the active player's `playlistid` — *before* the keypress, since
 *      afterwards there is no active player left to ask. No active player, or
 *      non-playlist playback (`playlistid < 0`, live TV / streams): nothing to
 *      clear, the keypress alone suffices.
 *   2. `Input.ExecuteAction { "action": "stop" }`, then the settle delay
 *      (handler_button()).
 *   3. `Playlist.Clear { playlistid }` on the playlist from step 1.
 *
 * Other playlists are left alone — clearing *everything* is `dropplaylists`'
 * job (§11.6.9); stop touches only what it stopped.
 *
 * @return the post-action player-state object (`{ "state": "stopped" }` on
 *         success), or NULL with @error set.
 */
static JsonNode *
handler_stop (MkTools *self, const MkToolDef *def, JsonObject *args,
              GError **error)
{
  const char *instance = arg_instance (args);

  /* Step 1: which playlist is being consumed, while there is still an active
   * player to ask. */
  gint64 playlistid = -1;
  g_autoptr (JsonNode) active =
    mk_kodi_call (self->kodi, instance, "Player.GetActivePlayers", NULL, error);
  if (active == NULL)
    return NULL;
  JsonArray *players =
    JSON_NODE_HOLDS_ARRAY (active) ? json_node_get_array (active) : NULL;
  if (players != NULL && json_array_get_length (players) > 0)
    {
      JsonObject *p0 = json_array_get_object_element (players, 0);
      gint64 playerid =
        json_object_get_int_member_with_default (p0, "playerid", 0);

      static const char *const fields[] = { "playlistid", NULL };
      g_autoptr (JsonNode) pparams = player_props (playerid, fields);
      g_autoptr (JsonNode) pres = mk_kodi_call (self->kodi, instance,
                                                "Player.GetProperties", pparams,
                                                error);
      if (pres == NULL)
        return NULL;
      playlistid = json_object_get_int_member_with_default (
        json_node_get_object (pres), "playlistid", -1);
    }

  /* Step 2: the keypress. */
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

  /* Step 3: clear the queue the player was consuming, so stop leaves no stale
   * items behind. */
  if (playlistid >= 0)
    {
      g_autoptr (JsonBuilder) cb = json_builder_new ();
      json_builder_begin_object (cb);
      json_builder_set_member_name (cb, "playlistid");
      json_builder_add_int_value (cb, playlistid);
      json_builder_end_object (cb);
      g_autoptr (JsonNode) cparams = json_builder_get_root (cb);

      g_autoptr (JsonNode) cleared = mk_kodi_call (self->kodi, instance,
                                                   "Playlist.Clear", cparams,
                                                   error);
      if (cleared == NULL)
        return NULL;
    }

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
 * schema_volume:
 * @self: the table, used to enumerate instance names.
 *
 * Schema for the volume Knob (§11.6.2.1): the optional `instance` plus an
 * optional integer `step`. `step` is a *relative* change in percentage points —
 * 0 (or omitted) reads the current volume without changing it, positive raises,
 * negative lowers — never an absolute level, so the AI nudges from the live
 * value instead of guessing a target.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_volume (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);
  prop_instance (b, self, FALSE);
  prop_typed (b, "step", "integer",
              "Relative volume change in percentage points. 0 or omitted reads "
              "the current volume without changing it; positive raises, "
              "negative lowers. The result is clamped to the 0-100 range "
              "reported as min/max.");
  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/* Defined alongside the other argument readers, below; used here by the volume
 * Knob to read its optional integer `step`. */
static gboolean arg_int (JsonObject *args, const char *name, gint64 *out);

/**
 * handler_volume:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments (optional `instance`, optional integer `step`).
 * @error: return location for a GError, or NULL.
 *
 * Implements the volume Knob (§11.6.2.1) as a **relative** adjustment, never an
 * absolute set: the AI cannot safely pick a level it has not read, so it nudges
 * from the box's live volume. The `step` argument is a signed integer — 0 (or
 * omitted) reads the current state and changes nothing (the audio analogue of
 * noop), `+N`/`−N` raises/lowers by N percentage points.
 *
 * Always reads first with one `Application.GetProperties` (`volume` + `muted`);
 * for a non-zero `step` it then writes the clamped absolute target with
 * `Application.SetVolume` (Kodi's own increment/decrement step by a fixed 1, so
 * the server computes the level itself to honour an arbitrary ±N and clamp to
 * [0,100]). The setter echoes only the new volume, so the `muted` flag in the
 * reply is carried from the initial read — a read-only call is one round-trip,
 * an adjustment two. Returns the audio snapshot extended with the fixed
 * `min`/`max` bounds: `{ "muted", "volume", "min", "max" }`.
 *
 * @return the resulting audio snapshot, or NULL with @error set.
 */
static JsonNode *
handler_volume (MkTools *self, const MkToolDef *def, JsonObject *args,
                GError **error)
{
  (void) def;
  const char *instance = arg_instance (args);

  gint64 step = 0;
  arg_int (args, "step", &step); /* absent → 0 → read-only */

  gint64 volume = 0;
  gboolean muted = FALSE;
  if (!app_audio_read (self, instance, &volume, &muted, error))
    return NULL;

  if (step != 0)
    {
      gint64 target = CLAMP (volume + step, MK_VOLUME_MIN, MK_VOLUME_MAX);

      g_autoptr (JsonBuilder) b = json_builder_new ();
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "volume");
      json_builder_add_int_value (b, target);
      json_builder_end_object (b);
      g_autoptr (JsonNode) params = json_builder_get_root (b);

      g_autoptr (JsonNode) set = mk_kodi_call (self->kodi, instance,
                                               "Application.SetVolume", params,
                                               error);
      if (set == NULL)
        return NULL;

      /* Kodi returns the new volume; fall back to our clamped target if the
       * reply is not the bare integer we expect. Mute is unchanged by
       * SetVolume, so it is reused from the read above. */
      volume = JSON_NODE_HOLDS_VALUE (set) ? json_node_get_int (set) : target;
    }

  return audio_snapshot (volume, muted, TRUE);
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

/* ---- queue (§11.6.7) -------------------------------------------------------
 *
 * Continuous playback: append to the **already-playing** queue. The caller must
 * start playback first — a single `play`/`playfile` auto-creates the one-item
 * playlist (§13.2.2) — so this tool never builds or starts a playlist itself; it
 * only adds behind (or right after) whatever is playing now.
 */

/* The library id kinds `queue` accepts for its `type`/`id` pair — one per
 * `search` media type (§11.6.4.1), each mapping to Kodi's `<type>id` item key. */
static const char *const mk_queue_types[] = { "song", "episode", "movie", NULL };

/**
 * schema_queue:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `queue` tool (§11.6.7): the optional `instance`, the item to
 * queue as either a `type` + `id` pair (a library item from a `search` row) or
 * a `file` path, and the optional `next` flag (insert right after the current
 * item instead of appending). The one-of-`id`/`file` rule is enforced in the
 * handler so the schema stays a flat property list.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_queue (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);
  prop_enum (b, "type", mk_queue_types,
             "Kind of library id passed in `id` — picks the item key "
             "(songid/episodeid/movieid). Required with `id`; not used with "
             "`file`. Must match the playing queue: a song joins audio "
             "playback, an episode or movie joins video playback.");
  prop_typed (b, "id", "integer",
              "Library id of the item to queue — the `songid`/`episodeid`/"
              "`movieid` of a `search` result row, matching `type`. Give "
              "exactly one of `id` or `file`.");
  prop_typed (b, "file", "string",
              "Path of the file to queue — the `file` field of a `search` "
              "result row; any path Kodi can reach works. Give exactly one of "
              "`id` or `file`.");
  prop_typed (b, "next", "boolean",
              "When true, insert the item right after the one now playing "
              "(\"play next\") instead of appending to the end of the queue.");

  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * handler_queue:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; `type`+`id` or `file` name the item, optional
 *        `next`/`instance`.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `queue` tool (§11.6.7): appends an item to the playlist behind
 * the **already-playing** item, so playback continues into it. Never builds or
 * starts a playlist — the caller starts playback first (`play`/`playfile`,
 * which auto-creates the one-item playlist, §13.2.2). Three steps:
 *
 *   1. `Player.GetActivePlayers` → the active playerid.
 *   2. `Player.GetProperties { playerid, ["playlistid","position"] }`. No
 *      active player, or `playlistid < 0` (live TV / streams play outside any
 *      playlist) → the one "nothing queueable is playing" tool error.
 *   3. `Playlist.Add { playlistid, item }` to append — or `Playlist.Insert`
 *      at `position + 1` when `next` is true, so the item plays right after
 *      the current one.
 *
 * The item is `{ "<type>id": id }` for a library item or `{ "file": path }`
 * for a path. A library item whose kind does not match the active player's
 * type (song ↔ audio, episode/movie ↔ video) is refused before the add —
 * Kodi would accept the foreign item silently — while a `file`'s media type
 * is unknowable up front, so for files matching stays the caller's job.
 * Playback itself is untouched, so the returned player_state() snapshot
 * shows the still-playing current item — a same-item duplicate the history
 * dedup drops (§13.5) — and no settle delay is needed.
 *
 * @return the player-state snapshot (playback unchanged), or NULL with @error
 *         set (bad arguments, nothing queueable playing, or a call failure).
 */
static JsonNode *
handler_queue (MkTools *self, const MkToolDef *def, JsonObject *args,
               GError **error)
{
  (void) def;
  const char *instance = arg_instance (args);
  const char *type = arg_str (args, "type", NULL);
  const char *file = arg_str (args, "file", NULL);
  gint64 id = 0;
  gboolean have_id = arg_int (args, "id", &id);
  gboolean have_file = file != NULL && file[0] != '\0';

  if (have_id == have_file)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "queue: give exactly one of \"id\" (with \"type\") or "
                   "\"file\"");
      return NULL;
    }
  if (have_id)
    {
      gboolean known = FALSE;
      for (gsize i = 0; mk_queue_types[i] != NULL; i++)
        known = known || g_strcmp0 (type, mk_queue_types[i]) == 0;
      if (!known)
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "queue: \"id\" needs \"type\" naming its kind "
                       "(song/episode/movie)");
          return NULL;
        }
    }

  /* Step 1: the active player. An empty list means nothing is playing — the
   * same "nothing queueable" condition as a negative playlistid below. */
  g_autoptr (JsonNode) active =
    mk_kodi_call (self->kodi, instance, "Player.GetActivePlayers", NULL, error);
  if (active == NULL)
    return NULL;
  JsonArray *players =
    JSON_NODE_HOLDS_ARRAY (active) ? json_node_get_array (active) : NULL;
  gint64 playlistid = -1, position = -1;
  const char *ptype = NULL;
  if (players != NULL && json_array_get_length (players) > 0)
    {
      JsonObject *p0 = json_array_get_object_element (players, 0);
      gint64 playerid =
        json_object_get_int_member_with_default (p0, "playerid", 0);
      ptype = json_object_get_string_member_with_default (p0, "type", NULL);

      /* Step 2: the playlist the player is consuming, and where it sits in it
       * (the insert point for `next`). */
      static const char *const fields[] = { "playlistid", "position", NULL };
      g_autoptr (JsonNode) pparams = player_props (playerid, fields);
      g_autoptr (JsonNode) pres = mk_kodi_call (self->kodi, instance,
                                                "Player.GetProperties", pparams,
                                                error);
      if (pres == NULL)
        return NULL;
      JsonObject *props = json_node_get_object (pres);
      playlistid =
        json_object_get_int_member_with_default (props, "playlistid", -1);
      position =
        json_object_get_int_member_with_default (props, "position", -1);
    }
  if (playlistid < 0)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "queue: nothing queueable is playing — start playback "
                   "first (play/playfile), then queue behind it");
      return NULL;
    }

  /* A library item of the wrong kind for the playing queue: Kodi 19.4 accepts
   * the foreign item silently (confirmed live), so without this check the
   * mix-up would surface only later, as a queue that never auto-advances
   * sensibly. A `file` is exempt — its media type is unknowable up front, so
   * for files matching stays the caller's job. */
  if (have_id && ptype != NULL)
    {
      const char *want = g_str_equal (type, "song") ? "audio" : "video";
      if (!g_str_equal (ptype, want))
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "queue: a %s does not match the playing %s queue — "
                       "queue %s instead, or start %s playback first",
                       type, ptype,
                       g_str_equal (ptype, "audio")
                         ? "a song" : "an episode or movie",
                       want);
          return NULL;
        }
    }

  /* Step 3: append, or insert right after the current item. */
  gboolean next = arg_bool (args, "next");
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "playlistid");
  json_builder_add_int_value (b, playlistid);
  if (next)
    {
      json_builder_set_member_name (b, "position");
      json_builder_add_int_value (b, position + 1);
    }
  json_builder_set_member_name (b, "item");
  json_builder_begin_object (b);
  if (have_file)
    {
      json_builder_set_member_name (b, "file");
      json_builder_add_string_value (b, file);
    }
  else
    {
      g_autofree char *idkey = g_strdup_printf ("%sid", type);
      json_builder_set_member_name (b, idkey);
      json_builder_add_int_value (b, id);
    }
  json_builder_end_object (b);
  json_builder_end_object (b);
  g_autoptr (JsonNode) params = json_builder_get_root (b);

  g_autoptr (JsonNode) added =
    mk_kodi_call (self->kodi, instance, next ? "Playlist.Insert" : "Playlist.Add",
                  params, error);
  if (added == NULL)
    return NULL;

  return player_state (self, instance, error);
}

/* ---- getplaylist (§11.6.8) -------------------------------------------------
 *
 * Read the current queue: the rows of a playlist plus where playback sits in
 * it, changing nothing — the inspection partner of `queue` (§11.6.7) and the
 * audit step before `dropplaylists` (§11.6.9) empties everything.
 */

/* The playlist kinds `getplaylist` accepts for `type`, indexed by Kodi's three
 * fixed playlist ids (audio 0 / video 1 / picture 2, §11.6.9). */
static const char *const mk_playlist_types[] = { "audio", "video", "picture",
                                                 NULL };

/**
 * schema_getplaylist:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `getplaylist` tool (§11.6.8): the optional `instance` and the
 * optional `type` selecting which playlist to read. `type` always wins when
 * provided — it reads that playlist even while another plays, the only way to
 * inspect an inactive queue before `dropplaylists` (§11.6.9) destroys it;
 * omitted, the active player's playlist is read.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_getplaylist (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);
  prop_enum (b, "type", mk_playlist_types,
             "Which playlist to read. Always wins when provided — reads that "
             "playlist even while another plays, the only way to inspect an "
             "inactive queue. Omitted reads the active player's playlist.");

  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * handler_getplaylist:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; optional `type` and `instance`.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `getplaylist` tool (§11.6.8): reports what is queued and
 * where playback sits, read-only. The playlist to read is the `type` argument
 * when provided — it always wins, mapping to Kodi's fixed playlist ids
 * (audio 0 / video 1 / picture 2) — else the one behind the active player,
 * resolved as `queue` does (§11.6.7):
 *
 *   1. `Player.GetActivePlayers` → the active playerid (if any).
 *   2. `Player.GetProperties { playerid, ["playlistid","position"] }` — which
 *      playlist the player is consuming and where it sits in it.
 *   3. `Playlist.GetItems { playlistid, properties: ["file"] }` on the
 *      resolved playlist (skipped when there is nothing to resolve).
 *
 * Each queued row carries the item's library id under its `<type>id` key —
 * the same identifier `queue` takes, so a row can be re-queued as-is — and/or
 * its `file` (a `playfile`-ready path), plus `label` and `type`. `position` is
 * reported only when the playlist read is the one the active player is
 * consuming: a cursor into someone else's playlist would be meaningless. An
 * empty playlist — or no `type` with nothing playing (no active player, or
 * non-playlist playback, §11.6.7) — yields an empty `items` list, not an
 * error.
 *
 * @return `{ "type"?, "total", "position"?, "items": [ { "<type>id"?,
 *         "file"?, "label"?, "type" } ] }`, or NULL with @error set (bad
 *         arguments or a call failure).
 */
static JsonNode *
handler_getplaylist (MkTools *self, const MkToolDef *def, JsonObject *args,
                     GError **error)
{
  (void) def;
  const char *instance = arg_instance (args);
  const char *type = arg_str (args, "type", NULL);

  /* The requested playlist: `type` maps to Kodi's fixed playlist ids by its
   * index in mk_playlist_types; -1 means "the active player's". */
  gint64 want = -1;
  if (type != NULL)
    {
      for (gsize i = 0; mk_playlist_types[i] != NULL; i++)
        if (g_str_equal (type, mk_playlist_types[i]))
          want = (gint64) i;
      if (want < 0)
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "getplaylist: \"type\" must name a playlist "
                       "(audio/video/picture)");
          return NULL;
        }
    }

  /* Steps 1–2: the active player's playlist and position — the default read
   * target, and the test deciding whether `position` applies below. */
  g_autoptr (JsonNode) active =
    mk_kodi_call (self->kodi, instance, "Player.GetActivePlayers", NULL, error);
  if (active == NULL)
    return NULL;
  JsonArray *players =
    JSON_NODE_HOLDS_ARRAY (active) ? json_node_get_array (active) : NULL;
  gint64 activelist = -1, position = -1;
  if (players != NULL && json_array_get_length (players) > 0)
    {
      JsonObject *p0 = json_array_get_object_element (players, 0);
      gint64 playerid =
        json_object_get_int_member_with_default (p0, "playerid", 0);
      static const char *const fields[] = { "playlistid", "position", NULL };
      g_autoptr (JsonNode) pparams = player_props (playerid, fields);
      g_autoptr (JsonNode) pres = mk_kodi_call (self->kodi, instance,
                                                "Player.GetProperties", pparams,
                                                error);
      if (pres == NULL)
        return NULL;
      JsonObject *props = json_node_get_object (pres);
      activelist =
        json_object_get_int_member_with_default (props, "playlistid", -1);
      position =
        json_object_get_int_member_with_default (props, "position", -1);
    }

  gint64 playlistid = want >= 0 ? want : activelist;

  /* Step 3: list the resolved playlist. No playlist to resolve (no `type` and
   * nothing playing) reads as an empty queue, not an error. */
  g_autoptr (JsonNode) ires = NULL;
  JsonArray *rows = NULL;
  if (playlistid >= 0)
    {
      g_autoptr (JsonBuilder) pb = json_builder_new ();
      json_builder_begin_object (pb);
      json_builder_set_member_name (pb, "playlistid");
      json_builder_add_int_value (pb, playlistid);
      json_builder_set_member_name (pb, "properties");
      json_builder_begin_array (pb);
      json_builder_add_string_value (pb, "file");
      json_builder_end_array (pb);
      json_builder_end_object (pb);
      g_autoptr (JsonNode) iparams = json_builder_get_root (pb);

      ires = mk_kodi_call (self->kodi, instance, "Playlist.GetItems", iparams,
                           error);
      if (ires == NULL)
        return NULL;
      /* An empty playlist's reply carries no `items` member at all. */
      JsonObject *io =
        JSON_NODE_HOLDS_OBJECT (ires) ? json_node_get_object (ires) : NULL;
      if (io != NULL && json_object_has_member (io, "items"))
        rows = json_object_get_array_member (io, "items");
    }

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  if (playlistid >= 0 && playlistid < 3)
    {
      json_builder_set_member_name (b, "type");
      json_builder_add_string_value (b, mk_playlist_types[playlistid]);
    }
  json_builder_set_member_name (b, "total");
  json_builder_add_int_value (b, rows != NULL ? json_array_get_length (rows)
                                              : 0);
  /* `position` only when the playlist read is the active player's own. */
  if (playlistid >= 0 && playlistid == activelist && position >= 0)
    {
      json_builder_set_member_name (b, "position");
      json_builder_add_int_value (b, position);
    }
  json_builder_set_member_name (b, "items");
  json_builder_begin_array (b);
  for (guint i = 0; rows != NULL && i < json_array_get_length (rows); i++)
    {
      JsonObject *it = json_array_get_object_element (rows, i);
      const char *itype =
        json_object_get_string_member_with_default (it, "type", "unknown");
      gint64 iid = json_object_get_int_member_with_default (it, "id", -1);

      json_builder_begin_object (b);
      /* The library id under its `<type>id` key — the identifier `queue`
       * takes, so the row re-queues as-is. Off-library rows have none: Kodi
       * reports them as type "unknown" with id -1 (§13.4.1.1), leaving their
       * `file` as the row's identity. */
      if (iid >= 0 && !g_str_equal (itype, "unknown"))
        {
          g_autofree char *idkey = g_strdup_printf ("%sid", itype);
          json_builder_set_member_name (b, idkey);
          json_builder_add_int_value (b, iid);
        }
      copy_member_nonempty (b, it, "file");
      copy_member_nonempty (b, it, "label");
      json_builder_set_member_name (b, "type");
      json_builder_add_string_value (b, itype);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/* ---- dropplaylists (§11.6.9) -----------------------------------------------
 *
 * Empty the queues: Playlist.Clear on each of Kodi's three fixed playlists so
 * no queued items remain anywhere — the destructive counterpart of
 * `getplaylist` (§11.6.8), and the whole-device sibling of `stop`'s single
 * clear (§11.6.1.3).
 */

/**
 * handler_dropplaylists:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments (optional `instance`), or NULL.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `dropplaylists` tool (§11.6.9): `Playlist.Clear` on each of
 * Kodi's three fixed playlist ids (audio 0 / video 1 / picture 2 — the
 * mk_playlist_types order), so no queued items remain. Plural by design: one
 * call clears all three, with no way to pick — clearing a single, known
 * playlist is `stop`'s narrower job (§11.6.1.3). Kodi's clear on the *active*
 * playlist leaves the current item playing — only the queue behind it is
 * emptied — so this never interrupts playback; the post-clear player_state()
 * snapshot shows whatever is still playing, now with nothing queued after it.
 *
 * The clears run in fixed id order and the first failure aborts the rest: a
 * box that refuses one clear would refuse them all the same way, and a
 * partial-success tally would only blur the error.
 *
 * @return the post-clear player_state() snapshot, or NULL with @error set.
 */
static JsonNode *
handler_dropplaylists (MkTools *self, const MkToolDef *def, JsonObject *args,
                       GError **error)
{
  (void) def;
  const char *instance = arg_instance (args);

  for (gsize i = 0; mk_playlist_types[i] != NULL; i++)
    {
      g_autoptr (JsonBuilder) b = json_builder_new ();
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "playlistid");
      json_builder_add_int_value (b, (gint64) i);
      json_builder_end_object (b);
      g_autoptr (JsonNode) params = json_builder_get_root (b);

      g_autoptr (JsonNode) cleared =
        mk_kodi_call (self->kodi, instance, "Playlist.Clear", params, error);
      if (cleared == NULL)
        return NULL;
    }

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

  g_autoptr (JsonNode) result =
    mk_kodi_call (self->kodi, instance, method, params, error);
  if (result == NULL)
    return NULL;

  /* §13.3.1: rpc returns Kodi's raw result and takes no snapshot of its own, so
   * a hand-rolled Player.Open would slip past history. After a *successful*
   * call, take one player_state() snapshot purely to feed the log — it records
   * as a side effect (§13.9.1). This must NOT change rpc's verbatim return
   * (§11.6.6), so the snapshot and any error are discarded. A non-playback rpc
   * just yields a stopped or duplicate snapshot that records nothing (§13.5),
   * costing only the cheap Player.GetActivePlayers probe. A brief settle delay
   * (as the Buttons use) lets a hand-rolled Player.Open register an active
   * player before we snapshot, so the capture isn't lost to the start-up race. */
  if (self->history != NULL)
    {
      g_usleep (MK_BUTTON_SETTLE_US);
      g_autoptr (GError) ignored = NULL;
      g_autoptr (JsonNode) snap = player_state (self, instance, &ignored);
      (void) snap;
    }

  return g_steal_pointer (&result);
}

/* ---- History tool (§13.10) ------------------------------------------------ */

#define MK_HISTORY_DEFAULT_LIMIT 50   /* newest-in-window cap when omitted */
#define MK_HISTORY_MAX_LIMIT     1000 /* hard ceiling on a single read     */

/**
 * schema_history:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `history` tool (§13.10.2): an optional ISO-8601 window
 * (`since`/`until`), an optional `instance`, and a `limit`. Nothing is required
 * — a bare call returns the most recent entries across all boxes. The `instance`
 * description deliberately spells out that *omitting* it spans all instances,
 * which inverts the §5.1 convention every other tool follows (omitted = the
 * default box), so the difference is explicit rather than a surprise.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_history (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  /* Custom instance prop — prop_instance() documents "omitted = default box",
   * the opposite of what history means, so build the description here. */
  g_autoptr (GString) idesc =
    g_string_new ("Restrict to one Kodi instance by key");
  GList *names = mk_config_instance_names (self->config);
  if (names != NULL)
    {
      g_string_append (idesc, " (one of: ");
      for (GList *l = names; l != NULL; l = l->next)
        {
          g_string_append (idesc, (const char *) l->data);
          if (l->next != NULL)
            g_string_append (idesc, ", ");
        }
      g_string_append_c (idesc, ')');
    }
  g_list_free (names);
  g_string_append (idesc, ". Omitted returns entries from ALL instances — note "
                          "this differs from other tools, where an omitted "
                          "instance means the default box.");
  prop_typed (b, "instance", "string", idesc->str);

  prop_typed (b, "since", "string",
              "Only entries at or after this ISO-8601 time (e.g. "
              "\"2026-06-01T00:00:00Z\"). Omitted = from the beginning of the "
              "log. Compute relative windows (\"last 7 days\") yourself.");
  prop_typed (b, "until", "string",
              "Only entries at or before this ISO-8601 time. Omitted = up to "
              "now.");
  prop_typed (b, "limit", "integer",
              "Max entries to return, newest first (default 50, max 1000). When "
              "more match the window, the reply sets \"truncated\": true.");

  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * handler_history:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments: optional instance/since/until/limit.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `history` tool (§13.10.2): reads the local playback log via
 * mk_history_read() and wraps the matches in a `search`-style envelope. Makes no
 * Kodi call. An omitted `instance` spans all boxes (the §5.1 inversion, see
 * schema_history()); `limit` defaults to 50 and is clamped. A malformed window
 * bound or a corrupt log surfaces as a normal tool error (mk_history_read sets
 * @error).
 *
 * @return `{ "instance"?, "since"?, "until"?, "total", "returned", "truncated",
 *         "entries": [ … ] }`, or NULL with @error set.
 */
static JsonNode *
handler_history (MkTools *self, const MkToolDef *def, JsonObject *args,
                 GError **error)
{
  (void) def;
  /* Unlike every other tool, an omitted instance is NOT defaulted to a box: NULL
   * tells mk_history_read() to span all instances (§13.10.2). */
  const char *instance = arg_instance (args);
  const char *since = arg_str (args, "since", NULL);
  const char *until = arg_str (args, "until", NULL);

  gint64 limit = MK_HISTORY_DEFAULT_LIMIT, v = 0;
  if (arg_int (args, "limit", &v))
    limit = v;
  limit = CLAMP (limit, 1, MK_HISTORY_MAX_LIMIT);

  gint64 total = 0;
  g_autoptr (JsonNode) entries =
    mk_history_read (self->history, instance, since, until, limit, &total,
                     error);
  if (entries == NULL)
    return NULL;
  gint64 returned = json_array_get_length (json_node_get_array (entries));

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  if (instance != NULL)
    {
      json_builder_set_member_name (b, "instance");
      json_builder_add_string_value (b, instance);
    }
  if (since != NULL && *since != '\0')
    {
      json_builder_set_member_name (b, "since");
      json_builder_add_string_value (b, since);
    }
  if (until != NULL && *until != '\0')
    {
      json_builder_set_member_name (b, "until");
      json_builder_add_string_value (b, until);
    }
  json_builder_set_member_name (b, "total");
  json_builder_add_int_value (b, total);
  json_builder_set_member_name (b, "returned");
  json_builder_add_int_value (b, returned);
  json_builder_set_member_name (b, "truncated");
  json_builder_add_boolean_value (b, total > returned);
  json_builder_set_member_name (b, "entries");
  json_builder_add_value (b, json_node_ref (entries));
  json_builder_end_object (b);
  return json_builder_get_root (b);
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
   * instance — and clear the playlist it was consuming (§11.6.1.3). Kodi's
   * stop ends playback but leaves the queue intact, so without the clear any
   * items queued behind the stopped one would linger invisibly; to the caller
   * "stop" means "done with this", queue included. Live/non-playlist playback
   * and an already-stopped player skip the clear (nothing to remove); other
   * playlists are untouched (`dropplaylists`, §11.6.9, clears everything).
   *
   * Call:  Player.GetActivePlayers + Player.GetProperties to resolve the
   *        consumed playlistid (before the keypress kills the player),
   *        Input.ExecuteAction { "action": "stop" }, Playlist.Clear on that
   *        playlist, then player_state() to report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the resulting player-state snapshot (player_state()); after a
   *         successful stop nothing is loaded, so `{ "state": "stopped" }`.
   */
  { "stop", "Press Stop on the Kodi remote: stop the active player on the "
            "target instance and clear the playlist it was playing, so no "
            "queued items linger.",
    schema_instance_only, handler_stop, "stop" },

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

  /* ---- Knobs — adjust a scalar relative to its live value (§11.6.2). ---- */

  /**
   * volume (Knob):
   *
   * Adjust the target instance's volume *relatively* (§11.6.2.1). Never set an
   * absolute level: pick a `step` and the box moves from wherever it is now, so
   * the caller cannot accidentally blast or silence it by guessing a number it
   * has not read. `step` is a signed integer in percentage points — 0 (or
   * omitted) reports the current volume without changing it (a read probe),
   * positive raises, negative lowers; the result is clamped to 0-100.
   *
   * Call:  Application.GetProperties (volume + muted) to read; when `step` ≠ 0,
   *        Application.SetVolume with the clamped absolute target.
   * @param step (optional): relative change in percentage points; 0/omitted
   *        reads only.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the resulting audio snapshot with the fixed scale bounds:
   *         `{ "muted": <bool>, "volume": <int>, "min": 0, "max": 100 }`.
   */
  { "volume", "Adjust the target instance's volume by a relative step (in "
              "percentage points); step 0 or omitted just reports the current "
              "volume. Returns volume, mute state, and the min/max bounds.",
    schema_volume, handler_volume, NULL },

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

  /**
   * queue (Playback tool):
   *
   * Append an item behind the already-playing one, so playback continues into
   * it (§11.6.7). Requires something to be playing: a single `play`/`playfile`
   * auto-creates the one-item playlist (§13.2.2), so this tool never builds or
   * starts a playlist itself — no active player, or non-playlist playback
   * (live TV / streams), is the one "nothing queueable is playing" error. The
   * item is a `search` row's library id (`type` + `id`) or `file` path;
   * `next: true` inserts it right after the current item instead of appending.
   * A library id whose kind does not match the playing queue (song ↔ audio,
   * episode/movie ↔ video) is refused — Kodi would accept the foreign item
   * silently; a `file`'s type is unknowable, so for files matching stays the
   * caller's job.
   *
   * Call:  Player.GetActivePlayers → Player.GetProperties (playlistid,
   *        position) → Playlist.Add, or Playlist.Insert at position+1 for
   *        `next` — then player_state() (playback unchanged).
   * @param type: "song" | "episode" | "movie"; required with `id`.
   * @param id: library id of the item to queue (give `id` or `file`).
   * @param file: path of the file to queue (give `id` or `file`).
   * @param next (optional): insert right after the current item.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default (§5.1).
   * @return the player-state snapshot (player_state()), unchanged by the add —
   *         the current item keeps playing with the new one queued behind it.
   */
  { "queue", "Queue an item behind the one now playing, for continuous "
             "playback: a `search` row's library id (type+id) or a file path; "
             "next:true plays it right after the current item. Something must "
             "already be playing (start with play/playfile).",
    schema_queue, handler_queue, NULL },

  /**
   * getplaylist (Playback tool):
   *
   * Read the queue without changing anything (§11.6.8) — the inspection
   * partner of `queue` and the audit step before `dropplaylists` (§11.6.9).
   * With no `type`, reads the playlist behind the active player (resolved
   * like `queue`, §11.6.7); a given `type` (audio/video/picture) always wins,
   * reading that playlist even while another plays — the only way to see an
   * inactive queue. Each row carries the item's library id under its
   * `<type>id` key (re-queueable as-is) and/or its `file` (playfile-ready),
   * plus `label` and `type`. `position` — where the now-playing item sits —
   * is reported only when the playlist read is the active player's own. An
   * empty playlist, or nothing playing with no `type`, is an empty list, not
   * an error.
   *
   * Call:  Player.GetActivePlayers → Player.GetProperties (playlistid,
   *        position) → Playlist.GetItems on the resolved playlist.
   * @param type: "audio" | "video" | "picture" — the playlist to read;
   *        omitted reads the active player's.
   * @param instance (optional): the Kodi instance to target; omitted uses the
   *        configured default (§5.1).
   * @return `{ "type"?, "total", "position"?, "items": [ { "<type>id"?,
   *         "file"?, "label"?, "type" } ] }` — read-only, playback untouched.
   */
  { "getplaylist", "Read the queue without changing it: the items of the "
                   "active player's playlist — or of a named one (audio/"
                   "video/picture), which always wins — plus the position of "
                   "the now-playing item. An empty queue is an empty list.",
    schema_getplaylist, handler_getplaylist, NULL },

  /**
   * dropplaylists (Playback tool):
   *
   * Empty the queues: clear all three of Kodi's fixed playlists (audio /
   * video / picture) in one call, so no queued items remain anywhere
   * (§11.6.9). Plural by design — there is no picking one; clearing only the
   * playlist being played is what `stop` does (§11.6.1.3). Clearing the
   * active playlist leaves the current item playing — only the queue behind
   * it is emptied — so playback is never interrupted; to also stop what is
   * playing, call `stop`. Read the queues first with `getplaylist` (§11.6.8)
   * if their content matters: the drop is not undoable.
   *
   * Call:  Playlist.Clear { playlistid } for ids 0, 1 and 2, then
   *        player_state() to report the (unchanged) playback.
   * @param instance (optional): the Kodi instance to target; omitted uses the
   *        configured default (§5.1).
   * @return the player-state snapshot (player_state()) — whatever was playing
   *         still is, now with nothing queued behind it; idle boxes report
   *         `{ "state": "stopped" }`.
   */
  { "dropplaylists", "Empty all queues: clear the audio, video and picture "
                     "playlists in one call. The current item keeps playing — "
                     "only the queued items behind it are removed — so "
                     "playback is never interrupted. Not undoable; inspect "
                     "with getplaylist first if the content matters.",
    schema_instance_only, handler_dropplaylists, NULL },

  /* ---- History tool — read back the local playback log; no Kodi call
   *      (§13.10). ---- */

  /**
   * history (History tool):
   *
   * List recently played items from the local playback log (§13) — the
   * backward-looking record of what the assistant caused or observed (§13.2),
   * written as a side effect of every playback-affecting call. Reads only the
   * local `history.json`; makes no Kodi request, so it answers even when no box
   * is reachable. Mirror of the write path: mk_history_read() under a shared
   * lock (§13.10.1), wrapped in a `search`-style envelope.
   *
   * @param since (optional): ISO-8601 lower bound; omitted = start of the log.
   * @param until (optional): ISO-8601 upper bound; omitted = now.
   * @param instance (optional): restrict to one box. **Omitted spans ALL
   *        instances** — unlike other tools, where omitted means the default box
   *        (§13.10.2).
   * @param limit (optional): newest-in-window cap (default 50, max 1000).
   * @return `{ "instance"?, "since"?, "until"?, "total", "returned",
   *         "truncated", "entries": [ { "at", "instance", "name"?, "kind",
   *         "media"?, "id"?, "title"?, … } ] }` — each entry a §13.4 record,
   *         newest first; `total` counts matches before the limit.
   */
  { "history", "List recently played items from the local playback log. Pass an "
               "optional ISO-8601 window (since/until) and a limit; an omitted "
               "instance returns all boxes. Reads only the local log — no Kodi "
               "call, so it works even when no box is reachable.",
    schema_history, handler_history, NULL },

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
 * Creates the tool table. Both @config and @kodi must outlive it. The table
 * also owns the playback-history log (§13) bound to the default path:
 * player_state() feeds the write path (§13.9.1) and the `history` tool reads it
 * back (§13.10).
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
  self->history = mk_history_new (NULL);
  return self;
}

/**
 * mk_tools_free:
 * @self: the table to free, or NULL.
 *
 * Frees the table and the history log it owns. The borrowed config and Kodi
 * client are left untouched. Safe to call with NULL.
 */
void
mk_tools_free (MkTools *self)
{
  if (self == NULL)
    return;
  mk_history_free (self->history);
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
