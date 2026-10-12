/* mcp-kodi — tool table: schemas + handlers. See mk-tools.h.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 */

#include "config.h"

#include "mk-tools.h"
#include "mk-build-id.h"

#include "mk-history.h"

struct _MkTools
{
  MkConfig  *config;  /* borrowed; must outlive the table */
  MkKodi    *kodi;    /* borrowed; must outlive the table */
  MkHistory *history; /* owned; the playback-history log */
};

G_DEFINE_QUARK (mk-tools-error-quark, mk_tools_error)

typedef struct _MkToolDef MkToolDef;

/* A handler runs a tool: it reads @args (may be NULL) and its own table row
 * @def, drives @self->kodi, and returns the result payload as a newly allocated
 * JsonNode, or NULL with @error set on failure. Receiving @def lets one handler
 * serve a whole family of data-driven rows — e.g. every Button shares
 * handler_button() and reads its action from @def. A NULL handler in the
 * table marks a tool not yet implemented. */
typedef JsonNode *(*MkToolHandler) (MkTools         *self,
                                    const MkToolDef *def,
                                    JsonObject      *args,
                                    GError         **error);

/* Builds a tool's `inputSchema` (a JSON Schema object) against @self, so the
 * schema can name the configured instances. */
typedef JsonNode *(*MkToolSchema) (MkTools *self);

struct _MkToolDef
{
  const char   *name;
  const char   *description;
  MkToolSchema  schema;
  MkToolHandler handler; /* NULL until the tool is implemented */
  /* Per-handler payload string carried by data-driven rows, or NULL.
   * handler_button() reads it as the Input.ExecuteAction action ("play",
   * "pause", "stop"); handler_mute() reads it as the SetMute value ("true" to
   * mute, "false" to unmute). */
  const char   *action;
  /* Result-shape schema (MCP `outputSchema`, spec 2025-06-18), or NULL when
   * the result has no fixed shape (`rpc`). A tool that declares one also gets
   * its successful result mirrored as `structuredContent` (mk_tools_call()). */
  MkToolSchema  out_schema;
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
 * prop_array_obj:
 * @b: the builder, positioned inside a `properties` object.
 * @name: the property name.
 * @item_desc: a description of the item objects' members, or NULL.
 * @desc: a description of the array property itself, or NULL.
 *
 * Adds `"<name>": { "type": "array"[, "description": <desc>], "items":
 * { "type": "object"[, "description": <item_desc>] } }` — an array of open
 * objects whose members are documented in @item_desc rather than pinned as
 * sub-properties. Used by the output schemas for row/entry arrays, whose
 * members genuinely vary per media type (a movie row carries no artist; a
 * playlist item's id key is the dynamic `<type>id`), so a fixed property
 * list would misdescribe them.
 */
static void
prop_array_obj (JsonBuilder *b, const char *name, const char *item_desc,
                const char *desc)
{
  json_builder_set_member_name (b, name);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "array");
  if (desc != NULL)
    {
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, desc);
    }
  json_builder_set_member_name (b, "items");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "object");
  if (item_desc != NULL)
    {
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, item_desc);
    }
  json_builder_end_object (b);
  json_builder_end_object (b);
}

/**
 * prop_instance:
 * @b: the builder, positioned inside a `properties` object.
 * @self: the table, used to list configured instance names.
 * @allow_star: whether to document the `"*"` (all instances) wildcard.
 *
 * Adds the optional `instance` property whose description enumerates the
 * configured instances and names the default, so the client can target
 * a box by name. With @allow_star, also documents `"*"` for "every instance"
 * (used by `status`).
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
 * Button plus other device-targeted, argument-free tools.
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
 * Schema for the `instances` config tool: an `action` selector plus
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
 * Reads the optional `instance` argument. The returned string is
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
 * array_has_value:
 * @a: a JSON array.
 *
 * @return TRUE if @a carries any information: a string element is informative
 * only when non-empty, any non-string element always counts. So `[]` and an
 * array of only empty strings (`[""]`, the blank artist Kodi hands back for an
 * untagged song) report FALSE, while `["Abba"]` reports TRUE.
 */
static gboolean
array_has_value (JsonArray *a)
{
  guint n = json_array_get_length (a);
  for (guint i = 0; i < n; i++)
    {
      JsonNode *el = json_array_get_element (a, i);
      if (!JSON_NODE_HOLDS_VALUE (el)
          || json_node_get_value_type (el) != G_TYPE_STRING)
        return TRUE;
      const char *s = json_node_get_string (el);
      if (s != NULL && *s != '\0')
        return TRUE;
    }
  return FALSE;
}

/**
 * copy_member_nonempty:
 * @b: the builder, positioned inside an object.
 * @src: the source object to copy from, or NULL.
 * @name: the member to copy.
 *
 * Like copy_member(), but skips values that carry no information: an empty
 * string, an empty (or all-empty-string) array, or a negative integer. Kodi
 * returns every requested `Player.GetItem` property regardless of media type,
 * filling the inapplicable ones with sentinels — `""`, `[]`, and `-1`
 * (confirmed live: an off-library file reports `season`/`episode`/`track` as -1
 * and `showtitle`/`album` as "" with `type:"unknown"`). A poorly-tagged song
 * also yields `artist:[""]` — a one-element array of an empty string, length 1
 * yet empty — which this likewise drops, so a blank artist is omitted (its
 * absence then honestly means "unknown") rather than stored as noisy `[""]`.
 * This omits exactly those, so the snapshot carries a per-media field only where
 * it applies ("omit what's empty"). A zero is kept on purpose — `season` 0 is
 * the legitimate "specials" season.
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
           && !array_has_value (json_node_get_array (node)))
    return;
  json_builder_set_member_name (b, name);
  json_builder_add_value (b, json_node_copy (node));
}

/**
 * copy_best_artist:
 * @b: the builder, positioned inside the snapshot object.
 * @item: the `Player.GetItem` item object.
 *
 * Records the best available performer for a song under `artist`, trying the
 * song-level `artist`, then `albumartist`, then `displayartist` in turn and
 * stopping at the first that carries a value. For a well-tagged track the song
 * `artist` wins as before; but Kodi returns `artist:[""]` for an untagged /
 * bootleg rip even when it has computed an `albumartist`/`displayartist` from
 * the album/folder/scraper, so requesting only `artist` (as player_state() used
 * to) lost the performer for poorly-tagged music. The chosen value is
 * normalized to the array shape `artist` already carries: a `displayartist`
 * (a plain joined string) is wrapped in a one-element array. Nothing is
 * recorded when none of the three carries a value, so an absent `artist`
 * honestly means "unknown" — mirroring copy_member_nonempty's "omit what's
 * empty" for the rest of the snapshot.
 */
static void
copy_best_artist (JsonBuilder *b, JsonObject *item)
{
  static const char *const sources[] = { "artist", "albumartist", NULL };
  for (gsize i = 0; sources[i] != NULL; i++)
    {
      if (!json_object_has_member (item, sources[i]))
        continue;
      JsonNode *node = json_object_get_member (item, sources[i]);
      if (JSON_NODE_HOLDS_ARRAY (node)
          && array_has_value (json_node_get_array (node)))
        {
          json_builder_set_member_name (b, "artist");
          json_builder_add_value (b, json_node_copy (node));
          return;
        }
    }
  if (!json_object_has_member (item, "displayartist"))
    return;
  JsonNode *node = json_object_get_member (item, "displayartist");
  if (!JSON_NODE_HOLDS_VALUE (node)
      || json_node_get_value_type (node) != G_TYPE_STRING)
    return;
  const char *s = json_node_get_string (node);
  if (s == NULL || *s == '\0')
    return;
  json_builder_set_member_name (b, "artist");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, s);
  json_builder_end_array (b);
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
 * hms_to_ms:
 * @t: a Kodi time object `{ hours, minutes, seconds, milliseconds }`.
 *
 * Flattens one of Kodi's split clock objects into a single millisecond count.
 *
 * @return the total milliseconds (0 for a NULL or all-zero object).
 */
static gint64
hms_to_ms (JsonObject *t)
{
  if (t == NULL)
    return 0;
  gint64 h = json_object_get_int_member_with_default (t, "hours", 0);
  gint64 m = json_object_get_int_member_with_default (t, "minutes", 0);
  gint64 s = json_object_get_int_member_with_default (t, "seconds", 0);
  gint64 ms = json_object_get_int_member_with_default (t, "milliseconds", 0);
  return ((h * 60 + m) * 60 + s) * 1000 + ms;
}

/**
 * append_clock:
 * @out: the string to append to.
 * @ms: a duration in milliseconds.
 *
 * Appends @ms as a human clock — `H:MM:SS` when there is an hour or more, else
 * `M:SS`. Sub-second precision is dropped: the legible form answers "how far
 * in / how long", not frame-accurate position.
 */
static void
append_clock (GString *out, gint64 ms)
{
  gint64 total = ms / 1000;
  gint64 h = total / 3600;
  gint64 m = (total % 3600) / 60;
  gint64 s = total % 60;
  if (h > 0)
    g_string_append_printf (out,
                            "%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT
                            ":%02" G_GINT64_FORMAT,
                            h, m, s);
  else
    g_string_append_printf (out, "%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, m,
                            s);
}

/**
 * append_progress:
 * @b: the builder, positioned inside the snapshot object.
 * @props: the `Player.GetProperties` reply (carries `time`/`totaltime`).
 *
 * Surfaces playback position legibly (TODO 7.1): the bare
 * `{hours,minutes,seconds,milliseconds}` time objects don't read as "how far
 * in", so this derives a single human clause — `"15:38 / 46:40 (33%)"` —
 * the elapsed/total pair plus a rounded percentage, so an assistant can answer
 * "how much is left" without doing clock arithmetic itself. Emitted only when
 * both times are present and the item has a real duration; a live stream
 * (totaltime 0) carries no `progress`, matching "fields appear only where they
 * apply".
 */
static void
append_progress (JsonBuilder *b, JsonObject *props)
{
  JsonObject *t = json_object_has_member (props, "time")
                    ? json_object_get_object_member (props, "time")
                    : NULL;
  JsonObject *tt = json_object_has_member (props, "totaltime")
                     ? json_object_get_object_member (props, "totaltime")
                     : NULL;
  if (t == NULL || tt == NULL)
    return;

  gint64 cur = hms_to_ms (t);
  gint64 tot = hms_to_ms (tt);
  if (tot <= 0)
    return; /* unknown length (live stream): no meaningful progress */
  if (cur > tot)
    cur = tot;
  gint64 pct = (cur * 100 + tot / 2) / tot; /* rounded to nearest percent */

  g_autoptr (GString) s = g_string_new (NULL);
  append_clock (s, cur);
  g_string_append (s, " / ");
  append_clock (s, tot);
  g_string_append_printf (s, " (%" G_GINT64_FORMAT "%%)", pct);

  json_builder_set_member_name (b, "progress");
  json_builder_add_string_value (b, s->str);
}

/**
 * append_av_streams:
 * @b: the builder, positioned inside the snapshot object.
 * @props: the `Player.GetProperties` reply of a *video* player (the only kind
 *         that carries these stream properties).
 *
 * Surfaces the audio tracks and subtitles a playing video carries (TODO 7.3),
 * but **briefly**: how many of each are on offer (`count`) plus the live
 * selection (`current`), not the full per-stream dump — that detailed,
 * pre-playback view is `audio_and_subtitle_info`'s job, and re-listing every
 * stream on every status reply would only spam the caller. The `current`
 * object hands back the same `index` the `set_audio_and_subtitle` setter selects
 * by (live, so authoritative once sidecars have merged in), described with the
 * same `language`/`codec`/`channels` vocabulary that tool uses.
 *
 * `audio` is emitted whenever the player reports any audio stream (a video
 * always has at least one). `subtitle` appears only when subtitles are on offer,
 * carrying `enabled`; the `current` selection is included only while a subtitle
 * is actually shown, so a disabled subtitle reads `{ count, enabled: false }`.
 * Nothing is emitted for a kind with no streams ("fields appear only where they
 * apply").
 */
static void
append_av_streams (JsonBuilder *b, JsonObject *props)
{
  JsonNode *an = json_object_has_member (props, "audiostreams")
                   ? json_object_get_member (props, "audiostreams")
                   : NULL;
  JsonArray *audio =
    an != NULL && JSON_NODE_HOLDS_ARRAY (an) ? json_node_get_array (an) : NULL;
  guint na = audio != NULL ? json_array_get_length (audio) : 0;
  if (na > 0)
    {
      json_builder_set_member_name (b, "audio");
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "count");
      json_builder_add_int_value (b, na);
      JsonNode *cn = json_object_has_member (props, "currentaudiostream")
                       ? json_object_get_member (props, "currentaudiostream")
                       : NULL;
      if (cn != NULL && JSON_NODE_HOLDS_OBJECT (cn))
        {
          JsonObject *cur = json_node_get_object (cn);
          json_builder_set_member_name (b, "current");
          json_builder_begin_object (b);
          copy_member (b, cur, "index");
          copy_member_nonempty (b, cur, "language");
          copy_member_nonempty (b, cur, "codec");
          copy_member (b, cur, "channels");
          json_builder_end_object (b);
        }
      json_builder_end_object (b);
    }

  JsonNode *sn = json_object_has_member (props, "subtitles")
                   ? json_object_get_member (props, "subtitles")
                   : NULL;
  JsonArray *subs =
    sn != NULL && JSON_NODE_HOLDS_ARRAY (sn) ? json_node_get_array (sn) : NULL;
  guint ns = subs != NULL ? json_array_get_length (subs) : 0;
  if (ns > 0)
    {
      gboolean enabled = json_object_get_boolean_member_with_default (
        props, "subtitleenabled", FALSE);
      json_builder_set_member_name (b, "subtitle");
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "count");
      json_builder_add_int_value (b, ns);
      json_builder_set_member_name (b, "enabled");
      json_builder_add_boolean_value (b, enabled);
      JsonNode *cn = json_object_has_member (props, "currentsubtitle")
                       ? json_object_get_member (props, "currentsubtitle")
                       : NULL;
      if (enabled && cn != NULL && JSON_NODE_HOLDS_OBJECT (cn))
        {
          JsonObject *cur = json_node_get_object (cn);
          json_builder_set_member_name (b, "current");
          json_builder_begin_object (b);
          copy_member (b, cur, "index");
          copy_member_nonempty (b, cur, "language");
          json_builder_end_object (b);
        }
      json_builder_end_object (b);
    }
}

/* Recovers the real media identity of an off-library video played by path;
 * defined below its library-query helpers (add_field_filter/list_total/…). */
static void reconcile_off_library_item (MkTools     *self,
                                        const char  *instance,
                                        JsonObject  *item);

/**
 * player_state:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @error: return location for a GError, or NULL.
 *
 * Builds the server's canonical now-playing snapshot for @instance — the
 * **shared response shape** returned by the many tools whose effect is a
 * change in player state (every transport Button — play/pause/stop —
 * plus seek, skip, speed, handoff, status, …). Reusing one builder keeps that
 * response identical across all of them, so a client learns the shape once and
 * any of those calls answers "what is loaded, is it playing, and how far in".
 *
 * This snapshot is also the **raw material the playback-history log reuses**:
 * the history record is composed straight from this object plus the instance
 * key and a capture timestamp, with
 * **no extra Kodi round-trip** beyond the calls below. The one exception is the
 * `rpc` escape hatch, which takes no snapshot of its own and so will issue one
 * post-call purely to feed history.
 *
 * To be that single source of truth, the `Player.GetItem` call is widened
 * to also fetch the per-media identifiers history records:
 * `showtitle`/`season`/`episode` for an episode, `album`/`artist`/`track` for a
 * song — in the **same call**, no extra round-trip. Each is surfaced only where
 * it applies (copy_member_nonempty drops the `""`/`[]`/`-1` sentinels Kodi
 * returns for the other media types), so a movie carries none of them and every
 * status/transport reply reads richer ("playing S01E02 of …") for free. The
 * call also requests `albumartist`/`displayartist`: an untagged song reports a
 * blank `artist:[""]`, so copy_best_artist falls back to those computed names
 * (album/folder/scraper-derived) and records the best non-empty one as
 * `artist`, so the performer survives for poorly-tagged music too. The
 * real media type and library id (`media`/`id`) are surfaced too, under keys
 * distinct from `type` (which is spent on the player kind): both are identity
 * fields Kodi auto-injects into the same GetItem reply, so they cost nothing
 * extra. An off-library file reports `media` "unknown" and `id` -1; both are
 * kept as-is rather than dropped, so the history log can record an item as
 * unenriched.
 *
 * There is no single Kodi method for it, so it combines three calls:
 * `Player.GetActivePlayers` to find the active player, then
 * `Player.GetProperties` (speed/time/totaltime) and `Player.GetItem`
 * (file/label/title) on it.
 *
 * The response separates **how the player is doing** from **what is loaded**
 * (TODO 7.2): `state`/`type`/`time`/`totaltime`/`progress` stay at the top,
 * while the item-identity fields (media/id/file/label/title and the per-media
 * identifiers) are nested under a single `nowplaying` object. Position is also
 * made legible (TODO 7.1): alongside the bare `time`/`totaltime` clock objects a
 * derived `progress` string ("15:38 / 46:40 (33%)") spells out elapsed/total
 * and percent, so an assistant can answer "how much is left" without doing the
 * arithmetic. See append_progress().
 *
 * For a video it also summarizes the audio tracks and subtitles on offer
 * (TODO 7.3): top-level `audio` and `subtitle` objects each carry a `count` and
 * the live `current` selection (with the `index` the `set_audio_and_subtitle`
 * setter selects by) — a brief view, not the full per-stream dump
 * (`audio_and_subtitle_info` covers that). See append_av_streams().
 *
 * @return a newly allocated object node — `{ "state": "stopped" }` when nothing
 *         is active, else `{ "state": "playing"|"paused", "type"?, "time"?,
 *         "totaltime"?, "progress"?, "audio"?, "subtitle"?, "nowplaying":
 *         { "media"?, "id"?, "file"?, "label"?, "title"?, "showtitle"?,
 *         "season"?, "episode"?, "album"?, "artist"?, "track"? } }`
 *         (`nowplaying` whenever something is loaded, with `media`/`id` always
 *         and the per-media fields only where they apply; `audio`/`subtitle`
 *         only for video) — or NULL with @error set if a call fails. Example, a
 *         video paused 15:38 into 46:40 with two audio tracks and three
 *         subtitles:
 * @code{.json}
 * {
 *   "state": "paused",
 *   "type": "video",
 *   "time":      { "hours": 0, "minutes": 15, "seconds": 38, "milliseconds": 507 },
 *   "totaltime": { "hours": 0, "minutes": 46, "seconds": 40, "milliseconds": 633 },
 *   "progress":  "15:38 / 46:40 (33%)",
 *   "audio":     { "count": 2, "current": { "index": 0, "language": "eng", "codec": "ac3", "channels": 6 } },
 *   "subtitle":  { "count": 3, "enabled": true, "current": { "index": 1, "language": "hun" } },
 *   "nowplaying": {
 *     "media": "episode",
 *     "id":    1234,
 *     "file":  "/storage/Serials/Law & Order/Season 4/Law & Order 4x01.avi",
 *     "label": "Law & Order 4x01 Sweeps.avi",
 *     "title": "Sweeps",
 *     "showtitle": "Law & Order",
 *     "season":    4,
 *     "episode":   1
 *   }
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

  /* A video player also reports its audio-track / subtitle layout, summarized
   * into the snapshot by append_av_streams (TODO 7.3). Audio players carry none
   * of these properties — and asking for them on a non-video player can draw an
   * RPC error — so they are requested only for video. */
  gboolean is_video = g_strcmp0 (ptype, "video") == 0;
  static const char *const prop_fields_base[] = { "speed", "time", "totaltime",
                                                  NULL };
  static const char *const prop_fields_video[] = {
    "speed",       "time",          "totaltime",      "currentaudiostream",
    "audiostreams", "currentsubtitle", "subtitles",   "subtitleenabled",
    NULL
  };
  const char *const *prop_fields =
    is_video ? prop_fields_video : prop_fields_base;
  g_autoptr (JsonNode) pparams = player_props (playerid, prop_fields);
  g_autoptr (JsonNode) pres = mk_kodi_call (self->kodi, instance,
                                            "Player.GetProperties", pparams,
                                            error);
  if (pres == NULL)
    return NULL;
  JsonObject *props = json_node_get_object (pres);

  static const char *const item_fields[] = {
    "title", "file",
    /* History enrichment: episode and song identifiers, fetched in this same
     * call. `albumartist`/`displayartist` are the fallbacks copy_best_artist
     * uses when an untagged song's own `artist` tag is blank. */
    "showtitle", "season", "episode", "album", "artist", "albumartist",
    "displayartist", "track", NULL
  };
  g_autoptr (JsonNode) iparams = player_props (playerid, item_fields);
  g_autoptr (JsonNode) ires =
    mk_kodi_call (self->kodi, instance, "Player.GetItem", iparams, error);
  if (ires == NULL)
    return NULL;
  JsonObject *item =
    json_object_get_object_member (json_node_get_object (ires), "item");

  /* An off-library video — a Files-view play of a path the library indexed
   * under a different one — comes back type "unknown"/id -1. Recover its real
   * media type and metadata from the library by basename, so nowplaying,
   * status and the history log all read it as the episode/movie it is. Video
   * only (the chosen scope); audio players carry no such file mismatch in
   * practice and would only cost pointless lookups. */
  if (is_video)
    reconcile_off_library_item (self, instance, item);

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
  /* "How the player is doing" stays at the top: the bare clock objects plus the
   * legible `progress` clause derived from them (TODO 7.1). */
  copy_member (b, props, "time");
  copy_member (b, props, "totaltime");
  append_progress (b, props);
  /* A video's audio-track / subtitle layout, summarized (TODO 7.3): also "how
   * the player is doing" (the live selection is mutable player state), kept at
   * the top alongside position. Emitted for video only. */
  if (is_video)
    append_av_streams (b, props);

  /* "What is loaded" is nested under a single `nowplaying` object (TODO 7.2), so
   * the item identity is visibly distinct from the player status above. */
  json_builder_set_member_name (b, "nowplaying");
  json_builder_begin_object (b);
  /* The real media type and library id, surfaced under keys distinct
   * from `type` (which the snapshot spends on the player kind above). Both are
   * identity fields Kodi auto-injects into every Player.GetItem reply — free,
   * not requestable properties — so player_state() carries them too: status
   * and transport replies read richer ("episode 4838"), and the history log
   * copies them straight from this snapshot. An off-library file (a
   * `playfile` of a path absent from the library) reports media "unknown" and
   * id -1; both are recorded as-is so the log can mark it unenriched, so
   * unlike the per-media fields these are not dropped when empty. */
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
  /* Per-media enrichment: present only where it applies — empties and
   * -1 sentinels are dropped (copy_member_nonempty). */
  copy_member_nonempty (b, item, "showtitle");
  copy_member_nonempty (b, item, "season");
  copy_member_nonempty (b, item, "episode");
  copy_member_nonempty (b, item, "album");
  /* artist with album-artist / display-artist fallback for untagged rips. */
  copy_best_artist (b, item);
  copy_member_nonempty (b, item, "track");
  json_builder_end_object (b); /* nowplaying */
  json_builder_end_object (b); /* snapshot */
  JsonNode *root = json_builder_get_root (b);

  /* Feed the playback-history log from this very snapshot — the
   * single point every playback-affecting tool funnels through, so it is the
   * one place history needs to hook. Best-effort and dedup'd:
   * status/nowplaying re-observing the same item add nothing, and a logging failure
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
 * mk_tools_poll_history:
 * @self: the tool table.
 *
 * One round of live monitoring: take a player_state() snapshot
 * of **every** configured instance, purely for the side effect of that
 * snapshot feeding the playback-history log (mk_history_record, inside
 * player_state()). This is how the history also captures playback the server
 * did not cause — something started from the TV remote is observed on the
 * next round, and a repeat sighting of the same play merges instead of
 * duplicating, so the ~2-minute cadence costs nothing in log noise. With
 * several mcp-kodi processes polling the same boxes (one per client session),
 * the flock-serialized merge keeps recording idempotent.
 *
 * Errors are dropped without even a warning: an unreachable box is the normal
 * state of a powered-off TV, not a fault — polling it again next round *is*
 * the handling. An idle box costs one RPC (the active-players probe) and
 * never touches the log.
 */
void
mk_tools_poll_history (MkTools *self)
{
  g_return_if_fail (self != NULL);

  GList *names = mk_config_instance_names (self->config);
  for (GList *l = names; l != NULL; l = l->next)
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (JsonNode) snapshot =
        player_state (self, l->data, &error);
      (void) snapshot; /* wanted only for its history side effect */
    }
  g_list_free (names);
}

/**
 * app_audio_state:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @error: return location for a GError, or NULL.
 *
 * Builds the server's canonical audio snapshot for @instance — the **shared
 * response shape** returned by the tools whose effect is a change in the
 * application's audio output: mute/unmute and the volume Knob. As with
 * player_state(), reusing one builder keeps that response
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
 * volume snapshot — letting the AI nudge relatively without
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
 * @with_bounds: also emit the fixed `min`/`max` volume bounds.
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
 * mute/unmute: read the settled state, shape it. The volume Knob uses
 * app_audio_read()/audio_snapshot() directly so it can also report the
 * bounds and avoid a redundant round-trip.
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
 * Implements a Button: an argument-free remote keypress. Fires
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
 * wait_players:
 * @self: the tool table.
 * @instance: the target instance name, or NULL for the default.
 * @want_active: TRUE to wait until a player is active, FALSE until none is.
 *
 * Polls `Player.GetActivePlayers` until Kodi reports the wanted state, for at
 * most MK_PLAYER_WAIT_US. Kodi starts and tears down players asynchronously and
 * how long that takes varies widely: a file opening from a cold network share
 * (typically the first play after Kodi starts) can take seconds where a warm one
 * takes a fraction of one. A fixed settle delay therefore lets a snapshot read
 * the pre-action state — "playing" right after stop, or no player right after an
 * open that is merely slow, which open_file() would misreport as "never
 * started". Times out quietly: the caller's snapshot then reports whatever Kodi
 * says, which is the honest answer. Called from handler_stop() and open_file().
 */
#define MK_PLAYER_WAIT_US (5 * G_USEC_PER_SEC) /* upper bound on start/teardown */
#define MK_PLAYER_POLL_US (100 * 1000)         /* poll interval while waiting */

static void
wait_players (MkTools *self, const char *instance, gboolean want_active)
{
  gint64 deadline = g_get_monotonic_time () + MK_PLAYER_WAIT_US;

  for (;;)
    {
      g_autoptr (GError) ignored = NULL;
      g_autoptr (JsonNode) active = mk_kodi_call (self->kodi, instance,
                                                  "Player.GetActivePlayers",
                                                  NULL, &ignored);
      if (active == NULL)
        return; /* the snapshot that follows reports the failure */
      guint n = JSON_NODE_HOLDS_ARRAY (active)
                  ? json_array_get_length (json_node_get_array (active))
                  : 0;
      if ((n > 0) == want_active)
        return;
      if (g_get_monotonic_time () >= deadline)
        return;
      g_usleep (MK_PLAYER_POLL_US);
    }
}

/**
 * handler_stop:
 * @self: the tool table.
 * @def: this Button's table row; @def->action is "stop".
 * @args: the call arguments (optional `instance`), or NULL.
 * @error: return location for a GError, or NULL.
 *
 * Implements the stop Button: the handler_button() keypress plus an
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
 *   2. `Input.ExecuteAction { "action": "stop" }`, then wait until Kodi
 *      reports no active player (wait_players()) rather than the fixed
 *      handler_button() settle, so the snapshot never reads a player that is
 *      still tearing down.
 *   3. `Playlist.Clear { playlistid }` on the playlist from step 1.
 *
 * Other playlists are left alone — clearing *everything* is `dropplaylists`'
 * job; stop touches only what it stopped.
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
  if (players != NULL && json_array_get_length (players) > 0)
    wait_players (self, instance, FALSE);
  else
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
 * Implements the nowplaying Button: unlike handler_button() it sends no
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
 * Implements the mute and unmute Buttons. Unlike the transport
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
 * Schema for the volume Knob: the optional `instance` plus an
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
 * Implements the volume Knob as a **relative** adjustment, never an
 * absolute set: the AI cannot safely pick a level it has not read, so it nudges
 * from the box's live volume. The `step` argument is a signed integer — 0 (or
 * omitted) reads the current state and changes nothing (the audio analogue of
 * nowplaying), `+N`/`−N` raises/lowers by N percentage points.
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
 * Builds the **shared response shape** of the `instances` config tool,
 * returned by all three actions so the caller always sees the resulting config
 * state. Lists each configured instance — `{ key, name?, host, scheme, insecure,
 * has_auth, allow_rpc }` in sorted key order — alongside the `default` key.
 * Credentials are deliberately reduced to the boolean `has_auth`: the password
 * is write-only and never leaves the server. `allow_rpc` is reported read-only
 * so the caller can see which boxes permit the `rpc` escape hatch, but it
 * cannot be changed here — only by hand-editing the config file. `build` is
 * the server's build ID (mk_build_id()), so a caller can tell whether the
 * running binary matches the source it expects.
 *
 * @return a newly allocated object node
 *         `{ "default": <key>|null, "instances": [ … ], "build": <id> }`.
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

  json_builder_set_member_name (b, "build");
  json_builder_add_string_value (b, mk_build_id ());

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
 * Implements the `instances` config tool: read/write the server's own
 * instance config, making no Kodi call. `get` lists instances; `set`
 * upserts one by `key` (provided fields override, omitted fields keep the
 * current value or take defaults) and may make it the default; `remove` deletes
 * one, refusing the current default so the config is never left defaultless.
 * `set`/`remove` persist atomically (mk_config_save()). Every action returns
 * instances_list().
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
      /* allow_rpc is deliberately never read from @args: the escape-hatch
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

/* ---- searchmedia ----------------------------------------------------------
 *
 * One generic find-by-name tool that drills each media type's hierarchy down to
 * playable **leaf files**. The key design choice is to descend to the
 * album/season level only when the user names one, so every *wide* query (bare
 * artist, bare show) is a single Kodi call whose `limits {start, end, total}`
 * maps straight onto the tool's `total`/`limit`/`offset`. The one multi-call
 * case — a substring album `title` matching several albums — is collected
 * app-side and flagged `approximate`.
 *
 * Video queries can also name a person: optional `actor`/`director` become
 * server-side `{field, operator: contains}` rules, and-combined with the
 * title rule when both are given. For
 * tv-shows the person rules go on GetEpisodes, not the show resolve: episode
 * cast holds the regulars AND the guest stars (verified live — a lead matches
 * every episode), and show-level director data is empty in practice, so the
 * episode level is the one place both "show with X" and "episodes with guest
 * X" work. A person-only tv query (no `title`) skips the show resolve and
 * filters episodes library-wide in one call; its rows carry `showtitle` so
 * each hit names its show.
 */

#define MK_SEARCH_DEFAULT_LIMIT 50  /* default leaf cap when `limit` omitted */
#define MK_SEARCH_MAX_LIMIT     500 /* hard ceiling on `limit` */
#define MK_SEARCH_RESOLVE_SCAN  25  /* candidates fetched when resolving a name */

/* Leaf-row members forwarded from a Kodi item into a result row, when present.
 * A superset across the three types — copy_member() skips the ones a given item
 * lacks, so one list serves songs, episodes, and movies. `file` is the playfile
 * input; the per-type id (`songid`/`episodeid`/`movieid`) is the library handle.
 * `showtitle` appears only on the person-only tv path, where no resolved show
 * names the container. */
static const char *const mk_search_row_fields[] = {
  "songid", "episodeid", "movieid", "label", "title", "file",
  "track", "season", "episode", "year", "artist", "album",
  "duration", "runtime", "firstaired", "showtitle", NULL
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

/* One field/operator/value filter rule, collected before building so several
 * can be and-combined into a single Kodi `filter`. */
typedef struct
{
  const char *field;
  const char *op;
  const char *value;
} MkFilterRule;

/**
 * build_filter_rule:
 * @b: the builder, positioned where the rule object goes.
 * @rule: the rule to emit.
 *
 * Emits one `{ "field", "operator", "value" }` rule object.
 */
static void
build_filter_rule (JsonBuilder *b, const MkFilterRule *rule)
{
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "field");
  json_builder_add_string_value (b, rule->field);
  json_builder_set_member_name (b, "operator");
  json_builder_add_string_value (b, rule->op);
  json_builder_set_member_name (b, "value");
  json_builder_add_string_value (b, rule->value);
  json_builder_end_object (b);
}

/**
 * add_filter_rules:
 * @b: the builder, positioned inside a params object.
 * @rules: the collected rules.
 * @n: how many rules; 0 adds no filter at all.
 *
 * Adds the `"filter"` member: a bare `{ "field", "operator", "value" }` rule
 * when there is one, or the composite
 * `{ "and": [ rule, … ] }` form when several must hold at once (verified live
 * on GetMovies/GetTVShows/GetEpisodes).
 */
static void
add_filter_rules (JsonBuilder *b, const MkFilterRule *rules, gsize n)
{
  if (n == 0)
    return;
  json_builder_set_member_name (b, "filter");
  if (n == 1)
    {
      build_filter_rule (b, &rules[0]);
      return;
    }
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "and");
  json_builder_begin_array (b);
  for (gsize i = 0; i < n; i++)
    build_filter_rule (b, &rules[i]);
  json_builder_end_array (b);
  json_builder_end_object (b);
}

/**
 * append_rule:
 * @rules: the rule array being collected.
 * @n: rules collected so far.
 * @field: the field name to match (e.g. "title", "actor", "director").
 * @op: the operator ("contains" for substring, "is" for exact).
 * @value: the value to match, or NULL/"" to skip the rule.
 *
 * Appends a rule when @value is non-empty — the collection step for the
 * optional search arguments. @value is borrowed; it must outlive the array.
 *
 * @return the new rule count.
 */
static gsize
append_rule (MkFilterRule *rules, gsize n, const char *field, const char *op,
             const char *value)
{
  if (value == NULL || value[0] == '\0')
    return n;
  rules[n].field = field;
  rules[n].op = op;
  rules[n].value = value;
  return n + 1;
}

/**
 * add_field_filter:
 * @b: the builder, positioned inside a params object.
 * @field: the field name to match (e.g. "title", "artist", "episode").
 * @op: the operator ("contains" for substring, "is" for exact).
 * @value: the value to match (always a string, even for numeric fields).
 *
 * Adds a single-rule `"filter": { "field", "operator", "value" }`.
 */
static void
add_field_filter (JsonBuilder *b, const char *field, const char *op,
                  const char *value)
{
  MkFilterRule rule = { field, op, value };
  add_filter_rules (b, &rule, 1);
}

/**
 * add_id_filter:
 * @b: the builder, positioned inside a params object.
 * @key: the special filter key ("artistid" or "albumid").
 * @id: the library id to filter by.
 *
 * Adds Kodi's special id `"filter": { "<key>": <id> }` form
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
 * MkReconcileKind:
 * @method: the library list method to query (VideoLibrary.GetEpisodes, …).
 * @list_key: the array member its reply carries ("episodes"/"movies"/…).
 * @id_field: the library-id field on each row ("episodeid"/"movieid"/…).
 * @media: the media type to stamp on a matched item ("episode"/"movie"/…).
 * @props: NULL-terminated enrichment properties to request and copy across.
 *
 * One video media type the off-library reconciler (reconcile_off_library_item)
 * tries, in order, when matching a played file's basename against the library.
 */
typedef struct
{
  const char *method;
  const char *list_key;
  const char *id_field;
  const char *media;
  const char *const *props;
} MkReconcileKind;

/**
 * reconcile_off_library_item:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @item: the Player.GetItem `item` object, patched in place on a hit.
 *
 * Recovers the real media identity of an off-library video. When a file is
 * played by path (the everyday "browse to it in Files and press play") and that
 * exact path is not the one the library scraped — a duplicate copy, an extra
 * directory level, a differently-mounted share — Kodi cannot tie it to a library
 * row and reports `type:"unknown"`, `id:-1`, with empty `title`/`showtitle` and
 * `-1` season/episode. The snapshot (and so status, nowplaying and the history
 * log) would then record it as unenriched, and a `media:"episode"` history
 * filter would miss it.
 *
 * Kodi's library matches the `filename` field on basename, not full path, so one
 * indexed `filter:{field:"filename",operator:"is",value:<basename>}` query
 * reverse-maps the played file to its library row even when the directories
 * differ. This tries each video type in turn (episode, movie, music video) and,
 * on a UNIQUE match, stamps the real `type`/`id` onto @item and copies the
 * type's enrichment fields across — so the rest of player_state reads it exactly
 * as if Kodi had resolved it. A basename shared by more than one library row is
 * ambiguous and left untouched (better unknown than mislabelled). No-ops unless
 * @item is type "unknown" with a non-empty `file`; a failed lookup is swallowed
 * (best-effort enrichment must never break the snapshot).
 */
static void
reconcile_off_library_item (MkTools *self, const char *instance,
                            JsonObject *item)
{
  if (item == NULL)
    return;
  const char *type =
    json_object_get_string_member_with_default (item, "type", "unknown");
  if (g_strcmp0 (type, "unknown") != 0)
    return; /* Kodi already classified it — trust that. */
  const char *file =
    json_object_get_string_member_with_default (item, "file", NULL);
  if (file == NULL || file[0] == '\0')
    return;
  g_autofree char *base = g_path_get_basename (file);
  if (base == NULL || base[0] == '\0' || g_strcmp0 (base, ".") == 0
      || g_strcmp0 (base, G_DIR_SEPARATOR_S) == 0)
    return;

  static const char *const ep_props[] = { "showtitle", "season", "episode",
                                          "title", NULL };
  static const char *const mv_props[] = { "title", NULL };
  static const char *const mvid_props[] = { "title", "artist", NULL };
  static const MkReconcileKind kinds[] = {
    { "VideoLibrary.GetEpisodes", "episodes", "episodeid", "episode", ep_props },
    { "VideoLibrary.GetMovies", "movies", "movieid", "movie", mv_props },
    { "VideoLibrary.GetMusicVideos", "musicvideos", "musicvideoid",
      "musicvideo", mvid_props },
  };

  for (gsize k = 0; k < G_N_ELEMENTS (kinds); k++)
    {
      g_autoptr (JsonBuilder) pb = json_builder_new ();
      json_builder_begin_object (pb);
      add_field_filter (pb, "filename", "is", base);
      add_properties (pb, kinds[k].props);
      /* Window of 2 so an ambiguous basename still reports its full `total`. */
      add_page_limits (pb, 0, 2, FALSE);
      json_builder_end_object (pb);
      g_autoptr (JsonNode) params = json_builder_get_root (pb);

      /* Best-effort: a lookup failure leaves the item unknown, never errors. */
      g_autoptr (JsonNode) result =
        mk_kodi_call (self->kodi, instance, kinds[k].method, params, NULL);
      if (result == NULL || !JSON_NODE_HOLDS_OBJECT (result))
        continue;
      if (list_total (result) != 1) /* no match, or ambiguous → skip */
        continue;
      JsonObject *robj = json_node_get_object (result);
      if (!json_object_has_member (robj, kinds[k].list_key))
        continue;
      JsonArray *rows = json_object_get_array_member (robj, kinds[k].list_key);
      if (rows == NULL || json_array_get_length (rows) < 1)
        continue;
      JsonObject *row = json_array_get_object_element (rows, 0);

      /* Stamp the recovered identity and graft the enrichment onto the snapshot
       * item, replacing the unknown/sentinel placeholders Kodi left. */
      json_object_set_string_member (item, "type", kinds[k].media);
      json_object_set_int_member (
        item, "id",
        json_object_get_int_member_with_default (row, kinds[k].id_field, -1));
      for (gsize p = 0; kinds[k].props[p] != NULL; p++)
        {
          const char *name = kinds[k].props[p];
          if (json_object_has_member (row, name))
            json_object_set_member (
              item, name, json_node_copy (json_object_get_member (row, name)));
        }
      return;
    }
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
 * Builds the shared `searchmedia` response shape: `{ type, total, returned,
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
 * query_songs_by_rules:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @rules: the field rules to and-combine (title/artist/album/tracknumber).
 * @nrules: how many of @rules are populated (must be >= 1).
 * @offset: paging offset.
 * @limit: paging limit.
 * @count: count-only request.
 * @error: return location for a GError, or NULL.
 *
 * Runs `AudioLibrary.GetSongs` matched by a composite field filter rather than a
 * resolved container id — the song-title path, where a song is found by its own
 * title (optionally narrowed by artist/album/track) library-wide, sorted by
 * title, with server-side paging. The shared song properties omit `artist`, so
 * the title filter keeps the set small enough to dodge the sort-with-artist
 * slowdown (see the GetSongs note in the project memory).
 *
 * @return the Kodi `result` node (free with json_node_unref()), or NULL with
 *         @error set.
 */
static JsonNode *
query_songs_by_rules (MkTools *self, const char *instance,
                      const MkFilterRule *rules, gsize nrules, gint64 offset,
                      gint64 limit, gboolean count, GError **error)
{
  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  add_filter_rules (pb, rules, nrules);
  add_properties (pb, mk_search_song_props);
  add_sort (pb, "title");
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
 * @actor: cast-member name substring, or NULL/"" for no person filter.
 * @director: director name substring, or NULL/"" for no person filter.
 * @limit: max rows.
 * @offset: rows to skip.
 * @count: count-only request.
 * @error: return location for a GError, or NULL.
 *
 * Resolves the movie type directly: `VideoLibrary.GetMovies` returns the
 * playable `file` with no sublevels, so this is one paged call.
 * Title and person rules are and-combined server-side.
 *
 * @return the search-result node, or NULL with @error set.
 */
static JsonNode *
search_movie (MkTools *self, const char *instance, const char *title,
              const char *actor, const char *director, gint64 limit,
              gint64 offset, gboolean count, GError **error)
{
  static const char *const props[] = { "title",   "year",      "genre",
                                       "rating",  "runtime",   "playcount",
                                       "file",    NULL };

  MkFilterRule rules[3];
  gsize nrules = 0;
  nrules = append_rule (rules, nrules, "title", "contains", title);
  nrules = append_rule (rules, nrules, "actor", "contains", actor);
  nrules = append_rule (rules, nrules, "director", "contains", director);

  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  add_filter_rules (pb, rules, nrules);
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
 * @title: show-name substring, or NULL/"" for a person/episode-title query.
 * @actor: cast-member name substring, or NULL/"" for no person filter.
 * @director: episode-director name substring, or NULL/"" for no person filter.
 * @episodetitle: episode-title substring (§11.6.4.1.3), or NULL/"" — an episode
 *        is matched by its OWN title, library-wide unless @title narrows it.
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
 * number is given — its value is a string). One paged call
 * past the resolve. The person and episode-title rules ride the same
 * GetEpisodes call — episode cast covers regulars and guest stars alike, and
 * per-episode directors are the only populated director data. Without a @title
 * the resolve is skipped entirely and one library-wide GetEpisodes call filters
 * by person/episode-title, sorted by show; those rows carry `showtitle` instead
 * of a `resolved` container, and @season/@number make no sense show-less, so
 * they are rejected.
 *
 * @return the search-result node, or NULL with @error set (no title/actor/
 *         director/episodetitle, season/number without title, or a call
 *         failure); an unresolved show yields a clean zero-total result.
 */
static JsonNode *
search_tv (MkTools *self, const char *instance, const char *title,
           const char *actor, const char *director, const char *episodetitle,
           gboolean have_season, gint64 season, gboolean have_number,
           gint64 number, gint64 limit, gint64 offset, gboolean count,
           GError **error)
{
  static const char *const props[] = { "title",      "season",  "episode",
                                       "file",       "firstaired",
                                       "runtime",    NULL };
  /* The library-wide person path adds `showtitle` — there is no resolved
   * show to name the container. */
  static const char *const gprops[] = { "title",      "season",  "episode",
                                        "file",       "firstaired",
                                        "runtime",    "showtitle", NULL };

  gboolean have_title = (title != NULL && title[0] != '\0');
  gboolean have_person = (actor != NULL && actor[0] != '\0')
                         || (director != NULL && director[0] != '\0');
  gboolean have_eptitle = (episodetitle != NULL && episodetitle[0] != '\0');

  if (!have_title && !have_person && !have_eptitle)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "searchmedia tv-show: \"title\" (show name), \"episodetitle\","
                   " \"actor\" or \"director\" is required");
      return NULL;
    }
  if (!have_title && (have_season || have_number))
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "searchmedia tv-show: \"season\"/\"number\" need a show — "
                   "give \"title\" too");
      return NULL;
    }

  gint64 showid = 0;
  g_autofree char *show = NULL;
  if (have_title)
    {
      int r = resolve_container (self, instance, "VideoLibrary.GetTVShows",
                                 "title", title, "tvshows", "tvshowid", &showid,
                                 &show, error);
      if (r < 0)
        return NULL;
      if (r == 0)
        return build_search_result ("tv-show", NULL, NULL, NULL, 0, FALSE, 0,
                                    offset, FALSE, NULL, count);
    }

  g_autofree char *ns =
    have_number ? g_strdup_printf ("%" G_GINT64_FORMAT, number) : NULL;
  MkFilterRule rules[4];
  gsize nrules = 0;
  nrules = append_rule (rules, nrules, "title", "contains", episodetitle);
  nrules = append_rule (rules, nrules, "episode", "is", ns);
  nrules = append_rule (rules, nrules, "actor", "contains", actor);
  nrules = append_rule (rules, nrules, "director", "contains", director);

  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  if (have_title)
    {
      json_builder_set_member_name (pb, "tvshowid");
      json_builder_add_int_value (pb, showid);
    }
  if (have_season)
    {
      json_builder_set_member_name (pb, "season");
      json_builder_add_int_value (pb, season);
    }
  add_filter_rules (pb, rules, nrules);
  add_properties (pb, have_title ? props : gprops);
  add_sort (pb, have_title ? "episode" : "tvshowtitle");
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
  return build_search_result ("tv-show", "show", show, "tvshowid", showid,
                              have_title, total, offset, FALSE, rows, count);
}

/**
 * search_music:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @artist: performer substring, or NULL/"" to anchor on the album title.
 * @title: album-name substring, or NULL/"" to span every album.
 * @song: song-title substring, or NULL/"" — when given, takes the song-title
 *        path (§11.6.4.1.5) regardless of artist/album.
 * @have_number: whether @number (track) was given.
 * @number: track number to keep.
 * @limit: max rows.
 * @offset: rows to skip.
 * @count: count-only request.
 * @error: return location for a GError, or NULL.
 *
 * When @song is given, songs are matched by their OWN title (§11.6.4.1.5): one
 * server-side `GetSongs` call with an and-combined `{title, artist?, album?,
 * tracknumber?}` field filter, library-wide and no container resolve, so
 * `total`/paging come straight from Kodi and `resolved` is absent. This is the
 * only path that anchors on the song; artist/album/number narrow it instead of
 * resolving a container.
 *
 * Otherwise the search resolves the artist, then drills to song leaves. The wide
 * paths — artist-only, or a single matched album, with no track filter — are one
 * paged `GetSongs {artistid|albumid}` call, so `total`/`limit`/`offset` come
 * straight from Kodi. A track filter, or an album substring matching several
 * albums, falls back to fetching the songs and paging app-side; the multi-album
 * case is flagged `approximate`.
 *
 * Without an artist (and without @song) the album title anchors the search
 * instead: it is resolved library-wide to ONE album (`GetAlbums` with a
 * server-side `album contains` rule, exact match preferred — the same
 * single-container resolve tv-show applies to its title), and the songs drill
 * from that `albumid`; `resolved` then carries `album`/`albumid`. At least one
 * of @artist/@title/@song must be given.
 *
 * @return the search-result node, or NULL with @error set (no anchor field, or
 *         a call failure); an unresolved artist or album yields a clean
 *         zero-total result.
 */
static JsonNode *
search_music (MkTools *self, const char *instance, const char *artist,
              const char *title, const char *song, gboolean have_number,
              gint64 number, gint64 limit, gint64 offset, gboolean count,
              GError **error)
{
  gboolean have_artist = (artist != NULL && artist[0] != '\0');
  gboolean have_song = (song != NULL && song[0] != '\0');

  if (!have_artist && !have_song && (title == NULL || title[0] == '\0'))
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "searchmedia music: \"artist\", \"title\" or \"song\" "
                   "is required");
      return NULL;
    }

  /* Song-title path: anchor on the song's own title, matched server-side and
   * narrowed (not container-resolved) by artist/album/track. One GetSongs call,
   * library-wide, so no GetArtists/GetAlbums and no `resolved` container. */
  if (have_song)
    {
      MkFilterRule rules[4];
      gsize nrules = 0;
      g_autofree char *ns =
        have_number ? g_strdup_printf ("%" G_GINT64_FORMAT, number) : NULL;
      nrules = append_rule (rules, nrules, "title", "contains", song);
      nrules = append_rule (rules, nrules, "artist", "contains", artist);
      nrules = append_rule (rules, nrules, "album", "contains", title);
      nrules = append_rule (rules, nrules, "tracknumber", "is", ns);

      g_autoptr (JsonNode) result = query_songs_by_rules (
        self, instance, rules, nrules, offset, limit, count, error);
      if (result == NULL)
        return NULL;
      gint64 total = list_total (result);
      g_autoptr (GPtrArray) rows = count ? NULL : collect_items (result, "songs");
      return build_search_result ("music", NULL, NULL, NULL, 0, FALSE, total,
                                  offset, FALSE, rows, count);
    }

  /* Album-anchored path: no artist, so the album title is the anchor —
   * resolve it library-wide to one album, then drill its songs. */
  if (!have_artist)
    {
      gint64 albumid = 0;
      g_autofree char *albname = NULL;
      int r = resolve_container (self, instance, "AudioLibrary.GetAlbums",
                                 "album", title, "albums", "albumid", &albumid,
                                 &albname, error);
      if (r < 0)
        return NULL;
      if (r == 0)
        return build_search_result ("music", NULL, NULL, NULL, 0, FALSE, 0,
                                    offset, FALSE, NULL, count);

      if (!have_number)
        {
          g_autoptr (JsonNode) result = query_songs (self, instance, "albumid",
                                                     albumid, TRUE, offset,
                                                     limit, count, error);
          if (result == NULL)
            return NULL;
          gint64 total = list_total (result);
          g_autoptr (GPtrArray) rows =
            count ? NULL : collect_items (result, "songs");
          return build_search_result ("music", "album", albname, "albumid",
                                      albumid, TRUE, total, offset, FALSE, rows,
                                      count);
        }

      /* Track filter: fetch the album's songs, keep the track, page. */
      g_autoptr (JsonNode) result = query_songs (self, instance, "albumid",
                                                 albumid, FALSE, 0, 0, FALSE,
                                                 error);
      if (result == NULL)
        return NULL;
      g_autoptr (GPtrArray) items = collect_items (result, "songs");
      g_autoptr (GPtrArray) tracks = filter_track (items, number);
      gint64 total = tracks->len;
      g_autoptr (GPtrArray) page =
        count ? NULL : slice_items (tracks, offset, limit);
      return build_search_result ("music", "album", albname, "albumid", albumid,
                                  TRUE, total, offset, FALSE, page, count);
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
 * Schema for the `searchmedia` tool: the `type` selector plus the
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
              "Resolves the artist; music needs artist, title or song.");
  prop_typed (b, "song", "string",
              "Music only: song title (substring, case-insensitive). Matches "
              "songs by their OWN name library-wide — the way to find a track "
              "without its album; narrow with artist/title(album)/number. "
              "Rows carry no resolved container.");
  prop_typed (b, "actor", "string",
              "movie/tv-show only: cast-member name (substring, "
              "case-insensitive). For tv-show it matches episode cast, which "
              "includes guest stars; combinable with title.");
  prop_typed (b, "director", "string",
              "movie/tv-show only: director name (substring, "
              "case-insensitive). For tv-show it matches per-episode "
              "directors; combinable with title.");
  prop_typed (b, "episodetitle", "string",
              "tv-show only: an episode's OWN title (substring, "
              "case-insensitive) — the way to find an episode by name. Matched "
              "library-wide (rows carry showtitle) unless a show title also "
              "narrows it. This is the title, NOT the episode number (use "
              "number for that).");
  prop_typed (b, "title", "string",
              "Container/title to match (substring, case-insensitive): album "
              "for music (a song's own title is the separate `song` arg), show "
              "for tv-show, the movie title for movie. tv-show needs title, "
              "actor or director; without title the person is matched "
              "library-wide and rows carry showtitle. Music without artist "
              "resolves the album library-wide.");
  prop_typed (b, "season", "integer",
              "tv-show only: season number to narrow the episodes.");
  prop_typed (b, "number", "integer",
              "Position within the container: track number (music) or episode "
              "number (tv-show) — for an episode's title use episodetitle.");
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
 * Implements the `searchmedia` tool: reads the drill fields and paging
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
                   "searchmedia: \"type\" is required (music/tv-show/movie)");
      return NULL;
    }

  const char *title = arg_str (args, "title", NULL);
  const char *artist = arg_str (args, "artist", NULL);
  const char *song = arg_str (args, "song", NULL);
  const char *actor = arg_str (args, "actor", NULL);
  const char *director = arg_str (args, "director", NULL);
  const char *episodetitle = arg_str (args, "episodetitle", NULL);
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
    return search_movie (self, instance, title, actor, director, limit, offset,
                         count, error);
  if (g_str_equal (type, "tv-show"))
    return search_tv (self, instance, title, actor, director, episodetitle,
                      have_season, season, have_number, number, limit, offset,
                      count, error);
  if (g_str_equal (type, "music"))
    return search_music (self, instance, artist, title, song, have_number,
                         number, limit, offset, count, error);

  g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
               "searchmedia: unknown type \"%s\" (use music/tv-show/movie)", type);
  return NULL;
}

/* ---- contributors ----------------------------------------------------------
 *
 * Find or list *people* — bands, solo artists, composers, actors, directors —
 * instead of media. Each row is `{ name, in: [containers] }`: the exact name IS
 * the follow-up handle (`searchmedia` takes names, and Kodi's person filters are
 * name-only — no id-based filter exists), and `in` lists where feeding it back
 * will hit (albums/songs for music people, movies/tvshows for cast and crew),
 * so the caller knows where to drill next without Kodi-specific knowledge.
 *
 * Both arguments are optional: `name` substring-matches, `type` narrows to one
 * contributor kind, and omitting both enumerates the whole merged people set —
 * the node matching is app-side anyway (see below), so an empty needle costs
 * the same as a one-letter one. The `band` type is SYNTHESIZED: Kodi's native
 * group/person notion (the MusicBrainz-fed `artisttype`/`gender` artist
 * properties) is empty on real tag-scraped libraries (verified live), so
 * "band" means album-level artist (`albumartistsonly: true`) — headline
 * artists incl. solo ones, the set "list my bands" actually wants — TODO
 * 11.6.4.4.
 *
 * Music people come from one server-filtered `AudioLibrary.GetArtists` call
 * (`allroles` folds in composers, conductors, lyricists; the `composer` type
 * is a server-side `{role is "Composer"}` rule).
 * Actors and directors have no Get* method at all, so they are enumerated
 * through the virtual `videodb://` nodes with `Files.GetDirectory` — those
 * listings take sort and limits but NO filter, so name
 * matching is app-side over the paged labels. Hits from every source are
 * merged case-insensitively into one row per person.
 */

#define MK_CONTRIB_NODE_PAGE 1000 /* labels per Files.GetDirectory page */

/* Container bits for a row's `in` list — where the person's name yields hits
 * when fed back into `searchmedia`. Bits make the cross-source merge a cheap OR;
 * mk_contrib_containers spells them out in emission order. */
enum
{
  MK_CONTRIB_ALBUMS  = 1 << 0,
  MK_CONTRIB_SONGS   = 1 << 1,
  MK_CONTRIB_MOVIES  = 1 << 2,
  MK_CONTRIB_TVSHOWS = 1 << 3,
};

/* Contributor kinds the optional `type` argument can narrow to. ANY (no
 * `type`) spans every source; the rest pick the matching subset. */
enum
{
  MK_CONTRIB_TYPE_ANY = 0,
  MK_CONTRIB_TYPE_BAND,     /* album-level music artists (synthesized) */
  MK_CONTRIB_TYPE_COMPOSER, /* role "Composer" music contributors */
  MK_CONTRIB_TYPE_ACTOR,    /* movie + tv-show cast nodes */
  MK_CONTRIB_TYPE_DIRECTOR, /* movie directors node */
};

static const struct
{
  const char *name;
  guint       type;
} mk_contrib_types[] = {
  { "band",     MK_CONTRIB_TYPE_BAND     },
  { "composer", MK_CONTRIB_TYPE_COMPOSER },
  { "actor",    MK_CONTRIB_TYPE_ACTOR    },
  { "director", MK_CONTRIB_TYPE_DIRECTOR },
};

static const struct
{
  guint       bit;
  const char *label;
} mk_contrib_containers[] = {
  { MK_CONTRIB_ALBUMS,  "albums"  },
  { MK_CONTRIB_SONGS,   "songs"   },
  { MK_CONTRIB_MOVIES,  "movies"  },
  { MK_CONTRIB_TVSHOWS, "tvshows" },
};

/* One merged contributor: the display name (first spelling seen wins), its
 * casefolded form (the merge/sort key, borrowed by the handler's index hash),
 * and the container bits accumulated across the sources. */
typedef struct
{
  char  *name;
  char  *key;
  guint  in;
} MkContribRow;

/**
 * contrib_row_free:
 * @row: the row to free.
 *
 * Frees one MkContribRow — the rows array's element free func.
 */
static void
contrib_row_free (gpointer row)
{
  MkContribRow *r = row;
  g_free (r->name);
  g_free (r->key);
  g_free (r);
}

/**
 * contrib_add:
 * @index: casefolded name → row lookup (keys borrowed from the rows).
 * @rows: the row store (owns the rows).
 * @name: the person's display name as this source spells it.
 * @in: the container bit(s) this source contributes.
 *
 * Records that @name yields hits @in, merging case-insensitively into an
 * existing row when the person was already seen through another source (or the
 * same source under a different spelling of the case).
 */
static void
contrib_add (GHashTable *index, GPtrArray *rows, const char *name, guint in)
{
  g_autofree char *key = g_utf8_casefold (name, -1);
  MkContribRow *row = g_hash_table_lookup (index, key);
  if (row == NULL)
    {
      row = g_new0 (MkContribRow, 1);
      row->name = g_strdup (name);
      row->key = g_steal_pointer (&key);
      g_ptr_array_add (rows, row);
      g_hash_table_insert (index, row->key, row);
    }
  row->in |= in;
}

/**
 * contrib_row_cmp:
 * @a: pointer to the first MkContribRow*.
 * @b: pointer to the second MkContribRow*.
 *
 * Orders rows by their casefolded name, so the merged set pages
 * deterministically regardless of which source produced each row.
 *
 * @return the strcmp-style ordering of the two rows' keys.
 */
static gint
contrib_row_cmp (gconstpointer a, gconstpointer b)
{
  const MkContribRow *ra = *(MkContribRow *const *) a;
  const MkContribRow *rb = *(MkContribRow *const *) b;
  return g_strcmp0 (ra->key, rb->key);
}

/**
 * contrib_music:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @type: MK_CONTRIB_TYPE_ANY, _BAND or _COMPOSER — which music people to ask
 *        Kodi for.
 * @name: the person-name substring to match (case-insensitive), or "" to
 *        match everyone (no name rule is sent).
 * @index: casefolded name → row lookup, fed to contrib_add().
 * @rows: the row store, fed to contrib_add().
 * @error: return location for a GError, or NULL.
 *
 * Collects the matching music people in one server-filtered
 * `AudioLibrary.GetArtists` call. ANY: `allroles: true` folds in artists known
 * only as composers/conductors/lyricists and
 * `albumartistsonly: false` keeps song-only artists regardless of the GUI
 * setting. BAND: `albumartistsonly: true` instead — the album-level artists,
 * Kodi's closest notion of a band. COMPOSER: a server-side
 * `{role is "Composer"}` rule (needs `allroles: true`), and-combined with the
 * name rule when both apply (verified live). Every hit
 * drills to `songs`; only an `isalbumartist` also drills to `albums` (a
 * GetAlbums artist filter finds nothing for the rest).
 *
 * @return TRUE on success (even with zero matches), FALSE with @error set on a
 *         call failure.
 */
static gboolean
contrib_music (MkTools *self, const char *instance, guint type,
               const char *name, GHashTable *index, GPtrArray *rows,
               GError **error)
{
  static const char *const props[] = { "isalbumartist", NULL };

  MkFilterRule rules[2];
  gsize nrules = 0;
  if (type == MK_CONTRIB_TYPE_COMPOSER)
    nrules = append_rule (rules, nrules, "role", "is", "Composer");
  nrules = append_rule (rules, nrules, "artist", "contains", name);

  g_autoptr (JsonBuilder) pb = json_builder_new ();
  json_builder_begin_object (pb);
  add_filter_rules (pb, rules, nrules);
  json_builder_set_member_name (pb, "albumartistsonly");
  json_builder_add_boolean_value (pb, type == MK_CONTRIB_TYPE_BAND);
  json_builder_set_member_name (pb, "allroles");
  json_builder_add_boolean_value (pb, type != MK_CONTRIB_TYPE_BAND);
  add_properties (pb, props);
  add_sort (pb, "artist");
  json_builder_end_object (pb);
  g_autoptr (JsonNode) params = json_builder_get_root (pb);

  g_autoptr (JsonNode) result = mk_kodi_call (self->kodi, instance,
                                              "AudioLibrary.GetArtists", params,
                                              error);
  if (result == NULL)
    return FALSE;

  g_autoptr (GPtrArray) items = collect_items (result, "artists");
  for (guint i = 0; i < items->len; i++)
    {
      JsonObject *it = g_ptr_array_index (items, i);
      const char *label = json_object_get_string_member_with_default (
        it, "artist",
        json_object_get_string_member_with_default (it, "label", NULL));
      if (label == NULL || label[0] == '\0')
        continue;
      guint in = MK_CONTRIB_SONGS;
      if (json_object_get_boolean_member_with_default (it, "isalbumartist",
                                                       FALSE))
        in |= MK_CONTRIB_ALBUMS;
      contrib_add (index, rows, label, in);
    }
  return TRUE;
}

/**
 * contrib_node:
 * @self: the tool table.
 * @instance: target instance, or NULL for the default.
 * @directory: the virtual people node to enumerate (a videodb:// path).
 * @in: the container bit a hit in this node drills to.
 * @name: the person-name substring to match (case-insensitive), or "" to
 *        match every label (an empty needle occurs in any haystack).
 * @index: casefolded name → row lookup, fed to contrib_add().
 * @rows: the row store, fed to contrib_add().
 * @error: return location for a GError, or NULL.
 *
 * Collects the matching people of one `videodb://` node by paging its full
 * listing through `Files.GetDirectory {directory, media: "video"}` in
 * MK_CONTRIB_NODE_PAGE windows and matching each label app-side with
 * ci_contains() — the node listings take NO filter param, so a full enumeration
 * is the only way to search them. An empty page ends the walk even when
 * `limits.total` claims more, so an inconsistent reply cannot loop forever.
 *
 * @return TRUE on success (even with zero matches), FALSE with @error set on a
 *         call failure.
 */
static gboolean
contrib_node (MkTools *self, const char *instance, const char *directory,
              guint in, const char *name, GHashTable *index, GPtrArray *rows,
              GError **error)
{
  for (gint64 start = 0;;)
    {
      g_autoptr (JsonBuilder) pb = json_builder_new ();
      json_builder_begin_object (pb);
      json_builder_set_member_name (pb, "directory");
      json_builder_add_string_value (pb, directory);
      json_builder_set_member_name (pb, "media");
      json_builder_add_string_value (pb, "video");
      add_sort (pb, "label");
      add_limits (pb, start, start + MK_CONTRIB_NODE_PAGE);
      json_builder_end_object (pb);
      g_autoptr (JsonNode) params = json_builder_get_root (pb);

      g_autoptr (JsonNode) result = mk_kodi_call (self->kodi, instance,
                                                  "Files.GetDirectory", params,
                                                  error);
      if (result == NULL)
        return FALSE;

      gint64 total = list_total (result);
      g_autoptr (GPtrArray) items = collect_items (result, "files");
      for (guint i = 0; i < items->len; i++)
        {
          JsonObject *it = g_ptr_array_index (items, i);
          const char *label =
            json_object_get_string_member_with_default (it, "label", NULL);
          if (label != NULL && ci_contains (label, name))
            contrib_add (index, rows, label, in);
        }

      start += MK_CONTRIB_NODE_PAGE;
      if (items->len == 0 || start >= total)
        return TRUE;
    }
}

/**
 * build_contributors_result:
 * @rows: the merged, sorted row set.
 * @offset: rows to skip.
 * @limit: max rows to emit.
 * @count: when TRUE, emit zero rows (a count-only response).
 *
 * Builds the `contributors` response: `{ total, returned, offset, truncated,
 * rows: [ { name, in: [containers] } ] }`. Paging is over the merged set
 * (matching is partly app-side, so the set is already fully collected);
 * `truncated` is TRUE when rows remain beyond this page.
 *
 * @return a newly allocated result object node; free with json_node_unref().
 */
static JsonNode *
build_contributors_result (GPtrArray *rows, gint64 offset, gint64 limit,
                           gboolean count)
{
  gint64 total = rows->len;
  gint64 first = count ? total : MIN (offset, total);
  gint64 end = count ? total : MIN (first + limit, total);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "total");
  json_builder_add_int_value (b, total);
  json_builder_set_member_name (b, "returned");
  json_builder_add_int_value (b, end - first);
  json_builder_set_member_name (b, "offset");
  json_builder_add_int_value (b, offset);
  json_builder_set_member_name (b, "truncated");
  json_builder_add_boolean_value (b, offset + (end - first) < total);

  json_builder_set_member_name (b, "rows");
  json_builder_begin_array (b);
  for (gint64 i = first; i < end; i++)
    {
      const MkContribRow *row = g_ptr_array_index (rows, (guint) i);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "name");
      json_builder_add_string_value (b, row->name);
      json_builder_set_member_name (b, "in");
      json_builder_begin_array (b);
      for (gsize c = 0; c < G_N_ELEMENTS (mk_contrib_containers); c++)
        if (row->in & mk_contrib_containers[c].bit)
          json_builder_add_string_value (b, mk_contrib_containers[c].label);
      json_builder_end_array (b);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);

  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/**
 * schema_contributors:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `contributors` tool: the optional `name` and `type` filters
 * plus the paging controls. Everything is optional — no arguments at all means
 * "enumerate every contributor", paged.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_contributors (MkTools *self)
{
  static const char *const types[] =
    { "band", "composer", "actor", "director", NULL };

  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);
  prop_typed (b, "name", "string",
              "Person name to look for (substring, case-insensitive): a band, "
              "solo artist, composer, actor, or director. Omit to list "
              "everyone (of `type` when given).");
  prop_enum (b, "type", types,
             "Contributor kind to list. band = album-level music artist "
             "(closest Kodi gets to a band — includes headline solo artists); "
             "composer/actor/director are literal. Omit for all kinds. "
             "\"List all bands\" = {type: \"band\"} with no name.");
  prop_typed (b, "limit", "integer",
              "Max rows to return (default 50, max 500). Page with offset.");
  prop_typed (b, "offset", "integer",
              "Number of rows to skip — paginate together with limit.");
  prop_typed (b, "count", "boolean",
              "When true, return only the total match count (zero rows).");

  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * handler_contributors:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; both `name` and `type` are optional.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `contributors` tool: collects the matching people from the
 * sources `type` selects — the music library (contrib_music(); ANY, band,
 * composer) and the video people nodes (contrib_node(); ANY walks all three,
 * actor the two cast nodes, director videodb://movies/directors/) — merges
 * them case-insensitively, sorts by name, and pages the merged set. An empty
 * or missing `name` matches everyone, so `{type: "band"}` lists all bands and
 * `{}` enumerates every contributor. Makes no Kodi state change.
 *
 * @return the contributors-result node, or NULL with @error set (unknown type
 *         or a call failure).
 */
static JsonNode *
handler_contributors (MkTools *self, const MkToolDef *def, JsonObject *args,
                      GError **error)
{
  static const struct
  {
    const char *directory;
    guint       in;
    guint       type; /* the one non-ANY type that includes this node */
  } nodes[] = {
    { "videodb://movies/actors/",    MK_CONTRIB_MOVIES,
      MK_CONTRIB_TYPE_ACTOR },
    { "videodb://movies/directors/", MK_CONTRIB_MOVIES,
      MK_CONTRIB_TYPE_DIRECTOR },
    { "videodb://tvshows/actors/",   MK_CONTRIB_TVSHOWS,
      MK_CONTRIB_TYPE_ACTOR },
  };

  (void) def;
  const char *instance = arg_instance (args);
  const char *name = arg_str (args, "name", "");

  guint type = MK_CONTRIB_TYPE_ANY;
  const char *type_str = arg_str (args, "type", NULL);
  if (type_str != NULL)
    {
      gsize t = 0;
      while (t < G_N_ELEMENTS (mk_contrib_types)
             && g_strcmp0 (type_str, mk_contrib_types[t].name) != 0)
        t++;
      if (t == G_N_ELEMENTS (mk_contrib_types))
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "contributors: unknown type \"%s\" "
                       "(use band/composer/actor/director)", type_str);
          return NULL;
        }
      type = mk_contrib_types[t].type;
    }

  gint64 limit = MK_SEARCH_DEFAULT_LIMIT, offset = 0, v = 0;
  if (arg_int (args, "limit", &v))
    limit = v;
  if (arg_int (args, "offset", &v))
    offset = v;
  limit = CLAMP (limit, 0, MK_SEARCH_MAX_LIMIT);
  if (offset < 0)
    offset = 0;
  gboolean count = arg_bool (args, "count");

  g_autoptr (GPtrArray) rows =
    g_ptr_array_new_with_free_func (contrib_row_free);
  g_autoptr (GHashTable) index = g_hash_table_new (g_str_hash, g_str_equal);

  if (type == MK_CONTRIB_TYPE_ANY || type == MK_CONTRIB_TYPE_BAND
      || type == MK_CONTRIB_TYPE_COMPOSER)
    if (!contrib_music (self, instance, type, name, index, rows, error))
      return NULL;
  for (gsize n = 0; n < G_N_ELEMENTS (nodes); n++)
    if ((type == MK_CONTRIB_TYPE_ANY || type == nodes[n].type)
        && !contrib_node (self, instance, nodes[n].directory, nodes[n].in,
                          name, index, rows, error))
      return NULL;

  g_ptr_array_sort (rows, contrib_row_cmp);
  return build_contributors_result (rows, offset, limit, count);
}

/* ---- missing-media detection ----------------------------------------------
 *
 * Kodi disk-checks a plain path in Player.Open / Playlist.Add only when the
 * path is NOT in the library: an unknown missing path is refused outright
 * ("Invalid params"), but a path the library still lists — the file deleted or
 * its share moved after the last scan — is accepted with "OK" and playback
 * just silently never starts (a doomed open even stops whatever was playing).
 * A library id is never disk-checked at all. Verified live on Kodi 19.4 and
 * 20.5. So the playback tools verify the file on disk themselves before
 * touching the player: Files.GetDirectory is a true disk
 * listing, so "is the file really there, as seen from the Kodi box?" is
 * answerable in one call by listing the file's parent directory.
 *
 * One caveat shapes the verdict logic: Files.GetDirectory only lists paths
 * inside the configured media sources — anywhere else it answers the same
 * bare RPC error a truly nonexistent directory gets (verified live: an
 * existing dir outside the sources errors, while an *empty* dir under a
 * source lists fine as []). A refused listing is therefore meaningful only
 * when the path sits under a media source (Files.GetSources); outside the
 * sources the file is simply unverifiable and Kodi stays the authority — its
 * own checks (it does disk-check non-library plain paths) and playfile's
 * post-open backstop cover those.
 */

/**
 * disk_checkable_path:
 * @file: the path or URL a caller asked to play or queue.
 *
 * Says whether check_file_on_disk() can verify @file at all. Only
 * filesystem-like paths are listable via Files.GetDirectory: absolute local
 * paths and the smb://`/`nfs:// network shares libraries are scanned from.
 * Anything else (http(s) streams, plugin:// and other virtual schemes) has no
 * meaningful parent listing — a check would false-positive on perfectly
 * playable URLs — so those are left for Kodi itself to accept or refuse.
 *
 * @return TRUE when @file is a listable filesystem path, FALSE to skip the
 *         disk check.
 */
static gboolean
disk_checkable_path (const char *file)
{
  return file[0] == '/'
         || g_str_has_prefix (file, "smb://")
         || g_str_has_prefix (file, "nfs://");
}

/**
 * path_under_any_source:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @file: the path to test.
 *
 * Says whether @file sits under any configured media source (`Files.GetSources`,
 * queried for both "music" and "video") — the area
 * Files.GetDirectory is able to list, which decides whether a refused listing
 * convicts the file as missing or merely means "outside the browsable area"
 * (see check_file_on_disk()). Best-effort by design: a failing sources query —
 * or an exotic source path (multipath:// et al.) that a plain prefix match
 * cannot claim — reads as "not under a source", which only ever downgrades a
 * would-be "missing" verdict to "unverifiable, let Kodi decide". Never fails
 * the caller.
 *
 * @return TRUE when @file is prefix-covered by a music or video source.
 */
static gboolean
path_under_any_source (MkTools *self, const char *instance, const char *file)
{
  static const char *const medias[] = { "music", "video", NULL };
  for (gsize m = 0; medias[m] != NULL; m++)
    {
      g_autoptr (JsonBuilder) b = json_builder_new ();
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "media");
      json_builder_add_string_value (b, medias[m]);
      json_builder_end_object (b);
      g_autoptr (JsonNode) params = json_builder_get_root (b);

      g_autoptr (GError) local = NULL;
      g_autoptr (JsonNode) res = mk_kodi_call (self->kodi, instance,
                                               "Files.GetSources", params,
                                               &local);
      if (res == NULL)
        continue;

      JsonObject *obj =
        JSON_NODE_HOLDS_OBJECT (res) ? json_node_get_object (res) : NULL;
      JsonNode *sn = obj != NULL ? json_object_get_member (obj, "sources") : NULL;
      JsonArray *sources =
        sn != NULL && JSON_NODE_HOLDS_ARRAY (sn) ? json_node_get_array (sn)
                                                 : NULL;
      for (guint i = 0;
           sources != NULL && i < json_array_get_length (sources); i++)
        {
          JsonNode *en = json_array_get_element (sources, i);
          if (!JSON_NODE_HOLDS_OBJECT (en))
            continue;
          const char *src = json_object_get_string_member_with_default (
            json_node_get_object (en), "file", NULL);
          if (src != NULL && src[0] != '\0' && g_str_has_prefix (file, src))
            return TRUE;
        }
    }
  return FALSE;
}

/**
 * check_file_on_disk:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @file: the path to verify, as the Kodi box sees it.
 * @why: out: on a "missing" verdict, a static human-readable reason; untouched
 *       otherwise.
 * @error: return location for a GError, or NULL.
 *
 * Verifies that @file exists on disk as seen from the target Kodi box, by
 * listing the file's parent directory with `Files.GetDirectory { directory,
 * media: "files" }` and looking for the exact path among the entries. `media`
 * must be "files": the parameter is an extension filter and the schema default
 * ("video") would hide every audio file, reading as "missing".
 *
 * A successful listing is authoritative: the file either is among the entries
 * (present) or is not (verdict "missing" — an empty folder under a source
 * lists fine as [], so absence really is absence). A listing Kodi refuses
 * with an RPC error is ambiguous — a nonexistent directory and an existing
 * one outside the configured media sources answer identically (see the
 * section comment) — so the refusal convicts only a path that
 * path_under_any_source() says Kodi should be able to list: there the folder
 * (or its whole share) is really gone. Outside the sources the file is
 * unverifiable and passes, like a non-filesystem path
 * (disk_checkable_path() FALSE) or a path with no parent to list: Kodi
 * remains the authority on what it can reach. Only a non-RPC failure
 * (transport, auth, …) aborts — that says nothing about the file, so it
 * propagates as the call failure it is.
 *
 * @return 0 when the file is present or unverifiable, 1 when it is verifiably
 *         missing (*why says how it was determined), or -1 with @error set
 *         when the probe itself failed.
 */
static int
check_file_on_disk (MkTools *self, const char *instance, const char *file,
                    const char **why, GError **error)
{
  if (!disk_checkable_path (file))
    return 0;
  const char *slash = g_strrstr (file, "/");
  if (slash == NULL || slash[1] == '\0')
    return 0; /* a directory or unparseable — let Kodi decide */
  g_autofree char *dir = g_strndup (file, (gsize) (slash - file) + 1);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "directory");
  json_builder_add_string_value (b, dir);
  json_builder_set_member_name (b, "media");
  json_builder_add_string_value (b, "files");
  json_builder_end_object (b);
  g_autoptr (JsonNode) params = json_builder_get_root (b);

  g_autoptr (GError) local = NULL;
  g_autoptr (JsonNode) listing = mk_kodi_call (self->kodi, instance,
                                               "Files.GetDirectory", params,
                                               &local);
  if (listing == NULL)
    {
      if (!g_error_matches (local, MK_KODI_ERROR, MK_KODI_ERROR_RPC))
        {
          g_propagate_error (error, g_steal_pointer (&local));
          return -1;
        }
      if (path_under_any_source (self, instance, file))
        {
          *why = "its folder sits inside a configured media source yet "
                 "cannot be listed — the folder or its share is gone, "
                 "renamed, or unmounted on the Kodi box";
          return 1;
        }
      return 0; /* outside the media sources — unverifiable, let Kodi decide */
    }

  JsonObject *obj =
    JSON_NODE_HOLDS_OBJECT (listing) ? json_node_get_object (listing) : NULL;
  JsonNode *fn = obj != NULL ? json_object_get_member (obj, "files") : NULL;
  JsonArray *entries =
    fn != NULL && JSON_NODE_HOLDS_ARRAY (fn) ? json_node_get_array (fn) : NULL;
  for (guint i = 0; entries != NULL && i < json_array_get_length (entries); i++)
    {
      JsonNode *en = json_array_get_element (entries, i);
      if (!JSON_NODE_HOLDS_OBJECT (en))
        continue;
      const char *f = json_object_get_string_member_with_default (
        json_node_get_object (en), "file", NULL);
      if (g_strcmp0 (f, file) == 0)
        return 0;
    }
  *why = "its folder lists fine but holds no such file";
  return 1;
}

/* ---- playfile -------------------------------------------------------------
 *
 * Play one file by path. The natural partner to `searchmedia`: the caller
 * picks a `file` out of a multi-leaf search result and hands it here. Unlike the
 * library `play` (by id), this opens an arbitrary path, so it works for items
 * that are not in the library at all.
 */

#define MK_PLAYFILE_SETTLE_US (500 * 1000) /* let the new player start first */

/**
 * schema_playfile:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `playfile` tool: the required `file` path plus the
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
              "Path of the file to play — the `file` field of a `searchmedia` result "
              "row. Any path Kodi can reach works, in-library or not.");
  prop_typed (b, "fullscreenaudio", "boolean",
              "Audio only: once playback starts, bring Kodi's fullscreen "
              "now-playing screen (the `visualisation` window) to the front. "
              "Audio playback does not auto-fullscreen the way video does, so "
              "without this the player stays behind whatever window is up, "
              "showing only the small now-playing strip. Ignored when the open "
              "starts video (Kodi already fullscreens that) and a no-op when "
              "nothing starts. Note: with no visualisation add-on installed the "
              "window renders blank behind the now-playing info.");

  static const char *const required[] = { "file", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * open_file:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @file: the path to open; non-empty (the caller validates presence).
 * @who: tool name used to prefix the error messages (e.g. "playfile").
 * @error: return location for a GError, or NULL.
 *
 * Shared core of opening one file by path: the three missing-media layers and
 * the post-open "did it actually start" check that both `playfile` and
 * `playfile_with_audio_and_subtitle` need.
 *
 *   1. A verifiably missing file (check_file_on_disk()) is refused *before* the
 *      open — a doomed open would stop whatever is playing now.
 *   2. A path Kodi itself refuses ("Invalid params" for an unknown missing
 *      path) is reshaped to name the file and the likely cause.
 *   3. "OK" with still no active player after the start wait means the open
 *      silently died (the in-library stale-entry case) — reported as a
 *      failure, not a "stopped" success.
 *
 * `Player.Open` replies "OK" the moment the open is queued, before the new
 * player has started, so a brief settle delay, extended by a bounded wait for
 * an active player (wait_players()) when the start is slow, makes the reported
 * state reflect the file that is now playing.
 *
 * @return the post-open player-state snapshot (free with json_node_unref()), or
 *         NULL with @error set.
 */
static JsonNode *
open_file (MkTools *self, const char *instance, const char *file,
           const char *who, GError **error)
{
  /* Layer 1: refuse a verifiably missing file before the open. */
  {
    const char *why = NULL;
    int missing = check_file_on_disk (self, instance, file, &why, error);
    if (missing < 0)
      return NULL;
    if (missing > 0)
      {
        g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                     "%s: \"%s\" is missing from disk — %s; a library "
                     "item pointing at it is stale (the library needs a "
                     "clean/re-scan)", who, file, why);
        return NULL;
      }
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

  /* Layer 2: a refusal from Kodi itself (an unknown missing path is "Invalid
   * params") gets reshaped to name the file and the likely cause. */
  g_autoptr (GError) local = NULL;
  g_autoptr (JsonNode) opened =
    mk_kodi_call (self->kodi, instance, "Player.Open", params, &local);
  if (opened == NULL)
    {
      if (g_error_matches (local, MK_KODI_ERROR, MK_KODI_ERROR_RPC))
        g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                     "%s: Kodi refused to open \"%s\" (%s) — the path "
                     "does not exist or is not reachable from the Kodi box",
                     who, file, local->message);
      else
        g_propagate_error (error, g_steal_pointer (&local));
      return NULL;
    }

  /* The settle covers the common case; a slow start (a cold network share)
   * gets the bounded wait so it is not misreported as never started below. */
  g_usleep (MK_PLAYFILE_SETTLE_US);
  wait_players (self, instance, TRUE);
  JsonNode *state = player_state (self, instance, error);
  if (state == NULL)
    return NULL;

  /* Layer 3: "OK" with no active player after the wait means the open
   * silently died — report the failure instead of a "stopped" success. */
  if (g_strcmp0 (json_object_get_string_member_with_default (
                   json_node_get_object (state), "state", ""),
                 "stopped")
      == 0)
    {
      json_node_unref (state);
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "%s: Kodi accepted \"%s\" but playback never started "
                   "— the file is missing or unreadable on the Kodi box "
                   "(a stale library entry?)", who, file);
      return NULL;
    }

  return state;
}

/**
 * handler_playfile:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; `file` is the path to play, optional
 *        `fullscreenaudio` brings the visualisation window to the front when an
 *        audio player starts.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `playfile` tool: opens one file by path with
 * `Player.Open { "item": { "file": <file> } }`. Kodi auto-selects the
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
 * Missing media is reported as a proper tool error, never as success
 * (see the missing-media section comment). Three layers:
 *
 *   1. A verifiably missing file (check_file_on_disk()) is refused *before*
 *      Player.Open — a doomed open is not just useless, it stops whatever is
 *      playing now.
 *   2. A path Kodi itself refuses ("Invalid params" for an unknown missing
 *      path) is reshaped into an error naming the file and the likely cause,
 *      instead of surfacing Kodi's cryptic refusal verbatim.
 *   3. If Kodi answered "OK" but the post-wait snapshot shows no active
 *      player, the open silently died (the in-library missing-file case) —
 *      that snapshot is discarded and the failure reported.
 *
 * With `fullscreenaudio` set, a successful *audio* open is followed by a
 * best-effort GUI.ActivateWindow("visualisation") to bring the fullscreen
 * now-playing screen forward — audio, unlike video, does not auto-fullscreen on
 * Player.Open. The activation never affects the result: video opens skip it
 * (already fullscreen) and a failed activation is swallowed.
 *
 * @return the post-open player-state object, or NULL with @error set (missing
 *         `file`, a missing/unplayable file, or a call failure).
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

  /* The three missing-media layers and the post-wait start check, shared
   * with playfile_with_audio_and_subtitle. */
  JsonNode *state = open_file (self, instance, file, "playfile", error);
  if (state == NULL)
    return NULL;

  /* Optional fullscreen-on-start, audio only. Video already switches to its
   * fullscreen window on Player.Open; audio does not — it leaves the caller on
   * whatever window was up, showing only the now-playing strip. So when the
   * caller asked AND an audio player actually started, activate `visualisation`
   * to fill the screen. Best-effort: the open has already succeeded and `state`
   * is the real result, so a failed activation must not fail the call — its
   * error is swallowed. */
  if (arg_bool (args, "fullscreenaudio")
      && g_strcmp0 (json_object_get_string_member_with_default (
                      json_node_get_object (state), "type", ""),
                    "audio")
           == 0)
    {
      g_autoptr (JsonBuilder) wb = json_builder_new ();
      json_builder_begin_object (wb);
      json_builder_set_member_name (wb, "window");
      json_builder_add_string_value (wb, "visualisation");
      json_builder_end_object (wb);
      g_autoptr (JsonNode) wparams = json_builder_get_root (wb);
      g_autoptr (GError) wlocal = NULL;
      g_autoptr (JsonNode) wres = mk_kodi_call (
        self->kodi, instance, "GUI.ActivateWindow", wparams, &wlocal);
      (void) wres; /* best-effort: playback already reported in `state` */
    }
  return state;
}

/* ---- queue ----------------------------------------------------------------
 *
 * Continuous playback: append to the **already-playing** queue. The caller must
 * start playback first — a single `play`/`playfile` auto-creates the one-item
 * playlist — so this tool never builds or starts a playlist itself; it
 * only adds behind (or right after) whatever is playing now.
 */

/* The library id kinds `queue` accepts for its `type`/`id` pair — one per
 * `searchmedia` media type, each mapping to Kodi's `<type>id` item key. */
static const char *const mk_queue_types[] = { "song", "episode", "movie", NULL };

/**
 * library_item_file:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @type: the item kind, one of mk_queue_types ("song"/"episode"/"movie").
 * @id: the library id of that kind.
 * @error: return location for a GError, or NULL.
 *
 * Resolves a library id to the file path its library entry points at, via the
 * kind's details method (`AudioLibrary.GetSongDetails` /
 * `VideoLibrary.GetEpisodeDetails` / `VideoLibrary.GetMovieDetails`)
 * requesting just the `file` property — the input
 * check_file_on_disk() needs to verify a queued library item
 * (Playlist.Add never disk-checks an id). As a side effect an id that is not in
 * the library at all gets a proper "no such item" error here, where Kodi's own
 * reply would be a bare "Invalid params".
 *
 * @return the entry's file path as a newly allocated string — empty ("") when
 *         the details carry no file, i.e. nothing to verify — or NULL with
 *         @error set (an unknown id, or a call failure).
 */
static char *
library_item_file (MkTools *self, const char *instance, const char *type,
                   gint64 id, GError **error)
{
  const char *method, *idkey, *detkey;
  if (g_str_equal (type, "song"))
    {
      method = "AudioLibrary.GetSongDetails";
      idkey = "songid";
      detkey = "songdetails";
    }
  else if (g_str_equal (type, "episode"))
    {
      method = "VideoLibrary.GetEpisodeDetails";
      idkey = "episodeid";
      detkey = "episodedetails";
    }
  else
    {
      method = "VideoLibrary.GetMovieDetails";
      idkey = "movieid";
      detkey = "moviedetails";
    }

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, idkey);
  json_builder_add_int_value (b, id);
  json_builder_set_member_name (b, "properties");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "file");
  json_builder_end_array (b);
  json_builder_end_object (b);
  g_autoptr (JsonNode) params = json_builder_get_root (b);

  g_autoptr (GError) local = NULL;
  g_autoptr (JsonNode) res =
    mk_kodi_call (self->kodi, instance, method, params, &local);
  if (res == NULL)
    {
      if (g_error_matches (local, MK_KODI_ERROR, MK_KODI_ERROR_RPC))
        g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                     "queue: no %s with id %" G_GINT64_FORMAT
                     " in the library", type, id);
      else
        g_propagate_error (error, g_steal_pointer (&local));
      return NULL;
    }

  JsonObject *obj =
    JSON_NODE_HOLDS_OBJECT (res) ? json_node_get_object (res) : NULL;
  JsonNode *dn = obj != NULL ? json_object_get_member (obj, detkey) : NULL;
  JsonObject *det =
    dn != NULL && JSON_NODE_HOLDS_OBJECT (dn) ? json_node_get_object (dn) : NULL;
  return g_strdup (det != NULL
                     ? json_object_get_string_member_with_default (det, "file",
                                                                   "")
                     : "");
}

/**
 * schema_queue:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `queue` tool: the optional `instance`, the item to
 * queue as either a `type` + `id` pair (a library item from a `searchmedia` row) or
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
              "`movieid` of a `searchmedia` result row, matching `type`. Give "
              "exactly one of `id` or `file`.");
  prop_typed (b, "file", "string",
              "Path of the file to queue — the `file` field of a `searchmedia` "
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
 * Implements the `queue` tool: appends an item to the playlist behind
 * the **already-playing** item, so playback continues into it. Never builds or
 * starts a playlist — the caller starts playback first (`play`/`playfile`,
 * which auto-creates the one-item playlist). Three steps:
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
 * An item whose file is verifiably missing from disk is refused too
 * (see the missing-media section comment): Kodi accepts such an
 * item with "OK" and the failure would surface only when playback reaches it,
 * so the path — given directly, or resolved from a library id via
 * library_item_file() — is checked with check_file_on_disk() before the add.
 * Playback itself is untouched, so the returned player_state() snapshot
 * shows the still-playing current item — a same-item duplicate the history
 * dedup drops — and no settle delay is needed.
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

  /* Missing media: Playlist.Add, like Player.Open, answers "OK"
   * for a library item whose file has vanished from disk — the failure would
   * surface only when playback reaches the item, long after this call claimed
   * success. The path is resolvable up front for every item kind (an id
   * through its library details), so verify it now and fail the add loudly
   * instead. */
  {
    g_autofree char *path =
      have_file ? g_strdup (file)
                : library_item_file (self, instance, type, id, error);
    if (path == NULL)
      return NULL;
    if (path[0] != '\0')
      {
        const char *why = NULL;
        int missing = check_file_on_disk (self, instance, path, &why, error);
        if (missing < 0)
          return NULL;
        if (missing > 0)
          {
            g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                         "queue: \"%s\" is missing from disk — %s; a library "
                         "item pointing at it is stale (the library needs a "
                         "clean/re-scan)", path, why);
            return NULL;
          }
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

  /* Kodi disk-checks a plain non-library path right here (see the
   * missing-media section comment) — reshape its bare "Invalid params" into
   * an error naming the item and the likely cause. */
  g_autoptr (GError) local = NULL;
  g_autoptr (JsonNode) added =
    mk_kodi_call (self->kodi, instance, next ? "Playlist.Insert" : "Playlist.Add",
                  params, &local);
  if (added == NULL)
    {
      if (!g_error_matches (local, MK_KODI_ERROR, MK_KODI_ERROR_RPC))
        g_propagate_error (error, g_steal_pointer (&local));
      else if (have_file)
        g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                     "queue: Kodi refused to add \"%s\" (%s) — the path does "
                     "not exist or is not reachable from the Kodi box",
                     file, local->message);
      else
        g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                     "queue: Kodi refused to add %s %" G_GINT64_FORMAT " (%s)",
                     type, id, local->message);
      return NULL;
    }

  return player_state (self, instance, error);
}

/* ---- audio_and_subtitle_info ----------------------------------------------
 *
 * Read what audio tracks and subtitles a video carries **before** playing it.
 * Kodi caches each item's embedded stream layout in its video DB at scan time
 * (`streamdetails`), so this is answerable with no playback and nothing
 * mutated — the read-only first step of the audio/subtitle group, surfacing the
 * per-stream index that the during-playback setters will select by.
 */

/* The library id kinds `audio_and_subtitle_info` accepts for its `type`/`id`
 * pair. Songs are excluded: an audio file carries no selectable audio-track or
 * subtitle streamdetails, so the question is meaningless for them. */
static const char *const mk_info_types[] = { "movie", "episode", NULL };

/* Members copied through from each `streamdetails` entry, per stream kind. Kodi
 * fills a superset and copy_member() silently skips any a given entry lacks, so
 * a missing field just drops from that element rather than erroring. */
static const char *const mk_audio_stream_fields[] = { "codec", "channels",
                                                      "language", NULL };
static const char *const mk_subtitle_stream_fields[] = { "language", NULL };
static const char *const mk_video_stream_fields[] = {
  "codec", "width", "height", "aspect", "duration", "stereomode", "hdrtype",
  NULL
};

/**
 * stream_array:
 * @b: the builder, positioned inside the result object.
 * @sd: Kodi's `streamdetails` object, or NULL when the item has none.
 * @kind: the stream kind / member name — "audio", "subtitle" or "video".
 * @fields: NULL-terminated list of entry members to copy through.
 *
 * Emits `"<kind>": [ … ]` — one object per stream in @sd's @kind array, each
 * carrying a synthesized 0-based `index` (its position, the handle the
 * `Player.SetAudioStream`/`SetSubtitle` setters select by for embedded streams)
 * plus the @fields present on that entry. The array is always emitted, empty
 * when @sd is NULL or has no streams of this kind.
 */
static void
stream_array (JsonBuilder *b, JsonObject *sd, const char *kind,
              const char *const *fields)
{
  json_builder_set_member_name (b, kind);
  json_builder_begin_array (b);

  JsonNode *node = sd != NULL ? json_object_get_member (sd, kind) : NULL;
  JsonArray *arr =
    node != NULL && JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node)
                                                 : NULL;
  guint n = arr != NULL ? json_array_get_length (arr) : 0;
  for (guint i = 0; i < n; i++)
    {
      JsonNode *en = json_array_get_element (arr, i);
      if (en == NULL || !JSON_NODE_HOLDS_OBJECT (en))
        continue;
      JsonObject *entry = json_node_get_object (en);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "index");
      json_builder_add_int_value (b, i);
      for (gsize f = 0; fields[f] != NULL; f++)
        copy_member (b, entry, fields[f]);
      json_builder_end_object (b);
    }

  json_builder_end_array (b);
}

/**
 * schema_audio_and_subtitle_info:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `audio_and_subtitle_info` tool: the optional `instance`, the
 * item named as either a `type` + `id` pair (a `searchmedia` movie/episode row)
 * or a `file` path, mirroring `queue`. The one-of-`id`/`file` rule is enforced
 * in the handler so the schema stays a flat property list.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_audio_and_subtitle_info (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);
  prop_enum (b, "type", mk_info_types,
             "Kind of library id passed in `id` — picks the item key "
             "(movieid/episodeid). Required with `id`; not used with `file`. "
             "Songs are not accepted: audio files carry no selectable "
             "audio-track or subtitle streams.");
  prop_typed (b, "id", "integer",
              "Library id of the item to inspect — the `movieid`/`episodeid` of "
              "a `searchmedia` result row, matching `type`. Give exactly one of "
              "`id` or `file`.");
  prop_typed (b, "file", "string",
              "Path of the file to inspect — the `file` field of a "
              "`searchmedia` result row; any path Kodi can reach works. "
              "Streamdetails are only populated for files Kodi has scanned into "
              "its library. Give exactly one of `id` or `file`.");

  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * handler_audio_and_subtitle_info:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; `type`+`id` or `file` name the item, optional
 *        `instance`.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `audio_and_subtitle_info` tool: returns the embedded
 * audio/subtitle/video stream layout of a movie or episode **before** playback,
 * reading Kodi's cached `streamdetails`. No playback, nothing mutated.
 *
 * Resolves the item like `queue` (exactly one of `type`+`id` or `file`):
 *
 *   - `type`+`id` → `VideoLibrary.GetMovieDetails`/`GetEpisodeDetails`
 *   - `file`      → `Files.GetFileDetails { media: "video" }`
 *
 * both with `properties: ["streamdetails","file"]`. An unknown id (or a file
 * Kodi cannot detail) yields a clean "no such item" tool error; other call
 * failures propagate. The reply echoes the resolved `file` and the three
 * normalized stream arrays via stream_array().
 *
 * Only **embedded** streams appear — external subtitle sidecars (.srt) and a
 * file Kodi has not scanned into its library both surface as empty arrays;
 * those are discovered only at playback. The per-stream `index` maps to the
 * `Player.SetAudioStream`/`SetSubtitle` index for embedded streams, but may
 * diverge once sidecars merge in at playback.
 *
 * @return the stream-info object `{ "file", "audio":[…], "subtitle":[…],
 *         "video":[…] }`, or NULL with @error set (bad arguments, an unknown
 *         item, or a call failure).
 */
static JsonNode *
handler_audio_and_subtitle_info (MkTools *self, const MkToolDef *def,
                                 JsonObject *args, GError **error)
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
                   "audio_and_subtitle_info: give exactly one of \"id\" (with "
                   "\"type\") or \"file\"");
      return NULL;
    }

  const char *method = NULL, *detkey = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  if (have_id)
    {
      gboolean known = FALSE;
      for (gsize i = 0; mk_info_types[i] != NULL; i++)
        known = known || g_strcmp0 (type, mk_info_types[i]) == 0;
      if (!known)
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "audio_and_subtitle_info: \"id\" needs \"type\" naming "
                       "its kind (movie/episode)");
          return NULL;
        }

      const char *idkey;
      if (g_str_equal (type, "episode"))
        {
          method = "VideoLibrary.GetEpisodeDetails";
          idkey = "episodeid";
          detkey = "episodedetails";
        }
      else
        {
          method = "VideoLibrary.GetMovieDetails";
          idkey = "movieid";
          detkey = "moviedetails";
        }
      json_builder_set_member_name (b, idkey);
      json_builder_add_int_value (b, id);
    }
  else
    {
      method = "Files.GetFileDetails";
      detkey = "filedetails";
      json_builder_set_member_name (b, "file");
      json_builder_add_string_value (b, file);
      /* `media` is an extension filter; "video" is what makes Kodi parse and
       * return the video item's streamdetails for the path. */
      json_builder_set_member_name (b, "media");
      json_builder_add_string_value (b, "video");
    }
  json_builder_set_member_name (b, "properties");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "streamdetails");
  json_builder_add_string_value (b, "file");
  json_builder_end_array (b);
  json_builder_end_object (b);
  g_autoptr (JsonNode) params = json_builder_get_root (b);

  g_autoptr (GError) local = NULL;
  g_autoptr (JsonNode) res =
    mk_kodi_call (self->kodi, instance, method, params, &local);
  if (res == NULL)
    {
      if (g_error_matches (local, MK_KODI_ERROR, MK_KODI_ERROR_RPC) && have_id)
        g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                     "audio_and_subtitle_info: no %s with id %" G_GINT64_FORMAT
                     " in the library", type, id);
      else if (g_error_matches (local, MK_KODI_ERROR, MK_KODI_ERROR_RPC))
        g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                     "audio_and_subtitle_info: Kodi has no details for \"%s\" "
                     "(%s)", file, local->message);
      else
        g_propagate_error (error, g_steal_pointer (&local));
      return NULL;
    }

  JsonObject *obj =
    JSON_NODE_HOLDS_OBJECT (res) ? json_node_get_object (res) : NULL;
  JsonNode *dn = obj != NULL ? json_object_get_member (obj, detkey) : NULL;
  JsonObject *det =
    dn != NULL && JSON_NODE_HOLDS_OBJECT (dn) ? json_node_get_object (dn) : NULL;
  JsonNode *sdn =
    det != NULL ? json_object_get_member (det, "streamdetails") : NULL;
  JsonObject *sd =
    sdn != NULL && JSON_NODE_HOLDS_OBJECT (sdn) ? json_node_get_object (sdn)
                                                : NULL;
  const char *rfile =
    det != NULL ? json_object_get_string_member_with_default (
                    det, "file", have_file ? file : "")
                : (have_file ? file : "");

  g_autoptr (JsonBuilder) ob = json_builder_new ();
  json_builder_begin_object (ob);
  json_builder_set_member_name (ob, "file");
  json_builder_add_string_value (ob, rfile);
  stream_array (ob, sd, "audio", mk_audio_stream_fields);
  stream_array (ob, sd, "subtitle", mk_subtitle_stream_fields);
  stream_array (ob, sd, "video", mk_video_stream_fields);
  json_builder_end_object (ob);
  return json_builder_get_root (ob);
}

/**
 * out_schema_audio_and_subtitle_info:
 * @self: the table (unused; the schema is static).
 *
 * Output schema for `audio_and_subtitle_info`: the resolved `file` plus the
 * three stream arrays, each an array of open objects whose members are
 * documented inline (they vary by stream kind and by what Kodi captured).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_audio_and_subtitle_info (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_typed (b, "file", "string",
              "The resolved file path of the inspected item.");
  prop_array_obj (b, "audio",
                  "An embedded audio track: { \"index\", \"codec\", "
                  "\"channels\", \"language\" }. \"index\" is the "
                  "Player.SetAudioStream handle for embedded streams.",
                  "Embedded audio tracks, in stream order.");
  prop_array_obj (b, "subtitle",
                  "An embedded subtitle track: { \"index\", \"language\" }. "
                  "External .srt sidecars are not listed here — they appear "
                  "only at playback.",
                  "Embedded subtitle tracks, in stream order.");
  prop_array_obj (b, "video",
                  "A video stream: { \"index\", \"codec\", \"width\", "
                  "\"height\", \"aspect\", \"duration\", \"stereomode\", "
                  "\"hdrtype\"? }.",
                  "Video streams, in stream order.");

  static const char *const required[] = { "file", "audio", "subtitle", "video",
                                          NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/* ---- audio/subtitle track selection (shared) ------------------------------
 *
 * The `audio`/`subtitle` selectors and the calls that apply them are shared by
 * two tools: `set_audio_and_subtitle` (switch on the player now playing) and
 * `playfile_with_audio_and_subtitle` (open a file, then select on it). The
 * schema prop builder, the argument parser, and the applier live here so the
 * two tools stay byte-for-byte consistent.
 */

/* A parsed audio/subtitle selection. `audio` is meaningful only when
 * `have_audio`; the subtitle is an index (`have_sub_index`) OR a disable
 * (`sub_off`) — never both — or neither (left unchanged). */
typedef struct
{
  gboolean have_audio;
  gint64   audio;
  gboolean have_sub_index;
  gboolean sub_off;
  gint64   subtitle;
} MkTrackArgs;

/**
 * prop_audio_subtitle_selectors:
 * @b: the schema builder, positioned to add properties.
 *
 * Adds the two optional stream selectors. `audio` is a plain integer stream
 * index. `subtitle` is a mixed integer-or-`"off"` type that no `prop_*` helper
 * produces, so it is built inline as a `oneOf` of an integer index and the
 * literal `"off"`. Both are optional; the "give at least one" rule (and the
 * index-or-`"off"` check on `subtitle`) is enforced by parse_track_args().
 */
static void
prop_audio_subtitle_selectors (JsonBuilder *b)
{
  prop_typed (b, "audio", "integer",
              "Audio stream index to select — an `index` from "
              "`audio_and_subtitle_info`. Omit to leave the audio track "
              "unchanged.");

  /* `subtitle`: integer index OR the literal "off". Built inline — no helper
   * emits a mixed type. parse_track_args() reads the node's type rather than
   * arg_int, which would misread the string "off" as index 0. */
  json_builder_set_member_name (b, "subtitle");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "description");
  json_builder_add_string_value (
    b, "Subtitle stream index to show — an `index` from "
       "`audio_and_subtitle_info` — or \"off\" to hide subtitles. Omit to "
       "leave the subtitle unchanged (\"off\" is distinct from omitting it).");
  json_builder_set_member_name (b, "oneOf");
  json_builder_begin_array (b);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "integer");
  json_builder_set_member_name (b, "description");
  json_builder_add_string_value (b, "Subtitle stream index to show.");
  json_builder_end_object (b);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "string");
  json_builder_set_member_name (b, "enum");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "off");
  json_builder_end_array (b);
  json_builder_set_member_name (b, "description");
  json_builder_add_string_value (b, "Turn subtitles off.");
  json_builder_end_object (b);
  json_builder_end_array (b);
  json_builder_end_object (b);
}

/**
 * parse_track_args:
 * @args: the call arguments; optional `audio` and `subtitle`.
 * @who: tool name used to prefix the error messages.
 * @out: filled with the parsed selection on success.
 * @error: return location for a GError, or NULL.
 *
 * Parses the `audio`/`subtitle` selectors and enforces "give at least one".
 * `subtitle` is read by inspecting the argument's JSON type (arg_int would
 * silently read the string `"off"` as index 0).
 *
 * @return TRUE with @out filled, or FALSE with @error set on a bad `subtitle`
 *         type or an empty selection.
 */
static gboolean
parse_track_args (JsonObject *args, const char *who, MkTrackArgs *out,
                  GError **error)
{
  MkTrackArgs t = { 0 };
  t.have_audio = arg_int (args, "audio", &t.audio);

  JsonNode *sn = (args != NULL && json_object_has_member (args, "subtitle"))
                   ? json_object_get_member (args, "subtitle")
                   : NULL;
  if (sn != NULL && !JSON_NODE_HOLDS_NULL (sn))
    {
      if (JSON_NODE_HOLDS_VALUE (sn)
          && json_node_get_value_type (sn) == G_TYPE_INT64)
        {
          t.have_sub_index = TRUE;
          t.subtitle = json_node_get_int (sn);
        }
      else if (JSON_NODE_HOLDS_VALUE (sn)
               && json_node_get_value_type (sn) == G_TYPE_STRING
               && g_strcmp0 (json_node_get_string (sn), "off") == 0)
        t.sub_off = TRUE;
      else
        {
          g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                       "%s: \"subtitle\" must be a stream index (integer) or "
                       "\"off\"", who);
          return FALSE;
        }
    }

  if (!t.have_audio && !t.have_sub_index && !t.sub_off)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "%s: give at least one of \"audio\" or \"subtitle\"", who);
      return FALSE;
    }

  *out = t;
  return TRUE;
}

/**
 * apply_audio_subtitle:
 * @self: the tool table.
 * @instance: target instance name, or NULL for the configured default.
 * @playerid: the active video player to act on.
 * @t: the parsed selection (parse_track_args()).
 * @error: return location for a GError, or NULL.
 *
 * Applies the selection to a known video @playerid: `Player.SetAudioStream` and/
 * or `Player.SetSubtitle`. Each is applied only when given. `enable: true` is
 * what actually shows a chosen subtitle; it is omitted for `"off"`.
 *
 * @return TRUE, or FALSE with @error set on a call failure.
 */
static gboolean
apply_audio_subtitle (MkTools *self, const char *instance, gint64 playerid,
                      const MkTrackArgs *t, GError **error)
{
  if (t->have_audio)
    {
      g_autoptr (JsonBuilder) b = json_builder_new ();
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "playerid");
      json_builder_add_int_value (b, playerid);
      json_builder_set_member_name (b, "stream");
      json_builder_add_int_value (b, t->audio);
      json_builder_end_object (b);
      g_autoptr (JsonNode) params = json_builder_get_root (b);
      g_autoptr (JsonNode) res = mk_kodi_call (
        self->kodi, instance, "Player.SetAudioStream", params, error);
      if (res == NULL)
        return FALSE;
    }

  if (t->have_sub_index || t->sub_off)
    {
      g_autoptr (JsonBuilder) b = json_builder_new ();
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "playerid");
      json_builder_add_int_value (b, playerid);
      json_builder_set_member_name (b, "subtitle");
      if (t->sub_off)
        json_builder_add_string_value (b, "off");
      else
        json_builder_add_int_value (b, t->subtitle);
      /* `enable: true` is what actually shows a selected subtitle (Kodi may
       * have them turned off); it is meaningless for "off", so omit it there. */
      if (t->have_sub_index)
        {
          json_builder_set_member_name (b, "enable");
          json_builder_add_boolean_value (b, TRUE);
        }
      json_builder_end_object (b);
      g_autoptr (JsonNode) params = json_builder_get_root (b);
      g_autoptr (JsonNode) res = mk_kodi_call (
        self->kodi, instance, "Player.SetSubtitle", params, error);
      if (res == NULL)
        return FALSE;
    }

  return TRUE;
}

/**
 * schema_set_audio_and_subtitle:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `set_audio_and_subtitle` tool: the optional `instance` and the
 * two optional stream selectors (prop_audio_subtitle_selectors()). Both
 * selectors are optional; the "give at least one" rule (and the
 * index-or-`"off"` check on `subtitle`) is enforced in the handler.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_set_audio_and_subtitle (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);
  prop_audio_subtitle_selectors (b);

  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * handler_set_audio_and_subtitle:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; optional `audio` (stream index), optional
 *        `subtitle` (stream index or "off"), optional `instance`.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `set_audio_and_subtitle` tool: switches the audio track and/or
 * subtitle of the video **now playing**, by the per-stream `index` that
 * `audio_and_subtitle_info` reports. The during-playback partner of that
 * read-only inspector.
 *
 * `audio` selects an audio track by index; `subtitle` selects a subtitle by
 * index or hides subtitles with `"off"`. Each is optional and applied only when
 * given — omitting one leaves that stream untouched; at least one must be
 * present. `"off"` is read by inspecting the argument's JSON type (arg_int
 * would silently read the string `"off"` as index 0).
 *
 * Resolves the active player like `queue` (`Player.GetActivePlayers`); with
 * nothing playing, or a non-video player (audio tracks and subtitles are a
 * video concept), it returns a clean tool error. Then applies
 * `Player.SetAudioStream { playerid, stream }` and/or `Player.SetSubtitle
 * { playerid, subtitle, enable: true }` — `enable: true` is what actually shows
 * a selected subtitle; it is omitted for `"off"`.
 *
 * @return the player-state snapshot (the standard playback status; playback
 *         item unchanged). Since TODO 7.3 that snapshot's top-level
 *         `audio`/`subtitle` summary echoes the resulting track layout, so the
 *         caller sees the selection it just made take effect. NULL with @error
 *         set on bad arguments, nothing playing, a non-video player, or a call
 *         failure.
 */
static JsonNode *
handler_set_audio_and_subtitle (MkTools *self, const MkToolDef *def,
                                JsonObject *args, GError **error)
{
  (void) def;
  const char *instance = arg_instance (args);

  MkTrackArgs t;
  if (!parse_track_args (args, "set_audio_and_subtitle", &t, error))
    return NULL;

  /* The active player, and its kind. Audio-track and subtitle switching apply
   * to video only; an empty list means nothing is playing. */
  g_autoptr (JsonNode) active =
    mk_kodi_call (self->kodi, instance, "Player.GetActivePlayers", NULL, error);
  if (active == NULL)
    return NULL;
  JsonArray *players =
    JSON_NODE_HOLDS_ARRAY (active) ? json_node_get_array (active) : NULL;
  if (players == NULL || json_array_get_length (players) == 0)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "set_audio_and_subtitle: nothing is playing — start "
                   "playback first (play/playfile)");
      return NULL;
    }
  JsonObject *p0 = json_array_get_object_element (players, 0);
  gint64 playerid = json_object_get_int_member_with_default (p0, "playerid", 0);
  const char *ptype =
    json_object_get_string_member_with_default (p0, "type", NULL);
  if (g_strcmp0 (ptype, "video") != 0)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "set_audio_and_subtitle: audio tracks and subtitles apply "
                   "to video playback only");
      return NULL;
    }

  if (!apply_audio_subtitle (self, instance, playerid, &t, error))
    return NULL;

  /* The standard playback status. The playback item is unchanged — only stream
   * selection — so no settle delay is needed; player_state() also records the
   * snapshot to history. */
  return player_state (self, instance, error);
}

/* ---- playfile_with_audio_and_subtitle -------------------------------------
 *
 * Start playback of a file AND select its audio track / subtitle in one call —
 * the "when starting the playback" partner of `set_audio_and_subtitle` (which
 * acts on the item already playing). `Player.Open` has no audio/subtitle
 * option, so the tool opens the file, waits for the stream to start, then
 * applies the same `Player.SetAudioStream`/`SetSubtitle` selection. Video only:
 * audio tracks and subtitles are a video concept.
 */

/* Let the just-opened stream's tracks enumerate before selecting. open_file()
 * already settled once and confirmed playback started; this second settle
 * covers the brief window where the player is up but its audio/subtitle streams
 * are not yet listed (Player.OnAVStart, which the server cannot subscribe to —
 * see TODO 8.9.4). */
#define MK_TRACKS_SETTLE_US (500 * 1000)

/**
 * schema_playfile_with_audio_and_subtitle:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `playfile_with_audio_and_subtitle` tool: the required `file`
 * (as `playfile`), the optional `instance`, and the two stream selectors
 * (prop_audio_subtitle_selectors()). At least one of `audio`/`subtitle` must be
 * given — that is the whole point of the tool — enforced in the handler.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_playfile_with_audio_and_subtitle (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);
  prop_typed (b, "file", "string",
              "Path of the video file to play — the `file` field of a "
              "`searchmedia` result row, or what `audio_and_subtitle_info` "
              "reports. Any path Kodi can reach works, in-library or not.");
  prop_audio_subtitle_selectors (b);

  static const char *const required[] = { "file", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * handler_playfile_with_audio_and_subtitle:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; required `file`, optional `audio` (stream index),
 *        optional `subtitle` (stream index or "off"), optional `instance`.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `playfile_with_audio_and_subtitle` tool: opens a file and
 * selects its audio track and/or subtitle once the stream starts — the
 * "starting the playback" partner of `set_audio_and_subtitle`. Since
 * `Player.Open` carries no track option, the selection is applied right after
 * the open (open → wait → apply → return), entirely within this call.
 *
 * Steps:
 *   1. Validate `file` and parse the `audio`/`subtitle` selectors
 *      (parse_track_args(); at least one is required — otherwise plain
 *      `playfile` is the tool).
 *   2. open_file() — the shared opener with its three missing-media layers and
 *      post-wait "did it actually start" check.
 *   3. Resolve the now-started player. Audio tracks and subtitles are a video
 *      concept, so a non-video open (an audio file given track args) is a clean
 *      tool error — playback has already begun, which the message notes.
 *   4. A brief settle so the new stream's tracks enumerate, then
 *      apply_audio_subtitle() applies the same calls `set_audio_and_subtitle`
 *      uses.
 *
 * @return the post-selection player-state snapshot — its top-level
 *         `audio`/`subtitle` summary (TODO 7.3) echoes the resulting layout, so
 *         the caller sees the selection take effect. NULL with @error set on a
 *         missing `file`, an empty selection, a missing/unplayable file, a
 *         non-video open, or a call failure.
 */
static JsonNode *
handler_playfile_with_audio_and_subtitle (MkTools *self, const MkToolDef *def,
                                          JsonObject *args, GError **error)
{
  (void) def;
  const char *who = "playfile_with_audio_and_subtitle";
  const char *instance = arg_instance (args);
  const char *file = arg_str (args, "file", NULL);
  if (file == NULL || file[0] == '\0')
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "%s: \"file\" is required", who);
      return NULL;
    }

  MkTrackArgs t;
  if (!parse_track_args (args, who, &t, error))
    return NULL;

  /* Open the file (shared missing-media layers + start check). The snapshot it
   * returns only confirms the open started; the player is re-resolved below for
   * its id and kind. */
  g_autoptr (JsonNode) opened = open_file (self, instance, file, who, error);
  if (opened == NULL)
    return NULL;

  /* Resolve the now-started player. Tracks are video-only; an audio open (an
   * audio file handed track args) is a clean error — playback has begun. */
  g_autoptr (JsonNode) active =
    mk_kodi_call (self->kodi, instance, "Player.GetActivePlayers", NULL, error);
  if (active == NULL)
    return NULL;
  JsonArray *players =
    JSON_NODE_HOLDS_ARRAY (active) ? json_node_get_array (active) : NULL;
  if (players == NULL || json_array_get_length (players) == 0)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "%s: Kodi accepted \"%s\" but playback never started", who,
                   file);
      return NULL;
    }
  JsonObject *p0 = json_array_get_object_element (players, 0);
  gint64 playerid = json_object_get_int_member_with_default (p0, "playerid", 0);
  const char *ptype =
    json_object_get_string_member_with_default (p0, "type", NULL);
  if (g_strcmp0 (ptype, "video") != 0)
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "%s: \"%s\" is not video — audio tracks and subtitles apply "
                   "to video playback only (it is now playing)", who, file);
      return NULL;
    }

  /* Wait for the stream's tracks to enumerate, then select. */
  g_usleep (MK_TRACKS_SETTLE_US);
  if (!apply_audio_subtitle (self, instance, playerid, &t, error))
    return NULL;

  /* The standard playback status; its audio/subtitle summary now reflects the
   * selection. player_state() also records the snapshot to history. */
  return player_state (self, instance, error);
}

/* ---- getplaylist ----------------------------------------------------------
 *
 * Read the current queue: the rows of a playlist plus where playback sits in
 * it, changing nothing — the inspection partner of `queue` and the
 * audit step before `dropplaylists` empties everything.
 */

/* The playlist kinds `getplaylist` accepts for `type`, indexed by Kodi's three
 * fixed playlist ids (audio 0 / video 1 / picture 2). */
static const char *const mk_playlist_types[] = { "audio", "video", "picture",
                                                 NULL };

/**
 * schema_getplaylist:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `getplaylist` tool: the optional `instance` and the
 * optional `type` selecting which playlist to read. `type` always wins when
 * provided — it reads that playlist even while another plays, the only way to
 * inspect an inactive queue before `dropplaylists` destroys it;
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
 * Implements the `getplaylist` tool: reports what is queued and
 * where playback sits, read-only. The playlist to read is the `type` argument
 * when provided — it always wins, mapping to Kodi's fixed playlist ids
 * (audio 0 / video 1 / picture 2) — else the one behind the active player,
 * resolved as `queue` does:
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
 * non-playlist playback) — yields an empty `items` list, not an error.
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
       * reports them as type "unknown" with id -1, leaving their
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

/* ---- dropplaylists --------------------------------------------------------
 *
 * Empty the queues: Playlist.Clear on each of Kodi's three fixed playlists so
 * no queued items remain anywhere — the destructive counterpart of
 * `getplaylist`, and the whole-device sibling of `stop`'s single clear.
 */

/**
 * handler_dropplaylists:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments (optional `instance`), or NULL.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `dropplaylists` tool: `Playlist.Clear` on each of
 * Kodi's three fixed playlist ids (audio 0 / video 1 / picture 2 — the
 * mk_playlist_types order), so no queued items remain. Plural by design: one
 * call clears all three, with no way to pick — clearing a single, known
 * playlist is `stop`'s narrower job. Kodi's clear on the *active*
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

/* ---- rpc ------------------------------------------------------------------
 *
 * The escape hatch: send any JSON-RPC method to a box and return Kodi's raw
 * reply unshaped, for anything the dedicated tools don't model. Powerful and
 * unconstrained, so it is gated per instance behind the hand-edit-only
 * `allow_rpc` flag: an instance that has not opted in refuses the call before
 * it reaches Kodi, and
 * the flag can never be set through the `instances` tool — only the model's
 * operator can grant it.
 */

/**
 * schema_rpc:
 * @self: the table (used to name the instances that permit the hatch).
 *
 * Schema for the `rpc` tool: the required `method`, an optional
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

  /* List the boxes that have opted in, so the schema documents where the
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
 * Implements the `rpc` escape hatch: builds the JSON-RPC envelope and
 * POSTs it to the resolved instance, returning Kodi's `result`
 * **verbatim** — no shaping, unlike every other tool. A Kodi `error` surfaces as
 * a tool error.
 *
 * Gated per instance: unless the target instance's config sets
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

  /* The escape-hatch gate: a configured instance must have opted in. An
   * unconfigured instance falls through to mk_kodi_call()'s no-instance error. */
  MkInstance *inst = mk_config_get_instance (self->config, instance);
  if (inst != NULL && !inst->allow_rpc)
    {
      const char *name = instance ? instance
                                  : mk_config_get_default (self->config);
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "rpc: the escape hatch is disabled for instance \"%s\". "
                   "Enable it by setting \"allow_rpc\": true on that instance in "
                   "the server config file by hand; it cannot be enabled via "
                   "the instances tool.",
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

  /* rpc returns Kodi's raw result and takes no snapshot of its own, so
   * a hand-rolled Player.Open would slip past history. After a *successful*
   * call, take one player_state() snapshot purely to feed the log — it records
   * as a side effect. This must NOT change rpc's verbatim return, so the
   * snapshot and any error are discarded. A non-playback rpc
   * just yields a stopped or duplicate snapshot that records nothing,
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

/* ---- History tool ---------------------------------------------------------- */

#define MK_HISTORY_DEFAULT_LIMIT 50   /* newest-in-window cap when omitted */
#define MK_HISTORY_MAX_LIMIT     1000 /* hard ceiling on a single read     */

/**
 * schema_history:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `history` tool: an optional ISO-8601 window
 * (`since`/`until`), an optional `instance`, paging (`limit`/`offset`/`order`/
 * `count`), and a set of app-side filters over the stored entries — `media`,
 * `kind`, `artist`, free-text `match`, and an exact `id`. Nothing is required —
 * a bare call returns the most recent entries across all boxes; every filter is
 * AND-combined with the window. The `instance` description deliberately spells
 * out that *omitting* it spans all instances, which inverts the convention every
 * other tool follows (omitted = the default box), so the difference is explicit
 * rather than a surprise.
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
              "Max entries to return (default 50, max 1000). When more match, "
              "the reply sets \"truncated\": true — page the rest with offset.");
  prop_typed (b, "offset", "integer",
              "Number of matching entries to skip before this page — paginate "
              "together with limit, keeping the same order.");
  static const char *const orders[] = { "newest", "oldest", NULL };
  prop_enum (b, "order", orders,
             "Result order: \"newest\" first (default) or \"oldest\" first to "
             "replay a session in the order it happened.");
  prop_typed (b, "count", "boolean",
              "When true, return only the total match count (zero entries) — a "
              "cheap \"how many times / how much did X play\".");

  static const char *const medias[] = { "song",       "episode", "movie",
                                        "musicvideo", "picture", "channel",
                                        NULL };
  prop_enum (b, "media", medias,
             "Keep only entries of this media kind (song/episode/movie/"
             "musicvideo/picture/channel) — e.g. \"movies only\".");
  static const char *const kinds[] = { "audio", "video", NULL };
  prop_enum (b, "kind", kinds,
             "Keep only entries of this broad kind: audio or video — e.g. "
             "\"what music did I play\".");
  prop_typed (b, "artist", "string",
              "Keep only entries whose performer contains this (substring, "
              "case-insensitive) — the music-by-artist filter (\"everything "
              "Pink Floyd I played\"). Only as good as the captured artist tag.");
  prop_typed (b, "match", "string",
              "Free-text substring (case-insensitive) over an entry's human "
              "fields — title, album, show, label and artist — to narrow the "
              "log without knowing ids.");
  prop_typed (b, "id", "integer",
              "Exact library id to match (\"every time THIS item played\"). Ids "
              "are per media type, so pair with media; feed back a searchmedia "
              "or getplaylist row's id.");

  schema_end (b, NULL);
  return json_builder_get_root (b);
}

/**
 * handler_history:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments: optional instance, since/until window, the
 *        media/kind/artist/match/id filters and limit/offset/order/count
 *        paging controls (see schema_history()).
 * @error: return location for a GError, or NULL.
 *
 * Implements the `history` tool: reads the local playback log via
 * mk_history_read() and wraps the matches in a `searchmedia`-style envelope.
 * Makes no Kodi call. An omitted `instance` spans all boxes (an inversion — see
 * schema_history()); the optional `media`/`kind`/`artist`/`match`/`id` filters
 * are AND-combined with the window. `limit` defaults to 50 and is clamped,
 * `offset` pages, `order` flips newest/oldest-first, and `count` returns only
 * the total (zero entries). A malformed window bound or a corrupt log surfaces
 * as a normal tool error (mk_history_read sets @error).
 *
 * @return `{ "instance"?, "since"?, "until"?, "total", "returned", "offset",
 *         "truncated", "entries": [ … ] }`, or NULL with @error set.
 */
static JsonNode *
handler_history (MkTools *self, const MkToolDef *def, JsonObject *args,
                 GError **error)
{
  (void) def;
  /* Unlike every other tool, an omitted instance is NOT defaulted to a box: NULL
   * tells mk_history_read() to span all instances. */
  MkHistoryQuery q = { 0 };
  q.instance = arg_instance (args);
  q.since = arg_str (args, "since", NULL);
  q.until = arg_str (args, "until", NULL);
  q.media = arg_str (args, "media", NULL);
  q.kind = arg_str (args, "kind", NULL);
  q.artist = arg_str (args, "artist", NULL);
  q.match = arg_str (args, "match", NULL);

  gint64 v = 0;
  q.have_id = arg_int (args, "id", &q.id);

  gint64 limit = MK_HISTORY_DEFAULT_LIMIT;
  if (arg_int (args, "limit", &v))
    limit = v;
  q.limit = CLAMP (limit, 1, MK_HISTORY_MAX_LIMIT);
  if (arg_int (args, "offset", &v))
    q.offset = v;
  if (q.offset < 0)
    q.offset = 0;

  const char *order = arg_str (args, "order", NULL);
  q.oldest_first = order != NULL && g_str_equal (order, "oldest");

  /* count-only: ask for the matches but emit none — q.limit drives the read, so
   * read a single page then drop the rows, keeping `total` honest. */
  gboolean count = arg_bool (args, "count");

  gint64 total = 0;
  g_autoptr (JsonNode) entries =
    mk_history_read (self->history, &q, &total, error);
  if (entries == NULL)
    return NULL;
  gint64 returned = count ? 0
                          : json_array_get_length (json_node_get_array (entries));

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  if (q.instance != NULL)
    {
      json_builder_set_member_name (b, "instance");
      json_builder_add_string_value (b, q.instance);
    }
  if (q.since != NULL && *q.since != '\0')
    {
      json_builder_set_member_name (b, "since");
      json_builder_add_string_value (b, q.since);
    }
  if (q.until != NULL && *q.until != '\0')
    {
      json_builder_set_member_name (b, "until");
      json_builder_add_string_value (b, q.until);
    }
  json_builder_set_member_name (b, "total");
  json_builder_add_int_value (b, total);
  json_builder_set_member_name (b, "returned");
  json_builder_add_int_value (b, returned);
  json_builder_set_member_name (b, "offset");
  json_builder_add_int_value (b, q.offset);
  json_builder_set_member_name (b, "truncated");
  json_builder_add_boolean_value (b, q.offset + returned < total);
  json_builder_set_member_name (b, "entries");
  if (count)
    {
      json_builder_begin_array (b);
      json_builder_end_array (b);
    }
  else
    json_builder_add_value (b, json_node_ref (entries));
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/* ---- Output schemas --------------------------------------------------------
 *
 * MCP `outputSchema` builders (spec revision 2025-06-18): one per distinct
 * result shape, attached to a tool via MkToolDef.out_schema, emitted by
 * mk_tools_list() and honoured by mk_tools_call(), which mirrors a declaring
 * tool's successful result as `structuredContent`. Envelope members are typed
 * precisely; row/entry objects stay open (prop_array_obj()) because their
 * members vary per media type. Older protocol revisions simply ignore the
 * extra member. `rpc` declares none: it returns Kodi's raw result verbatim,
 * which need not even be an object.
 */

/**
 * out_schema_snapshot:
 * @self: the table (unused; the shape is fixed).
 *
 * Output schema of the player-state snapshot (player_state()) shared by every
 * playback-affecting tool and `nowplaying`. Only `state` is guaranteed: the
 * top-level player status (`type`/`time`/`totaltime`/`progress`) appears when a
 * player is active, a video's `audio`/`subtitle` summary (TODO 7.3) when it is a
 * video, the loaded media nests under `nowplaying` (TODO 7.2) with
 * its per-media fields present only where they apply, and an empty field is
 * omitted rather than emitted blank.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_snapshot (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  static const char *const states[] = { "playing", "paused", "stopped", NULL };
  prop_enum (b, "state", states,
             "Playback state; \"stopped\" means nothing is loaded.");
  static const char *const players[] = { "audio", "video", "picture", NULL };
  prop_enum (b, "type", players, "The active player kind.");
  prop_typed (b, "time", "object",
              "Playback position { hours, minutes, seconds, milliseconds }.");
  prop_typed (b, "totaltime", "object",
              "The item's duration, same shape as time.");
  prop_typed (b, "progress", "string",
              "Legible position derived from time/totaltime, "
              "\"elapsed / total (NN%)\" e.g. \"15:38 / 46:40 (33%)\"; absent "
              "for live streams with no fixed length.");

  /* A video's audio/subtitle summary (TODO 7.3): brief — a count plus the live
   * selection, not the full per-stream dump (audio_and_subtitle_info has that).
   * Video only; absent for audio playback. */
  prop_typed (b, "audio", "object",
              "Audio tracks of the playing video (video only): "
              "{ count, current: { index, language, codec, channels } }. "
              "`index` is the set_audio_and_subtitle audio selector.");
  prop_typed (b, "subtitle", "object",
              "Subtitles of the playing video (video only, present only when "
              "any exist): { count, enabled, current: { index, language } }. "
              "`current` is present only while a subtitle is shown; `index` is "
              "the set_audio_and_subtitle subtitle selector.");

  /* The loaded media, nested (TODO 7.2). Members vary per media type, so the
   * object stays open (no fixed sub-property list), described in prose — the
   * prop_array_obj() treatment, minus the array wrapper. */
  prop_typed (b, "nowplaying", "object",
              "The loaded media (present whenever something is loaded): "
              "{ media (song/episode/movie/…; \"unknown\" off-library), "
              "id (library id, -1 off-library), file, label, title; and where "
              "they apply showtitle/season/episode (TV), album/artist/track "
              "(song) }.");

  static const char *const required[] = { "state", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * audio_out_schema:
 * @with_bounds: also include the fixed `min`/`max` volume bounds (the
 *               `volume` Knob's extension of the shared audio shape).
 *
 * Builds the audio-state output schema — `{ "muted", "volume" }`, optionally
 * with `min`/`max` — the schema twin of audio_snapshot()'s @with_bounds.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
audio_out_schema (gboolean with_bounds)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_typed (b, "muted", "boolean", "Whether audio output is muted.");
  prop_typed (b, "volume", "integer", "The application volume, 0-100.");
  if (with_bounds)
    {
      prop_typed (b, "min", "integer", "Lower volume bound (always 0).");
      prop_typed (b, "max", "integer", "Upper volume bound (always 100).");
    }

  static const char *const req[] = { "muted", "volume", NULL };
  static const char *const req_bounds[] =
    { "muted", "volume", "min", "max", NULL };
  schema_end (b, with_bounds ? req_bounds : req);
  return json_builder_get_root (b);
}

/**
 * out_schema_audio:
 * @self: the table (unused; the shape is fixed).
 *
 * Output schema of `mute`/`unmute`: the shared audio state without bounds.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_audio (MkTools *self)
{
  (void) self;
  return audio_out_schema (FALSE);
}

/**
 * out_schema_volume:
 * @self: the table (unused; the shape is fixed).
 *
 * Output schema of the `volume` Knob: the audio state plus the fixed scale
 * bounds, so the schema also tells the caller the step arithmetic's range.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_volume (MkTools *self)
{
  (void) self;
  return audio_out_schema (TRUE);
}

/**
 * out_schema_instances:
 * @self: the table (unused; the shape is fixed).
 *
 * Output schema of the `instances` config tool: the shared response shape
 * built by instances_list(), returned by all three actions.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_instances (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  /* `default` is string-or-null — beyond prop_typed's single type. */
  json_builder_set_member_name (b, "default");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "string");
  json_builder_add_string_value (b, "null");
  json_builder_end_array (b);
  json_builder_set_member_name (b, "description");
  json_builder_add_string_value (
    b, "Key of the default instance, or null when none is configured.");
  json_builder_end_object (b);

  prop_array_obj (b, "instances",
                  "{ \"key\", \"name\"?, \"host\", \"scheme\", \"insecure\", "
                  "\"has_auth\", \"allow_rpc\" } — has_auth stands in for the "
                  "write-only password; allow_rpc is read-only here.",
                  "The configured instances, in sorted key order.");
  prop_typed (b, "build", "string",
              "Build ID of this MCP server binary (version plus git "
              "describe); differs from the source tree's when the running "
              "server is stale.");

  static const char *const required[] = { "default", "instances", "build",
                                          NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * paging_out_props:
 * @b: the builder, positioned inside a `properties` object.
 *
 * Adds the shared paged-envelope members — `total`, `returned`, `offset`,
 * `truncated` — emitted identically by the searchmedia/contributors/history
 * envelopes, so their output schemas describe them with one voice.
 */
static void
paging_out_props (JsonBuilder *b)
{
  prop_typed (b, "total", "integer",
              "Full match count, before any paging.");
  prop_typed (b, "returned", "integer", "Rows in this page.");
  prop_typed (b, "offset", "integer", "Rows skipped before this page.");
  prop_typed (b, "truncated", "boolean",
              "Whether matches remain beyond this page.");
}

/**
 * out_schema_search:
 * @self: the table (unused; the shape is fixed).
 *
 * Output schema of `searchmedia`: the paged envelope built by
 * build_search_result(), with the leaf rows left open per media type.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_search (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  static const char *const types[] = { "music", "tv-show", "movie", NULL };
  prop_enum (b, "type", types, "The searched media type, echoed back.");
  paging_out_props (b);
  prop_typed (b, "approximate", "boolean",
              "Present (true) when total/paging are app-side estimates (a "
              "substring title matched several albums).");
  prop_typed (b, "resolved", "object",
              "The container the query resolved to — { \"artist\"?|\"show\"?, "
              "\"artistid\"|\"tvshowid\" } — when it drilled through one.");
  prop_array_obj (b, "rows",
                  "A playable leaf: { \"file\", \"<media>id\", \"label\", "
                  "\"title\", … } plus per-media fields (artist/album/track, "
                  "showtitle/season/episode, year). \"file\" is the playfile "
                  "input.",
                  "The matching leaf rows, paged.");

  static const char *const required[] =
    { "type", "total", "returned", "offset", "truncated", "rows", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * out_schema_contributors:
 * @self: the table (unused; the shape is fixed).
 *
 * Output schema of `contributors`: the paged envelope built by
 * build_contributors_result() over the merged person rows.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_contributors (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  paging_out_props (b);
  prop_array_obj (b, "rows",
                  "{ \"name\", \"in\": [\"albums\"|\"songs\"|\"movies\"|"
                  "\"tvshows\"] } — the containers where the person yields "
                  "hits; feed the exact name back into searchmedia.",
                  "The matching people, merged and sorted by name.");

  static const char *const required[] =
    { "total", "returned", "offset", "truncated", "rows", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * out_schema_getplaylist:
 * @self: the table (unused; the shape is fixed).
 *
 * Output schema of `getplaylist`: the queue listing, with `type` and
 * `position` present only when resolvable (see handler_getplaylist()).
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_getplaylist (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  static const char *const types[] = { "audio", "video", "picture", NULL };
  prop_enum (b, "type", types, "The playlist read, when one was resolved.");
  prop_typed (b, "total", "integer", "Number of queued items.");
  prop_typed (b, "position", "integer",
              "Index of the now-playing item — present only when the "
              "playlist read is the active player's own.");
  prop_array_obj (b, "items",
                  "{ \"<type>id\"?, \"file\"?, \"label\"?, \"type\" } — the id "
                  "key is dynamic (songid/episodeid/…) and re-queueable "
                  "as-is; file is playfile-ready.",
                  "The queued items, in playback order.");

  static const char *const required[] = { "total", "items", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * out_schema_history:
 * @self: the table (unused; the shape is fixed).
 *
 * Output schema of the `history` tool: the paged envelope built by
 * handler_history(), echoing the window/instance filters when given.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_history (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_typed (b, "instance", "string",
              "The instance filter, echoed when one was given.");
  prop_typed (b, "since", "string",
              "The window's lower bound, echoed when one was given.");
  prop_typed (b, "until", "string",
              "The window's upper bound, echoed when one was given.");
  paging_out_props (b);
  prop_array_obj (b, "entries",
                  "{ \"at\", \"last_seen\"?, \"instance\", \"name\"?, "
                  "\"kind\", \"media\"?, \"id\"?, \"title\"?, \"file\"?, … } "
                  "plus per-media fields (artist/album/track, showtitle/"
                  "season/episode). at = first sighting (ISO-8601 UTC), "
                  "last_seen appears once a play is re-observed.",
                  "The matching history records.");

  static const char *const required[] =
    { "total", "returned", "offset", "truncated", "entries", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/* ---- notify — push a toast onto the Kodi on-screen display. -------------- */

/**
 * schema_notify:
 * @self: the table (used to name the configured instances).
 *
 * Schema for the `notify` tool: the required `title` + `message`, plus the
 * optional `image` (a built-in icon keyword or any image path/URL) and
 * `displaytime`, alongside the optional `instance`.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
schema_notify (MkTools *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_instance (b, self, FALSE);
  prop_typed (b, "title", "string",
              "Notification title — the bold heading of the toast.");
  prop_typed (b, "message", "string",
              "Notification body text shown beneath the title.");
  prop_typed (b, "image", "string",
              "Icon for the toast: one of the keywords \"info\", \"warning\" "
              "or \"error\" for a built-in icon, or any image path/URL the "
              "Kodi box can reach. Omitted uses Kodi's default icon.");
  prop_typed (b, "displaytime", "integer",
              "How long the toast stays on screen, in milliseconds. Omitted "
              "uses Kodi's default of 5000; Kodi clamps very small values to "
              "its own minimum.");

  static const char *const required[] = { "title", "message", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * out_schema_notify:
 * @self: the table (unused; the shape is fixed).
 *
 * Output schema of `notify`: the confirmation echo built by handler_notify() —
 * `{ "shown", "title", "message", "displaytime", "image"? }`. `image` is
 * present only when one was supplied.
 *
 * @return a newly allocated schema node.
 */
static JsonNode *
out_schema_notify (MkTools *self)
{
  (void) self;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  schema_begin (b);

  prop_typed (b, "shown", "boolean",
              "Always true — the notification was handed to Kodi.");
  prop_typed (b, "title", "string", "The title that was shown.");
  prop_typed (b, "message", "string", "The message that was shown.");
  prop_typed (b, "displaytime", "integer",
              "The display time in milliseconds — the requested value, or "
              "5000 (Kodi's default) when none was given.");
  prop_typed (b, "image", "string",
              "The icon keyword or image path that was used, present only "
              "when one was supplied.");

  static const char *const required[] =
    { "shown", "title", "message", "displaytime", NULL };
  schema_end (b, required);
  return json_builder_get_root (b);
}

/**
 * handler_notify:
 * @self: the tool table.
 * @def: this tool's row (unused).
 * @args: the call arguments; `title` + `message` are required, `image` and
 *        `displaytime` optional, plus the optional `instance`.
 * @error: return location for a GError, or NULL.
 *
 * Implements the `notify` tool: pushes a toast onto the Kodi player's
 * on-screen display with `GUI.ShowNotification`. The `image`
 * argument is forwarded verbatim — Kodi reads the keywords "info"/"warning"/
 * "error" as built-in icons and anything else as an image path/URL — and
 * `displaytime` is forwarded only when given, letting Kodi apply its 5000ms
 * default otherwise.
 *
 * Unlike the transport tools this changes nothing about playback, so rather
 * than the player_state() snapshot it returns a small confirmation echo of
 * what was displayed: `{ "shown": true, "title", "message", "displaytime",
 * "image"? }`. `displaytime` echoes the requested value, or 5000 (Kodi's
 * documented default) when none was given, so the echo is self-consistent.
 *
 * @return the confirmation echo, or NULL with @error set (missing `title`/
 *         `message`, or a call failure).
 */
static JsonNode *
handler_notify (MkTools *self, const MkToolDef *def, JsonObject *args,
                GError **error)
{
  (void) def;
  const char *instance = arg_instance (args);
  const char *title = arg_str (args, "title", NULL);
  const char *message = arg_str (args, "message", NULL);
  const char *image = arg_str (args, "image", NULL);

  if (title == NULL || title[0] == '\0')
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "notify: \"title\" is required");
      return NULL;
    }
  if (message == NULL || message[0] == '\0')
    {
      g_set_error (error, MK_TOOLS_ERROR, MK_TOOLS_ERROR_INVALID_ARGS,
                   "notify: \"message\" is required");
      return NULL;
    }

  gint64 displaytime = 5000; /* Kodi's documented default. */
  gboolean have_dt = arg_int (args, "displaytime", &displaytime);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "title");
  json_builder_add_string_value (b, title);
  json_builder_set_member_name (b, "message");
  json_builder_add_string_value (b, message);
  if (image != NULL && image[0] != '\0')
    {
      json_builder_set_member_name (b, "image");
      json_builder_add_string_value (b, image);
    }
  if (have_dt)
    {
      json_builder_set_member_name (b, "displaytime");
      json_builder_add_int_value (b, displaytime);
    }
  json_builder_end_object (b);
  g_autoptr (JsonNode) params = json_builder_get_root (b);

  g_autoptr (JsonNode) shown = mk_kodi_call (self->kodi, instance,
                                             "GUI.ShowNotification", params,
                                             error);
  if (shown == NULL)
    return NULL;

  /* Confirmation echo — notify changes no playback state, so there is no
   * player_state() to report. Echo what was displayed instead. */
  g_autoptr (JsonBuilder) r = json_builder_new ();
  json_builder_begin_object (r);
  json_builder_set_member_name (r, "shown");
  json_builder_add_boolean_value (r, TRUE);
  json_builder_set_member_name (r, "title");
  json_builder_add_string_value (r, title);
  json_builder_set_member_name (r, "message");
  json_builder_add_string_value (r, message);
  json_builder_set_member_name (r, "displaytime");
  json_builder_add_int_value (r, displaytime);
  if (image != NULL && image[0] != '\0')
    {
      json_builder_set_member_name (r, "image");
      json_builder_add_string_value (r, image);
    }
  json_builder_end_object (r);
  return json_builder_get_root (r);
}

/* ---- The tool table -------------------------------------------------------
 *
 * The tools are grouped by tool kind — Buttons, Knobs, config, search,
 * playback, history, and the rpc escape hatch. Each tool is a
 * `{ name, description, schema, handler, action, out_schema }` row; a NULL
 * handler yields a clean "not implemented" result, a NULL out_schema means
 * the result has no fixed shape to declare (`rpc`).
 */
static const MkToolDef mk_tool_defs[] = {
  /* ---- Buttons — argument-free remote keypresses via Input.ExecuteAction.
   *      Each fires handler_button() with the `action` field. ---- */

  /**
   * play (Button):
   *
   * Press Play on the Kodi remote — resume a paused player on the target
   * instance. A remote keypress, not a player command: no playerid and no
   * active-player resolution. It cannot start new content — with nothing
   * loaded the keypress lands on the idle GUI and does nothing; starting
   * something is `playfile`'s job.
   *
   * Call:  Input.ExecuteAction { "action": "play" }, then player_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the resulting player-state snapshot: `{ "state":
   *         "playing"|"paused"|"stopped", "type", "time", "totaltime",
   *         "progress", "nowplaying": { "media", "id", "file", "label",
   *         "title", … } }` (fields present when a player is active). With
   *         nothing loaded the
   *         action is a no-op and the reply is `{ "state": "stopped" }`.
   */
  { "play", "Press Play on the Kodi remote: resumes paused playback only — it "
            "cannot start new content (with nothing loaded it is a no-op); "
            "use playfile to start something. Returns the player-state "
            "snapshot { \"state\", \"type\", \"time\", \"totaltime\", "
            "\"progress\", \"nowplaying\": { \"media\", \"id\", \"title\", "
            "\"artist\", … } }.",
    schema_instance_only, handler_button, "play", out_schema_snapshot },

  /**
   * pause (Button):
   *
   * Press Pause on the Kodi remote — pause the active player on the target
   * instance. A remote keypress, not a player command: no playerid and no
   * active-player resolution.
   *
   * Call:  Input.ExecuteAction { "action": "pause" }, then player_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the resulting player-state snapshot (player_state()). With nothing
   *         loaded the action is a no-op and the reply is `{ "state": "stopped" }`.
   */
  { "pause", "Press Pause on the Kodi remote: pause the active player on the "
             "target instance. Returns the player-state snapshot { \"state\", "
             "\"type\", \"time\", \"totaltime\", \"progress\", \"nowplaying\": "
             "{ \"media\", \"id\", \"title\", \"artist\", … } }.",
    schema_instance_only, handler_button, "pause", out_schema_snapshot },

  /**
   * stop (Button):
   *
   * Press Stop on the Kodi remote — stop the active player on the target
   * instance — and clear the playlist it was consuming. Kodi's
   * stop ends playback but leaves the queue intact, so without the clear any
   * items queued behind the stopped one would linger invisibly; to the caller
   * "stop" means "done with this", queue included. Live/non-playlist playback
   * and an already-stopped player skip the clear (nothing to remove); other
   * playlists are untouched (`dropplaylists` clears everything).
   *
   * Call:  Player.GetActivePlayers + Player.GetProperties to resolve the
   *        consumed playlistid (before the keypress kills the player),
   *        Input.ExecuteAction { "action": "stop" }, Playlist.Clear on that
   *        playlist, then player_state() to report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the resulting player-state snapshot (player_state()); after a
   *         successful stop nothing is loaded, so `{ "state": "stopped" }`.
   */
  { "stop", "Press Stop on the Kodi remote: stop the active player on the "
            "target instance and clear the playlist it was playing, so no "
            "queued items linger. Returns the player-state snapshot — "
            "{ \"state\": \"stopped\" } after a successful stop.",
    schema_instance_only, handler_stop, "stop", out_schema_snapshot },

  /**
   * skipnext (Button):
   *
   * Press Skip Next on the Kodi remote — advance the active player to the next
   * item in its playlist (or the next chapter). A remote keypress, not a player
   * command: no playerid and no active-player resolution. With nothing loaded
   * the action is a no-op.
   *
   * Call:  Input.ExecuteAction { "action": "skipnext" }, then player_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the resulting player-state snapshot (player_state()). With nothing
   *         loaded the action is a no-op and the reply is `{ "state": "stopped" }`.
   */
  { "skipnext", "Press Skip Next (Next track / chapter) on the Kodi remote: "
                "advance the active player to the next item in its playlist "
                "(or next chapter). A remote keypress, not a player command; "
                "with nothing loaded it is a no-op. Returns the player-state "
                "snapshot { \"state\", \"type\", \"time\", \"totaltime\", "
                "\"progress\", \"nowplaying\": { \"media\", \"id\", \"title\", "
                "\"artist\", … } }.",
    schema_instance_only, handler_button, "skipnext", out_schema_snapshot },

  /**
   * skipprevious (Button):
   *
   * Press Skip Previous on the Kodi remote — go to the previous item in the
   * playlist, or restart the current item (matching Kodi's own button). A remote
   * keypress, not a player command: no playerid and no active-player resolution.
   * With nothing loaded the action is a no-op.
   *
   * Call:  Input.ExecuteAction { "action": "skipprevious" }, then player_state()
   *        to report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the resulting player-state snapshot (player_state()). With nothing
   *         loaded the action is a no-op and the reply is `{ "state": "stopped" }`.
   */
  { "skipprevious", "Press Skip Previous (Previous track / chapter) on the Kodi "
                    "remote: go to the previous item in the playlist (or "
                    "restart the current item, matching Kodi's own button). A "
                    "remote keypress, not a player command; with nothing loaded "
                    "it is a no-op. Returns the player-state snapshot "
                    "{ \"state\", \"type\", \"time\", \"totaltime\", "
                    "\"progress\", \"nowplaying\": { \"media\", \"id\", \"title\", "
                    "\"artist\", … } }.",
    schema_instance_only, handler_button, "skipprevious", out_schema_snapshot },

  /**
   * mute (Button):
   *
   * Mute the target instance's audio output. Not a remote keypress: sets the
   * mute state directly with Application.SetMute, so it is always on
   * regardless of the current state.
   *
   * Call:  Application.SetMute { "mute": true }, then app_audio_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the resulting audio-state snapshot: `{ "muted": true, "volume": <int> }`
   *         (app_audio_state()).
   */
  { "mute", "Mute the target instance's audio output. Returns { \"muted\", "
            "\"volume\" }.",
    schema_instance_only, handler_mute, "true", out_schema_audio },

  /**
   * unmute (Button):
   *
   * Unmute the target instance's audio output. Not a remote keypress: sets the
   * mute state directly with Application.SetMute, so it is always off
   * regardless of the current state.
   *
   * Call:  Application.SetMute { "mute": false }, then app_audio_state() to
   *        report the effect.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the resulting audio-state snapshot: `{ "muted": false, "volume": <int> }`
   *         (app_audio_state()).
   */
  { "unmute", "Unmute the target instance's audio output. Returns "
              "{ \"muted\", \"volume\" }.",
    schema_instance_only, handler_mute, "false", out_schema_audio },

  /* ---- Knobs — adjust a scalar relative to its live value. ---- */

  /**
   * volume (Knob):
   *
   * Adjust the target instance's volume *relatively*. Never set an
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
   *        uses the configured default.
   * @return the resulting audio snapshot with the fixed scale bounds:
   *         `{ "muted": <bool>, "volume": <int>, "min": 0, "max": 100 }`.
   */
  { "volume", "Adjust the target instance's volume by a relative step (in "
              "percentage points); step 0 or omitted just reports the current "
              "volume. Returns { \"muted\", \"volume\", \"min\", \"max\" }.",
    schema_volume, handler_volume, NULL, out_schema_volume },

  /**
   * nowplaying (Button):
   *
   * Do nothing to the player, just report its state. Fires no action and
   * changes nothing; it only builds and returns the player_state()
   * snapshot. Because that snapshot is read from Kodi, a successful call also
   * confirms the instance is reachable — a reachability + state probe.
   *
   * Call:  no Kodi action; player_state() to report what is loaded/playing.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the current player-state snapshot (player_state()), identical in
   *         shape to what the transport Buttons return. With nothing loaded the
   *         reply is `{ "state": "stopped" }`. A communication failure instead
   *         yields the categorised tool error, telling the caller Kodi is
   *         unreachable and why.
   */
  { "nowplaying", "Report what is playing on the target instance without "
                  "changing anything — also a reachability and state probe. "
                  "Returns the player-state snapshot { \"state\", \"type\", "
                  "\"time\", \"totaltime\", \"progress\", \"audio\", "
                  "\"subtitle\" (audio/subtitle for video: count + current "
                  "track), \"nowplaying\": { \"media\", \"id\", \"title\", "
                  "\"artist\", … } }; { \"state\": \"stopped\" } when idle.",
    schema_instance_only, handler_noop, NULL, out_schema_snapshot },

  /* ---- Config tools — read/write the server's own instance config.
   *      These make no Kodi call. ---- */

  /**
   * instances (Config tool):
   *
   * Read or modify the set of configured Kodi instances — the boxes every
   * other tool targets by key. Makes no Kodi call; `set`/`remove` persist the
   * config file atomically. The `action` argument selects the operation:
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
   *         "has_auth", "allow_rpc" } ], "build" }`. `allow_rpc` is read-only
   *         here — it can only be changed by hand-editing the config file.
   *         `build` is this server's build ID.
   */
  { "instances", "Read or modify the configured Kodi instances "
                 "(action: get/set/remove). Manages the MCP server's own "
                 "config, not a Kodi device. Returns { \"default\", "
                 "\"instances\": [ { \"key\", \"host\", \"scheme\", "
                 "\"insecure\", \"has_auth\", \"allow_rpc\", … } ], "
                 "\"build\" }; build is this server's build ID.",
    schema_instances, handler_instances, NULL, out_schema_instances },

  /* ---- Search tools — resolve the library by name down to playable leaf
   *      files; no Kodi state change. ---- */

  /**
   * searchmedia (Search tool):
   *
   * Find playable leaf files by name across the three media types.
   * Drills each type's hierarchy to leaf rows, descending to
   * the album/season level only when the user names one — so a wide query (bare
   * artist, bare show) is a single Kodi call:
   *
   *   - music:   GetArtists(artist) → GetSongs {artistid} directly, or
   *              → GetAlbums(+title) → GetSongs {albumid} when an album is
   *              named; an album-only query (no artist) resolves the title
   *              library-wide via GetAlbums and drills the one matched album.
   *   - tv-show: GetTVShows(title) → GetEpisodes {tvshowid, season?} (+ episode
   *              number / actor / director filters, and-combined); a
   *              person-only query (no title) skips the resolve and filters
   *              GetEpisodes library-wide, rows carrying `showtitle`.
   *   - movie:   GetMovies(title/actor/director, and-combined) → file directly
   *              (no sublevels).
   *
   * Always reports `total` (Kodi's full match count); `count: true` returns it
   * with zero rows. `limit`/`offset` page the leaves; the default cap (50)
   * applies when `limit` is omitted, flagging `truncated`. A substring album
   * `title` matching several albums is collected app-side and flagged
   * `approximate`.
   *
   * @param type (required): "music" | "tv-show" | "movie".
   * @param artist: music performer (substring); music needs at least one of
   *        artist/title.
   * @param actor|director: movie/tv-show person filter (substring). tv-show
   *        matches episode-level cast (guests included) / per-episode directors.
   * @param title: album/show/movie name (substring); tv-show needs at least
   *        one of title/actor/director.
   * @param season|number: narrow tv episodes / pin a track or episode (tv
   *        needs `title` for these).
   * @param limit|offset|count: paging and count-only controls.
   * @param instance (optional): the Kodi instance whose library to search; omitted
   *        uses the configured default.
   * @return `{ "type", "total", "returned", "offset", "truncated",
   *         "approximate"?, "resolved"?, "rows": [ { "file", "<id>", "label",
   *         "title", … } ] }`. Each row's `file` is the `playfile` input.
   */
  { "searchmedia", "Find playable files by name: music/tv-show/movie, "
                   "drilled to leaf files with paging (limit/offset) and a "
                   "total count. For music, title means the ALBUM name; match "
                   "a song by its own title with the `song` arg (\"play Hey "
                   "Jude\" needs no artist). Find a tv episode by its own name "
                   "with the `episodetitle` arg. Movie and "
                   "tv-show queries can also filter by actor/director. Finds "
                   "media items only — for people lookups (bands, artists, "
                   "who is in the library) use `contributors`. Returns "
                   "{ \"type\", \"total\", \"returned\", \"offset\", "
                   "\"truncated\", \"rows\": [ { \"file\", \"<media>id\", "
                   "\"label\", \"title\", … } ] }.",
    schema_search, handler_search, NULL, out_schema_search },

  /**
   * contributors (Search tool):
   *
   * Find or list *people* — bands, solo artists, composers, actors,
   * directors — not media. Answers "do we have anything with X" and "what
   * bands do we have" across the whole library and hands back the exact names
   * to feed into `searchmedia`: names are the only follow-up handle (Kodi's person
   * filters are name-only; no id-based filter exists), so rows carry no person
   * ids. Each row's `in` lists the containers where that name yields hits —
   * `albums`/`songs` for music people (`albums` only when the artist is an
   * album artist), `movies`/`tvshows` for actors and directors — so the caller
   * knows where to drill next without Kodi-specific knowledge. Hits from every
   * source are merged case-insensitively into one row per person, sorted by
   * name. The `band` type is synthesized from `isalbumartist` — Kodi's native
   * group/person fields are empty on tag-scraped libraries.
   *
   * Call:  AudioLibrary.GetArtists for the music people — filter: artist
   *        contains name and/or role is "Composer"
   *        (type composer); albumartistsonly: true for type band, else false
   *        with allroles — then Files.GetDirectory over the virtual people
   *        nodes videodb://movies/actors/, videodb://movies/directors/ and
   *        videodb://tvshows/actors/ — those listings take
   *        no filter, so they are paged and matched app-side. `type` skips the
   *        sources that cannot hold it (band/composer: no node walks; actor/
   *        director: no music call).
   *
   * @param name (optional): person name (substring, case-insensitive);
   *        omitted matches everyone.
   * @param type (optional): band|composer|actor|director — one contributor
   *        kind; omitted spans all of them. {type: "band"} alone lists every
   *        band.
   * @param limit|offset|count: paging and count-only controls over the merged
   *        row set.
   * @param instance (optional): the Kodi instance whose library to search;
   *        omitted uses the configured default.
   * @return `{ "total", "returned", "offset", "truncated", "rows": [ { "name",
   *         "in": [ "albums"|"songs"|"movies"|"tvshows" ] } ] }`.
   */
  { "contributors", "Find or list contributors (bands, solo artists, "
                    "composers, actors, directors): optional name substring, "
                    "optional type (band/composer/actor/director) — e.g. "
                    "{type: \"band\"} lists all bands. Rows {name, in: "
                    "[albums|songs|movies|tvshows]} say where each name yields "
                    "hits — feed the exact name back into `searchmedia` to "
                    "drill. Returns { \"total\", \"returned\", \"offset\", "
                    "\"truncated\", \"rows\" }.",
    schema_contributors, handler_contributors, NULL, out_schema_contributors },

  /* ---- Playback tools — open content on a player. ---- */

  /**
   * playfile (Playback tool):
   *
   * Play one file by path — the partner to `searchmedia`.
   * Takes a `file` (typically the `file` field of a `searchmedia` result row) and
   * opens it with Player.Open `{ "item": { "file": <file> } }`. Kodi
   * auto-selects the audio vs video player from the file type and, when the path
   * matches a scanned library item, enriches the now-playing state back to its
   * library id. Plays a single file; for a multi-leaf `searchmedia` result the caller
   * picks which row to play. Works for any path Kodi can reach, in-library or
   * not. A file missing from disk is a proper tool error, never a false
   * success: it is refused before the open when verifiable on disk, and an
   * open that Kodi accepted but that silently never started (a stale library
   * entry) is detected from the post-wait snapshot.
   *
   * Call:  Files.GetDirectory (existence pre-check) → Player.Open { "item":
   *        { "file": <file> } }, then player_state() to report what is now
   *        loaded/playing.
   * @param file (required): path of the file to play.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the resulting player-state snapshot (player_state()): `{ "state":
   *         "playing"|"paused", "type", "time", "totaltime", "progress",
   *         "nowplaying": { "media", "id", "file", "label", "title", … } }` — a
   *         missing or never-starting file is an error, not a "stopped"
   *         snapshot.
   */
  { "playfile", "Play one file by path (e.g. a `searchmedia` result's `file`): "
                "Player.Open auto-selects the audio/video player. Works for any "
                "reachable path, in-library or not. A file missing from disk "
                "(stale library entry) is reported as an error. Returns the "
                "player-state snapshot { \"state\", \"type\", \"time\", "
                "\"totaltime\", \"progress\", \"nowplaying\": { \"media\", "
                "\"id\", \"title\", \"artist\", … } }.",
    schema_playfile, handler_playfile, NULL, out_schema_snapshot },

  /**
   * queue (Playback tool):
   *
   * Append an item behind the already-playing one, so playback continues into
   * it. Requires something to be playing: a single `play`/`playfile`
   * auto-creates the one-item playlist, so this tool never builds or
   * starts a playlist itself — no active player, or non-playlist playback
   * (live TV / streams), is the one "nothing queueable is playing" error. The
   * item is a `searchmedia` row's library id (`type` + `id`) or `file` path;
   * `next: true` inserts it right after the current item instead of appending.
   * A library id whose kind does not match the playing queue (song ↔ audio,
   * episode/movie ↔ video) is refused — Kodi would accept the foreign item
   * silently; a `file`'s type is unknowable, so for files matching stays the
   * caller's job. An item whose file is verifiably missing from disk is
   * refused as well — Kodi would accept it and the failure would
   * only surface when playback reaches it.
   *
   * Call:  Player.GetActivePlayers → Player.GetProperties (playlistid,
   *        position) → existence pre-check (a library id resolved via its
   *        Get<Kind>Details, then Files.GetDirectory) → Playlist.Add, or
   *        Playlist.Insert at position+1 for `next` — then player_state()
   *        (playback unchanged).
   * @param type: "song" | "episode" | "movie"; required with `id`.
   * @param id: library id of the item to queue (give `id` or `file`).
   * @param file: path of the file to queue (give `id` or `file`).
   * @param next (optional): insert right after the current item.
   * @param instance (optional): name of the Kodi instance to target; omitted
   *        uses the configured default.
   * @return the player-state snapshot (player_state()), unchanged by the add —
   *         the current item keeps playing with the new one queued behind it.
   */
  { "queue", "Queue an item behind the one now playing, for continuous "
             "playback: a `searchmedia` row's library id (type+id) or a file path; "
             "next:true plays it right after the current item. Something must "
             "already be playing (start with play/playfile). An item whose "
             "file is missing from disk (stale library entry) is refused. "
             "Returns the player-state snapshot, unchanged by the add — the "
             "current item keeps playing.",
    schema_queue, handler_queue, NULL, out_schema_snapshot },

  /**
   * getplaylist (Playback tool):
   *
   * Read the queue without changing anything — the inspection
   * partner of `queue` and the audit step before `dropplaylists`.
   * With no `type`, reads the playlist behind the active player (resolved
   * like `queue`); a given `type` (audio/video/picture) always wins,
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
   *        configured default.
   * @return `{ "type"?, "total", "position"?, "items": [ { "<type>id"?,
   *         "file"?, "label"?, "type" } ] }` — read-only, playback untouched.
   */
  { "getplaylist", "Read the queue without changing it: the items of the "
                   "active player's playlist — or of a named one (audio/"
                   "video/picture), which always wins — plus the position of "
                   "the now-playing item. An empty queue is an empty list. "
                   "Returns { \"type\"?, \"total\", \"position\"?, \"items\": "
                   "[ { \"<type>id\", \"file\", \"label\", \"type\" } ] }.",
    schema_getplaylist, handler_getplaylist, NULL, out_schema_getplaylist },

  /**
   * dropplaylists (Playback tool):
   *
   * Empty the queues: clear all three of Kodi's fixed playlists (audio /
   * video / picture) in one call, so no queued items remain anywhere.
   * Plural by design — there is no picking one; clearing only the
   * playlist being played is what `stop` does. Clearing the
   * active playlist leaves the current item playing — only the queue behind
   * it is emptied — so playback is never interrupted; to also stop what is
   * playing, call `stop`. Read the queues first with `getplaylist`
   * if their content matters: the drop is not undoable.
   *
   * Call:  Playlist.Clear { playlistid } for ids 0, 1 and 2, then
   *        player_state() to report the (unchanged) playback.
   * @param instance (optional): the Kodi instance to target; omitted uses the
   *        configured default.
   * @return the player-state snapshot (player_state()) — whatever was playing
   *         still is, now with nothing queued behind it; idle boxes report
   *         `{ "state": "stopped" }`.
   */
  { "dropplaylists", "Empty all queues: clear the audio, video and picture "
                     "playlists in one call. The current item keeps playing — "
                     "only the queued items behind it are removed — so "
                     "playback is never interrupted. Not undoable; inspect "
                     "with getplaylist first if the content matters. Returns "
                     "the player-state snapshot — whatever was playing still "
                     "is, with nothing queued behind it.",
    schema_instance_only, handler_dropplaylists, NULL, out_schema_snapshot },

  /* ---- History tool — read back the local playback log; no Kodi call. ---- */

  /**
   * history (History tool):
   *
   * List recently played items from the local playback log — the
   * backward-looking record of what the assistant caused or observed,
   * written as a side effect of every playback-affecting call. Reads only the
   * local `history.json`; makes no Kodi request, so it answers even when no box
   * is reachable. Mirror of the write path: mk_history_read() under a shared
   * lock, wrapped in a `searchmedia`-style envelope.
   *
   * @param since (optional): ISO-8601 lower bound; omitted = start of the log.
   * @param until (optional): ISO-8601 upper bound; omitted = now.
   * @param instance (optional): restrict to one box. **Omitted spans ALL
   *        instances** — unlike other tools, where omitted means the default
   *        box.
   * @param media|kind|artist|match|id (optional): app-side entry filters —
   *        media kind (song/episode/movie/…), broad audio/video kind,
   *        performer substring, free-text substring over the human fields,
   *        exact library id — all AND-combined with the window.
   * @param limit (optional): per-page cap (default 50, max 1000).
   * @param offset|order|count (optional): paging (entries to skip; "newest" or
   *        "oldest" first) and count-only mode (total with zero entries).
   * @return `{ "instance"?, "since"?, "until"?, "total", "returned", "offset",
   *         "truncated", "entries": [ { "at", "last_seen"?, "instance",
   *         "name"?, "kind", "media"?, "id"?, "title"?, … } ] }` — each entry
   *         a history record, newest first by default (`at` = earliest
   *         sighting, `last_seen` = latest, present once re-observed); `total`
   *         counts matches before the limit.
   */
  /* ---- On-screen display — push a toast to the box's screen. ---- */

  /**
   * notify (Display):
   *
   * Push a message onto the Kodi player's on-screen display — a toast with a
   * title + message, optional icon and display time. Changes nothing about
   * playback, so unlike the transport tools it does not report player_state();
   * it returns a confirmation echo of what was shown.
   *
   * Call:  GUI.ShowNotification { "title", "message", "image"?, "displaytime"? }.
   * @param title (required): the toast's bold heading.
   * @param message (required): the toast's body text.
   * @param image (optional): "info"/"warning"/"error" for a built-in icon, or
   *        any image path/URL the box can reach; omitted uses Kodi's default.
   * @param displaytime (optional): milliseconds on screen; omitted uses Kodi's
   *        5000ms default (Kodi clamps very small values).
   * @param instance (optional): the Kodi instance to target; omitted uses the
   *        configured default.
   * @return the confirmation echo `{ "shown": true, "title", "message",
   *         "displaytime", "image"? }`.
   */
  { "notify", "Show a notification toast on the Kodi box's screen: a title and "
              "message, with an optional icon (\"info\"/\"warning\"/\"error\" "
              "or any image path/URL) and display time in milliseconds. "
              "Doesn't touch playback. Returns { \"shown\", \"title\", "
              "\"message\", \"displaytime\", \"image\"? }.",
    schema_notify, handler_notify, NULL, out_schema_notify },

  { "history", "List recently played items from the local playback log. Filter "
               "with an ISO-8601 window (since/until), media/kind/artist, a "
               "free-text match or an exact id; page with limit/offset/order, "
               "or ask for a count only. An omitted instance returns all "
               "boxes. Reads only the local log — no Kodi call, so it works "
               "even when no box is reachable. Returns { \"total\", "
               "\"returned\", \"offset\", \"truncated\", \"entries\": [ "
               "{ \"at\", \"instance\", \"kind\", \"media\", \"title\", "
               "\"artist\", … } ] }.",
    schema_history, handler_history, NULL, out_schema_history },

  /**
   * audio_and_subtitle_info (read-only):
   *
   * Report what audio tracks and subtitles a movie or episode carries
   * **before** playback, from Kodi's cached `streamdetails` — no playback,
   * nothing mutated. The read-only first step of the audio/subtitle group: the
   * per-stream `index` it surfaces is the handle the during-playback setters
   * select by.
   *
   * Resolves the item like `queue` (exactly one of `type`+`id` or `file`) and
   * calls `VideoLibrary.GetMovieDetails`/`GetEpisodeDetails` or
   * `Files.GetFileDetails` with `properties: ["streamdetails","file"]`.
   *
   * Call:  the resolving *Details method above, then shape its streamdetails.
   * @param type: library kind for `id` — "movie" or "episode" (with `id`).
   * @param id: library id (movieid/episodeid) of the item — give exactly one
   *        of `id` (with `type`) or `file`.
   * @param file: path to inspect — give exactly one of `id` or `file`.
   * @param instance (optional): the Kodi instance to target; omitted uses the
   *        configured default.
   * @return `{ "file", "audio":[{ "index","codec","channels","language" }],
   *         "subtitle":[{ "index","language" }], "video":[{ "index", … }] }`.
   *         Embedded streams only — sidecar subs and unscanned files come back
   *         as empty arrays.
   */
  { "audio_and_subtitle_info",
    "Inspect a movie or episode's audio tracks and subtitles BEFORE playback, "
    "from Kodi's cached streamdetails (no playback). Name the item by "
    "type+id (a searchmedia movie/episode row) or by file. Returns { \"file\", "
    "\"audio\":[{ \"index\", \"codec\", \"channels\", \"language\" }], "
    "\"subtitle\":[{ \"index\", \"language\" }], \"video\":[…] }. Embedded "
    "streams only: external .srt sidecars and files Kodi hasn't scanned come "
    "back as empty arrays; \"index\" maps to the audio/subtitle setters for "
    "embedded streams.",
    schema_audio_and_subtitle_info, handler_audio_and_subtitle_info, NULL,
    out_schema_audio_and_subtitle_info },

  /**
   * set_audio_and_subtitle (during-playback setter):
   *
   * Switch the audio track and/or subtitle of the video **now playing**, by the
   * per-stream `index` that `audio_and_subtitle_info` reports — its
   * during-playback partner. `audio` and `subtitle` are each optional and
   * applied only when given (omit one to leave that stream untouched; at least
   * one must be present); `subtitle: "off"` hides subtitles, distinct from
   * omitting it.
   *
   * Resolves the active player like `queue`; with nothing playing or a
   * non-video player it returns a clean tool error. Reuses out_schema_snapshot
   * (like `nowplaying`) — the reply is the standard player-state snapshot, whose
   * top-level `audio`/`subtitle` summary (TODO 7.3) now echoes the resulting
   * track layout, so the caller sees the selection take effect.
   *
   * Call:  Player.GetActivePlayers → Player.SetAudioStream and/or
   *        Player.SetSubtitle (enable:true) → player_state snapshot.
   * @param audio (optional): audio stream index to select.
   * @param subtitle (optional): subtitle stream index to show, or "off".
   * @param instance (optional): the Kodi instance to target; omitted uses the
   *        configured default.
   * @return the player-state snapshot (playback item unchanged).
   */
  { "set_audio_and_subtitle",
    "Switch the audio track and/or subtitle of the video now playing on the "
    "target instance. \"audio\" is a stream index; \"subtitle\" is a stream "
    "index or \"off\" to hide subtitles — use the \"index\" values from "
    "audio_and_subtitle_info. Omit an argument to leave that stream unchanged; "
    "give at least one. Returns the player-state snapshot.",
    schema_set_audio_and_subtitle, handler_set_audio_and_subtitle, NULL,
    out_schema_snapshot },

  /**
   * playfile_with_audio_and_subtitle (when-starting setter):
   *
   * Start playback of a file AND select its audio track / subtitle in one call
   * — the "when starting the playback" partner of `set_audio_and_subtitle`
   * (which acts on the item already playing). `Player.Open` has no audio/
   * subtitle option, so the tool opens the file, waits for the stream, then
   * applies the same `Player.SetAudioStream`/`SetSubtitle` selection. Video
   * only.
   *
   * Reuses `playfile`'s opener (missing-media layers + start check) and
   * `set_audio_and_subtitle`'s selectors and applier; reuses out_schema_snapshot
   * — the reply is the standard player-state snapshot, whose top-level
   * `audio`/`subtitle` summary (TODO 7.3) echoes the resulting layout.
   *
   * Call:  Player.Open → settle → Player.GetActivePlayers → Player.SetAudioStream
   *        and/or Player.SetSubtitle (enable:true) → player_state snapshot.
   * @param file (required): path of the video file to play.
   * @param audio (optional): audio stream index to select.
   * @param subtitle (optional): subtitle stream index to show, or "off".
   * @param instance (optional): the Kodi instance to target; omitted uses the
   *        configured default.
   * @return the post-selection player-state snapshot. At least one of `audio`/
   *         `subtitle` is required.
   */
  { "playfile_with_audio_and_subtitle",
    "Play a video file by path AND select its audio track and/or subtitle once "
    "it starts — the \"when starting playback\" partner of "
    "set_audio_and_subtitle. \"file\" is required; \"audio\" is a stream index "
    "and \"subtitle\" is a stream index or \"off\", using the \"index\" values "
    "from audio_and_subtitle_info. Give at least one of audio/subtitle (else use "
    "playfile). Video only. Returns the player-state snapshot.",
    schema_playfile_with_audio_and_subtitle,
    handler_playfile_with_audio_and_subtitle, NULL, out_schema_snapshot },

  /* ---- Escape hatch — raw JSON-RPC passthrough, opt-in per instance
   *      (gated by `allow_rpc`). ---- */

  /**
   * rpc (Escape hatch):
   *
   * Send any JSON-RPC method to a Kodi box and get its reply back unchanged —
   * the catch-all for anything the dedicated tools don't model.
   * Builds the JSON-RPC envelope and POSTs it to the target instance, returning
   * Kodi's raw `result` verbatim; a Kodi error becomes a tool error.
   *
   * Powerful and unconstrained, so it is OFF by default and gated per instance:
   * a box permits it only when its server config sets `allow_rpc: true`,
   * which is set by hand-editing the config file and cannot be enabled through
   * the `instances` tool. Calling `rpc` on an instance that has not opted in
   * returns a clean tool error and makes no Kodi request.
   *
   * Call:  <method> with the given <params>, returning Kodi's raw result.
   * @param method (required): the JSON-RPC method, e.g. "GUI.ActivateWindow".
   * @param params: parameters object passed straight through (optional).
   * @param instance (optional): the Kodi instance to target; omitted uses the
   *        configured default. The instance must have `allow_rpc` set.
   * @return Kodi's raw `result` for the method, unshaped. On a disabled instance
   *         or a Kodi error, an `isError` result with the detail instead.
   */
  { "rpc", "Escape hatch: send a raw JSON-RPC method to Kodi and return its "
           "reply unchanged. Disabled unless the target instance has opted in "
           "(allow_rpc in the server config, set by hand only).",
    /* No out_schema: the result is Kodi's raw reply, verbatim — there is no
     * fixed shape to declare, and it need not even be an object. */
    schema_rpc, handler_rpc, NULL, NULL },
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
              "names this instance and gives it a host.";
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
 * Shapes a tool failure as machine-readable JSON text, the single source
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
 * result (make_result()); it is never a protocol-level error. @message
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
 * @structured: the same payload as a live node for `structuredContent`, or
 *              NULL to omit the member; borrowed (referenced, not copied).
 * @is_error: whether this result reports a tool-level failure.
 *
 * Builds the `tools/call` result envelope
 * `{ "content": [ { "type": "text", "text": <text> } ],
 *    "structuredContent"?: <structured>, "isError": <bool> }`.
 * The text block always carries the payload — the channel every client reads,
 * whatever protocol revision it negotiated; @structured mirrors it for
 * clients that consume or validate against the tool's declared outputSchema
 * (spec 2025-06-18). It is the success-path twin of that declaration —
 * mk_tools_call() passes it only for tools with an out_schema, and never on
 * errors, whose `{ "error", … }` shape is documented by error_text() instead.
 *
 * @return a newly allocated result node; free with json_node_unref().
 */
static JsonNode *
make_result (const char *text, JsonNode *structured, gboolean is_error)
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

  if (structured != NULL)
    {
      json_builder_set_member_name (b, "structuredContent");
      json_builder_add_value (b, json_node_ref (structured));
    }

  json_builder_set_member_name (b, "isError");
  json_builder_add_boolean_value (b, is_error);

  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/* ---- Argument validation --------------------------------------------------
 *
 * json-glib's getters are permissive: a wrong-typed field (e.g. a string where
 * an integer is wanted) reads back as the default, and a missing field is
 * indistinguishable from one set to its default. Left unchecked, a malformed
 * call returns a misleading success. These helpers validate a call's arguments
 * against the tool's own declared inputSchema — the single source of truth the
 * tool already publishes — and turn a violation into one actionable message the
 * model can correct from, mirroring error_text()'s `{ "error": … }` shape.
 *
 * Scope: required-property presence and primitive `type` matching. Enum
 * membership is left to the handler (which may match case-insensitively and can
 * phrase a domain-specific message); undeclared/extra members and an explicit
 * JSON null are allowed (a null is how `instances set` clears a field).
 */

/**
 * json_type_name:
 * @node: the value/container node whose JSON type to name; borrowed.
 *
 * Names @node's JSON type as it appears in a schema ("string", "integer",
 * "number", "boolean", "array", "object", "null"), for reporting what was
 * actually received in a validation message.
 *
 * @return a borrowed static type-name string.
 */
static const char *
json_type_name (JsonNode *node)
{
  if (JSON_NODE_HOLDS_NULL (node))
    return "null";
  if (JSON_NODE_HOLDS_ARRAY (node))
    return "array";
  if (JSON_NODE_HOLDS_OBJECT (node))
    return "object";
  GType vt = json_node_get_value_type (node);
  if (vt == G_TYPE_STRING)
    return "string";
  if (vt == G_TYPE_INT64)
    return "integer";
  if (vt == G_TYPE_DOUBLE)
    return "number";
  if (vt == G_TYPE_BOOLEAN)
    return "boolean";
  return "value";
}

/**
 * type_matches:
 * @node: the supplied argument value; borrowed.
 * @want: the schema `type` the property declares.
 *
 * Whether @node satisfies the declared JSON Schema @want. A JSON null always
 * passes (callers use an explicit null to clear a value), an "integer" accepts
 * only a whole number while "number" also accepts a real, and an unrecognised
 * @want passes (there is nothing to enforce).
 *
 * @return TRUE when @node is acceptable for @want.
 */
static gboolean
type_matches (JsonNode *node, const char *want)
{
  if (JSON_NODE_HOLDS_NULL (node))
    return TRUE;
  if (g_strcmp0 (want, "string") == 0)
    return JSON_NODE_HOLDS_VALUE (node)
           && json_node_get_value_type (node) == G_TYPE_STRING;
  if (g_strcmp0 (want, "integer") == 0)
    return JSON_NODE_HOLDS_VALUE (node)
           && json_node_get_value_type (node) == G_TYPE_INT64;
  if (g_strcmp0 (want, "number") == 0)
    return JSON_NODE_HOLDS_VALUE (node)
           && (json_node_get_value_type (node) == G_TYPE_INT64
               || json_node_get_value_type (node) == G_TYPE_DOUBLE);
  if (g_strcmp0 (want, "boolean") == 0)
    return JSON_NODE_HOLDS_VALUE (node)
           && json_node_get_value_type (node) == G_TYPE_BOOLEAN;
  if (g_strcmp0 (want, "array") == 0)
    return JSON_NODE_HOLDS_ARRAY (node);
  if (g_strcmp0 (want, "object") == 0)
    return JSON_NODE_HOLDS_OBJECT (node);
  return TRUE; /* unrecognised declared type — nothing to enforce */
}

/**
 * expected_phrase:
 * @want: the schema `type` string.
 *
 * Maps a schema type to its article-prefixed English phrase ("a string", "an
 * integer", …) for a validation message, falling back to @want verbatim.
 *
 * @return a borrowed static phrase, or @want itself when unrecognised.
 */
static const char *
expected_phrase (const char *want)
{
  if (g_strcmp0 (want, "string") == 0)
    return "a string";
  if (g_strcmp0 (want, "integer") == 0)
    return "an integer";
  if (g_strcmp0 (want, "number") == 0)
    return "a number";
  if (g_strcmp0 (want, "boolean") == 0)
    return "a boolean";
  if (g_strcmp0 (want, "array") == 0)
    return "an array";
  if (g_strcmp0 (want, "object") == 0)
    return "an object";
  return want;
}

/**
 * validate_arguments:
 * @self: the tool table, used to build @def's schema.
 * @def: the tool row whose declared inputSchema constrains the call.
 * @args: the call arguments object, or NULL.
 *
 * Checks @args against @def's inputSchema: every `required` property must be
 * present, and every supplied property the schema declares must match its
 * declared `type` (see type_matches() for the null/extra/enum policy). Catches
 * the malformed calls json-glib's getters would otherwise swallow — a
 * wrong-typed field read as its default, or a missing required field — naming
 * each offender with expected-vs-received detail.
 *
 * @return NULL when @args is valid (or @def declares no schema), else a newly
 *         allocated, human-readable error string; free with g_free().
 */
static char *
validate_arguments (MkTools *self, const MkToolDef *def, JsonObject *args)
{
  if (def->schema == NULL)
    return NULL;

  g_autoptr (JsonNode) schema = def->schema (self);
  if (schema == NULL || !JSON_NODE_HOLDS_OBJECT (schema))
    return NULL;
  JsonObject *sobj = json_node_get_object (schema);

  JsonObject *props = NULL;
  JsonNode *pnode = json_object_get_member (sobj, "properties");
  if (pnode != NULL && JSON_NODE_HOLDS_OBJECT (pnode))
    props = json_node_get_object (pnode);

  g_autoptr (GString) errs = g_string_new (NULL);

  /* Required-presence: each name in the schema's `required` array. */
  JsonNode *rnode = json_object_get_member (sobj, "required");
  if (rnode != NULL && JSON_NODE_HOLDS_ARRAY (rnode))
    {
      JsonArray *req = json_node_get_array (rnode);
      guint n = json_array_get_length (req);
      for (guint i = 0; i < n; i++)
        {
          const char *name = json_array_get_string_element (req, i);
          if (name == NULL)
            continue;
          if (args == NULL || !json_object_has_member (args, name))
            {
              if (errs->len > 0)
                g_string_append (errs, "; ");
              g_string_append_printf (
                errs, "argument \"%s\" is required but was not provided", name);
            }
        }
    }

  /* Type-match: each supplied member the schema declares (extras are allowed). */
  if (args != NULL && props != NULL)
    {
      GList *members = json_object_get_members (args);
      for (GList *l = members; l != NULL; l = l->next)
        {
          const char *name = l->data;
          JsonNode *spec = json_object_get_member (props, name);
          if (spec == NULL || !JSON_NODE_HOLDS_OBJECT (spec))
            continue; /* undeclared/extra member — allowed */
          const char *want = json_object_get_string_member_with_default (
            json_node_get_object (spec), "type", NULL);
          JsonNode *val = json_object_get_member (args, name);
          if (want == NULL || type_matches (val, want))
            continue;
          g_autofree char *got = node_to_string (val);
          if (errs->len > 0)
            g_string_append (errs, "; ");
          g_string_append_printf (
            errs, "argument \"%s\" expected %s, but received %s (%s)", name,
            expected_phrase (want), got, json_type_name (val));
        }
      g_list_free (members);
    }

  if (errs->len == 0)
    return NULL;
  return g_strdup_printf ("invalid arguments for tool \"%s\": %s", def->name,
                          errs->str);
}

/* ---- Public API ----------------------------------------------------------- */

/**
 * mk_tools_new:
 * @config: configuration for instance names/defaults; borrowed, not owned.
 * @kodi: the Kodi client handlers drive; borrowed, not owned.
 *
 * Creates the tool table. Both @config and @kodi must outlive it. The table
 * also owns the playback-history log bound to the default path:
 * player_state() feeds the write path and the `history` tool reads it back.
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
 * Renders every tool as `{ "name", "description", "inputSchema",
 * "outputSchema"? }` for `tools/list` — `outputSchema` only for tools that
 * declare a result shape (all but `rpc`). With no tools wired yet this is an
 * empty array.
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
      if (def->out_schema != NULL)
        {
          json_builder_set_member_name (b, "outputSchema");
          json_builder_add_value (b, def->out_schema (self)); /* ownership */
        }
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
 * Dispatches a `tools/call`. For a known tool, returns the result
 * envelope: on success the handler's JSON as text with `isError: false` —
 * mirrored as `structuredContent` when the tool declares an outputSchema; on
 * a handler failure (or a not-yet-implemented tool) the detail as JSON text
 * with `isError: true` — see error_text() for the shape. A server↔player
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
      return make_result (text, NULL, TRUE);
    }

  /* Reject malformed arguments against the tool's declared schema before the
   * handler runs, so a wrong-typed or missing field becomes one actionable
   * error instead of being silently coerced to a default (validate_arguments). */
  g_autofree char *verr = validate_arguments (self, def, arguments);
  if (verr != NULL)
    {
      g_autofree char *text = error_text (verr, NULL, NULL);
      return make_result (text, NULL, TRUE);
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
      return make_result (text, NULL, TRUE);
    }

  /* Tools that declare an outputSchema mirror the payload as
   * structuredContent, per the spec contract the declaration makes. */
  g_autofree char *text = node_to_string (result);
  return make_result (text, def->out_schema != NULL ? result : NULL, FALSE);
}
