/* mcp-kodi — configuration loader/saver. See mk-config.h and ../TODO.md §7. */

#include "config.h"

#include "mk-config.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

struct _MkConfig
{
  char       *default_name;          /* name of the default instance */
  GHashTable *instances;             /* char* name → MkInstance* (owned) */
};

/**
 * mk_config_error_quark:
 *
 * Defined by G_DEFINE_QUARK().
 *
 * @return the GQuark identifying the MK_CONFIG_ERROR domain.
 */
G_DEFINE_QUARK (mk-config-error-quark, mk_config_error)

/* ---- MkInstance ---------------------------------------------------------- */

/**
 * mk_instance_new:
 * @name: human-readable display label, or NULL for none.
 * @host: host (or "host:port") of the Kodi box, or NULL.
 * @auth: HTTP Basic credentials as "user:pass", or NULL for none.
 * @scheme: URL scheme ("http"/"https"); NULL defaults to "https".
 * @insecure: TRUE to accept a self-signed TLS certificate (curl -k).
 * @allow_rpc: TRUE to opt this instance into the `rpc` escape hatch (§7.7).
 *
 * Allocates a new instance, duplicating all string arguments.
 *
 * @return a newly allocated MkInstance; free with mk_instance_free().
 */
MkInstance *
mk_instance_new (const char *name,
                 const char *host,
                 const char *auth,
                 const char *scheme,
                 gboolean    insecure,
                 gboolean    allow_rpc)
{
  MkInstance *inst = g_new0 (MkInstance, 1);
  inst->name = g_strdup (name);
  inst->host = g_strdup (host);
  inst->auth = g_strdup (auth);
  inst->scheme = g_strdup (scheme ? scheme : "https");
  inst->insecure = insecure;
  inst->allow_rpc = allow_rpc;
  return inst;
}

/**
 * mk_instance_copy:
 * @inst: the instance to copy, or NULL.
 *
 * Deep-copies an instance.
 *
 * @return a newly allocated copy (free with mk_instance_free()), or NULL if
 *         @inst was NULL.
 */
MkInstance *
mk_instance_copy (const MkInstance *inst)
{
  if (inst == NULL)
    return NULL;
  return mk_instance_new (inst->name, inst->host, inst->auth, inst->scheme,
                          inst->insecure, inst->allow_rpc);
}

/**
 * mk_instance_free:
 * @inst: the instance to free, or NULL.
 *
 * Frees an instance and its owned strings. Safe to call with NULL.
 */
void
mk_instance_free (MkInstance *inst)
{
  if (inst == NULL)
    return;
  g_free (inst->name);
  g_free (inst->host);
  g_free (inst->auth);
  g_free (inst->scheme);
  g_free (inst);
}

/* ---- MkConfig lifecycle -------------------------------------------------- */

/**
 * mk_config_new:
 *
 * Allocates an empty config with no instances and no default.
 *
 * @return a newly allocated MkConfig; free with mk_config_free().
 */
MkConfig *
mk_config_new (void)
{
  MkConfig *self = g_new0 (MkConfig, 1);
  self->instances = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free,
                                           (GDestroyNotify) mk_instance_free);
  return self;
}

/**
 * mk_config_free:
 * @self: the config to free, or NULL.
 *
 * Frees a config and all instances it owns. Safe to call with NULL.
 */
void
mk_config_free (MkConfig *self)
{
  if (self == NULL)
    return;
  g_free (self->default_name);
  g_hash_table_destroy (self->instances);
  g_free (self);
}

/**
 * mk_config_default_path:
 *
 * Returns the default config path,
 * ${XDG_CONFIG_HOME:-~/.config}/mcp-kodi/config.json, computed once and cached.
 *
 * @return the path string, owned by the library; do not free.
 */
const char *
mk_config_default_path (void)
{
  static char *path = NULL;
  if (g_once_init_enter (&path))
    {
      /* g_get_user_config_dir() already honours XDG_CONFIG_HOME and falls
       * back to ~/.config. */
      char *p = g_build_filename (g_get_user_config_dir (),
                                  "mcp-kodi", "config.json", NULL);
      g_once_init_leave (&path, p);
    }
  return path;
}

/* ---- accessors ----------------------------------------------------------- */

/**
 * mk_config_get_default:
 * @self: the config.
 *
 * @return the name of the default instance, owned by @self (do not free), or
 *         NULL if none is set.
 */
const char *
mk_config_get_default (MkConfig *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->default_name;
}

/**
 * mk_config_set_default:
 * @self: the config.
 * @name: name of the instance to make default, or NULL to clear it.
 *
 * Sets the default instance name, duplicating @name. Does not check that an
 * instance of that name exists.
 */
void
mk_config_set_default (MkConfig *self, const char *name)
{
  g_return_if_fail (self != NULL);
  g_free (self->default_name);
  self->default_name = g_strdup (name);
}

/**
 * mk_config_get_instance:
 * @self: the config.
 * @name: instance name to look up, or NULL for the default instance.
 *
 * Looks up an instance by name. With @name NULL the configured default name is
 * used.
 *
 * @return the matching MkInstance owned by @self (do not free), or NULL if not
 *         found (or @name is NULL and no default is set).
 */
MkInstance *
mk_config_get_instance (MkConfig *self, const char *name)
{
  g_return_val_if_fail (self != NULL, NULL);
  if (name == NULL)
    name = self->default_name;
  if (name == NULL)
    return NULL;
  return g_hash_table_lookup (self->instances, name);
}

/**
 * mk_config_set_instance:
 * @self: the config.
 * @name: instance name (duplicated).
 * @inst: the instance to store; ownership is transferred to @self.
 *
 * Inserts or replaces the instance stored under @name. Any instance previously
 * stored under that name is freed.
 */
void
mk_config_set_instance (MkConfig *self, const char *name, MkInstance *inst)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (name != NULL);
  g_return_if_fail (inst != NULL);
  g_hash_table_insert (self->instances, g_strdup (name), inst);
}

/**
 * mk_config_remove_instance:
 * @self: the config.
 * @name: instance name to remove.
 *
 * Removes the instance stored under @name, freeing it. The default name (§7) is
 * left untouched; the caller is responsible for not leaving @self->default_name
 * pointing at a removed instance.
 *
 * @return TRUE if an instance was removed, FALSE if none was stored under @name.
 */
gboolean
mk_config_remove_instance (MkConfig *self, const char *name)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (name != NULL, FALSE);
  return g_hash_table_remove (self->instances, name);
}

/**
 * mk_config_instance_count:
 * @self: the config.
 *
 * @return the number of configured instances.
 */
guint
mk_config_instance_count (MkConfig *self)
{
  g_return_val_if_fail (self != NULL, 0);
  return g_hash_table_size (self->instances);
}

/**
 * mk_config_instance_names:
 * @self: the config.
 *
 * Lists the configured instance names in sorted order.
 *
 * @return a newly allocated GList of instance-name strings owned by @self; free
 *         the list with g_list_free() but do not free the strings.
 */
GList *
mk_config_instance_names (MkConfig *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  GList *keys = g_hash_table_get_keys (self->instances);
  return g_list_sort (keys, (GCompareFunc) g_strcmp0);
}

/* ---- parsing ------------------------------------------------------------- */

/**
 * instance_from_object:
 * @o: a JSON object holding name/host/auth/scheme/insecure members.
 *
 * Builds an instance from a JSON object. Missing members fall back to sane
 * defaults (scheme "https", insecure FALSE, allow_rpc FALSE, name/host/auth
 * NULL).
 *
 * @return a newly allocated MkInstance; free with mk_instance_free().
 */
static MkInstance *
instance_from_object (JsonObject *o)
{
  const char *name   = json_object_get_string_member_with_default (o, "name", NULL);
  const char *host   = json_object_get_string_member_with_default (o, "host", NULL);
  const char *auth   = json_object_get_string_member_with_default (o, "auth", NULL);
  const char *scheme = json_object_get_string_member_with_default (o, "scheme", "https");
  gboolean insecure  = json_object_get_boolean_member_with_default (o, "insecure", FALSE);
  gboolean allow_rpc = json_object_get_boolean_member_with_default (o, "allow_rpc", FALSE);
  return mk_instance_new (name, host, auth, scheme, insecure, allow_rpc);
}

/**
 * load_file:
 * @self: the config to populate.
 * @path: path of the JSON file to read.
 * @error: return location for a GError, or NULL.
 *
 * Parses a config file into @self. A file with an "instances" member is read as
 * the version-2 shape (named instances + optional "default"); any other object
 * is read as a version-1 flat file and stored as a single instance named
 * "default" (§7.6).
 *
 * @return TRUE on success; FALSE with @error set on parse or structural error.
 */
static gboolean
load_file (MkConfig *self, const char *path, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  GError *local = NULL;

  if (!json_parser_load_from_file (parser, path, &local))
    {
      g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_PARSE,
                   "failed to parse %s: %s", path, local->message);
      g_clear_error (&local);
      return FALSE;
    }

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_INVALID,
                   "%s: top-level value is not a JSON object", path);
      return FALSE;
    }

  JsonObject *obj = json_node_get_object (root);

  if (json_object_has_member (obj, "instances"))
    {
      /* version 2: a map of named instances + a default. */
      if (json_object_has_member (obj, "default"))
        mk_config_set_default (self,
                               json_object_get_string_member_with_default (obj, "default", NULL));

      JsonObject *insts = json_object_get_object_member (obj, "instances");
      if (insts == NULL)
        {
          g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_INVALID,
                       "%s: \"instances\" is not an object", path);
          return FALSE;
        }

      GList *members = json_object_get_members (insts);
      for (GList *l = members; l != NULL; l = l->next)
        {
          const char *name = l->data;
          JsonObject *io = json_object_get_object_member (insts, name);
          if (io == NULL)
            {
              g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_INVALID,
                           "%s: instance \"%s\" is not an object", path, name);
              g_list_free (members);
              return FALSE;
            }
          mk_config_set_instance (self, name, instance_from_object (io));
        }
      g_list_free (members);
    }
  else
    {
      /* version 1 (§7.6): a flat host/auth/… file → one instance "default". */
      mk_config_set_instance (self, "default", instance_from_object (obj));
      mk_config_set_default (self, "default");
    }

  return TRUE;
}

/* ---- environment overrides ----------------------------------------------- */

/**
 * curl_opts_insecure:
 * @opts: the value of KODI_CURL_OPTS, or NULL.
 *
 * Scans a curl-style options string for an insecure flag.
 *
 * @return TRUE if @opts contains a "-k" or "--insecure" token.
 */
static gboolean
curl_opts_insecure (const char *opts)
{
  if (opts == NULL)
    return FALSE;
  g_auto (GStrv) toks = g_strsplit_set (opts, " \t", -1);
  for (char **t = toks; *t != NULL; t++)
    if (g_strcmp0 (*t, "-k") == 0 || g_strcmp0 (*t, "--insecure") == 0)
      return TRUE;
  return FALSE;
}

/**
 * apply_env_overrides:
 * @self: the config to modify.
 *
 * Applies KODI_HOST/KODI_AUTH/KODI_SCHEME and the -k flag in KODI_CURL_OPTS to
 * the default instance only (§7.3). If no relevant variable is set this is a
 * no-op. With no config file loaded, this creates one implicit instance named
 * "default" and makes it the default.
 */
static void
apply_env_overrides (MkConfig *self)
{
  const char *host   = g_getenv ("KODI_HOST");
  const char *auth   = g_getenv ("KODI_AUTH");
  const char *scheme = g_getenv ("KODI_SCHEME");
  const char *opts   = g_getenv ("KODI_CURL_OPTS");
  gboolean    insecure = curl_opts_insecure (opts);

  if (host == NULL && auth == NULL && scheme == NULL && opts == NULL)
    return;

  const char *name = self->default_name ? self->default_name : "default";
  MkInstance *inst = g_hash_table_lookup (self->instances, name);
  if (inst == NULL)
    {
      inst = mk_instance_new (NULL, NULL, NULL, NULL, FALSE, FALSE);
      mk_config_set_instance (self, name, inst);
    }
  if (self->default_name == NULL)
    mk_config_set_default (self, name);

  if (host != NULL)
    {
      g_free (inst->host);
      inst->host = g_strdup (host);
    }
  if (auth != NULL)
    {
      g_free (inst->auth);
      inst->auth = g_strdup (auth);
    }
  if (scheme != NULL)
    {
      g_free (inst->scheme);
      inst->scheme = g_strdup (scheme);
    }
  if (insecure)
    inst->insecure = TRUE;
}

/* ---- load ---------------------------------------------------------------- */

/**
 * mk_config_load:
 * @path: config file path, or NULL to use mk_config_default_path().
 * @error: return location for a GError, or NULL.
 *
 * Loads configuration: reads @path if it exists, then applies environment
 * overrides to the default instance (§7.3). Resolves the default instance name,
 * choosing the sole instance when exactly one is configured and none was named.
 * Fails with MK_CONFIG_ERROR_NOT_CONFIGURED when neither a file nor environment
 * provides any instance.
 *
 * @return a newly allocated MkConfig (free with mk_config_free()), or NULL with
 *         @error set.
 */
MkConfig *
mk_config_load (const char *path, GError **error)
{
  const char *resolved = path ? path : mk_config_default_path ();
  g_autoptr (MkConfig) self = mk_config_new ();

  if (g_file_test (resolved, G_FILE_TEST_EXISTS)
      && !load_file (self, resolved, error))
    return NULL;

  apply_env_overrides (self);

  if (g_hash_table_size (self->instances) == 0)
    {
      g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_NOT_CONFIGURED,
                   "no Kodi configuration: create %s or set KODI_HOST"
                   " (and KODI_AUTH) in the environment", resolved);
      return NULL;
    }

  /* Ensure default names an existing instance. */
  if (self->default_name == NULL)
    {
      if (g_hash_table_size (self->instances) == 1)
        {
          GList *names = g_hash_table_get_keys (self->instances);
          mk_config_set_default (self, names->data);
          g_list_free (names);
        }
      else
        {
          g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_INVALID,
                       "%s: multiple instances but no \"default\" set", resolved);
          return NULL;
        }
    }
  else if (!g_hash_table_contains (self->instances, self->default_name))
    {
      g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_INVALID,
                   "%s: default instance \"%s\" is not defined",
                   resolved, self->default_name);
      return NULL;
    }

  return g_steal_pointer (&self);
}

/* ---- save ---------------------------------------------------------------- */

/**
 * serialise:
 * @self: the config to serialise.
 *
 * Renders the config as pretty-printed JSON in the version-2 shape, with
 * instances emitted in sorted key order, the "name" (display label) and "auth"
 * members each omitted when the instance has none. "allow_rpc" is emitted only
 * when set, so a hand-enabled escape hatch (§7.7) survives a save while a box
 * that never opted in stays clean.
 *
 * @return a newly allocated JSON string; free with g_free().
 */
static char *
serialise (MkConfig *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);

  json_builder_set_member_name (b, "version");
  json_builder_add_int_value (b, 2);

  if (self->default_name != NULL)
    {
      json_builder_set_member_name (b, "default");
      json_builder_add_string_value (b, self->default_name);
    }

  json_builder_set_member_name (b, "instances");
  json_builder_begin_object (b);
  GList *names = mk_config_instance_names (self);
  for (GList *l = names; l != NULL; l = l->next)
    {
      const char *name = l->data;
      MkInstance *inst = g_hash_table_lookup (self->instances, name);

      json_builder_set_member_name (b, name);
      json_builder_begin_object (b);

      if (inst->name != NULL)
        {
          json_builder_set_member_name (b, "name");
          json_builder_add_string_value (b, inst->name);
        }

      json_builder_set_member_name (b, "host");
      json_builder_add_string_value (b, inst->host);

      if (inst->auth != NULL)
        {
          json_builder_set_member_name (b, "auth");
          json_builder_add_string_value (b, inst->auth);
        }

      json_builder_set_member_name (b, "scheme");
      json_builder_add_string_value (b, inst->scheme ? inst->scheme : "https");

      json_builder_set_member_name (b, "insecure");
      json_builder_add_boolean_value (b, inst->insecure);

      if (inst->allow_rpc)
        {
          json_builder_set_member_name (b, "allow_rpc");
          json_builder_add_boolean_value (b, TRUE);
        }

      json_builder_end_object (b);
    }
  g_list_free (names);
  json_builder_end_object (b); /* instances */

  json_builder_end_object (b); /* root */

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);
  json_generator_set_indent (gen, 2);
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  return json_generator_to_data (gen, NULL);
}

/**
 * write_all:
 * @fd: an open, writable file descriptor.
 * @data: buffer to write.
 * @len: number of bytes to write from @data.
 * @error: return location for a GError, or NULL.
 *
 * Writes the whole buffer, retrying short writes and restarting on EINTR.
 *
 * @return TRUE if all @len bytes were written; FALSE with @error set on
 *         failure.
 */
static gboolean
write_all (int fd, const char *data, gsize len, GError **error)
{
  gsize off = 0;
  while (off < len)
    {
      gssize w = write (fd, data + off, len - off);
      if (w < 0)
        {
          if (errno == EINTR)
            continue;
          g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_IO,
                       "write failed: %s", g_strerror (errno));
          return FALSE;
        }
      off += (gsize) w;
    }
  return TRUE;
}

/**
 * mk_config_save:
 * @self: the config to write.
 * @path: destination path, or NULL to use mk_config_default_path().
 * @error: return location for a GError, or NULL.
 *
 * Writes @self to @path atomically (§7.4): creates the parent directory 0700,
 * writes a 0600 temp file in the same directory, fsync()s it, then rename()s it
 * over the target. Always written in the version-2 shape. The temp file is
 * removed if any step fails.
 *
 * @return TRUE on success; FALSE with @error set on I/O failure.
 */
gboolean
mk_config_save (MkConfig *self, const char *path, GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);

  const char *resolved = path ? path : mk_config_default_path ();
  g_autofree char *dir = g_path_get_dirname (resolved);

  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_IO,
                   "failed to create %s: %s", dir, g_strerror (errno));
      return FALSE;
    }

  g_autofree char *data = serialise (self);
  gsize len = strlen (data);

  /* Atomic write: temp file in the same dir (0600), fsync, rename over. */
  g_autofree char *tmpl = g_strconcat (resolved, ".XXXXXX", NULL);
  int fd = g_mkstemp_full (tmpl, O_WRONLY, 0600);
  if (fd == -1)
    {
      g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_IO,
                   "failed to create temp file near %s: %s",
                   resolved, g_strerror (errno));
      return FALSE;
    }

  if (!write_all (fd, data, len, error)
      || fsync (fd) != 0)
    {
      if (error != NULL && *error == NULL)
        g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_IO,
                     "fsync failed: %s", g_strerror (errno));
      close (fd);
      g_unlink (tmpl);
      return FALSE;
    }

  if (close (fd) != 0)
    {
      g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_IO,
                   "close failed: %s", g_strerror (errno));
      g_unlink (tmpl);
      return FALSE;
    }

  if (g_rename (tmpl, resolved) != 0)
    {
      g_set_error (error, MK_CONFIG_ERROR, MK_CONFIG_ERROR_IO,
                   "rename onto %s failed: %s", resolved, g_strerror (errno));
      g_unlink (tmpl);
      return FALSE;
    }

  return TRUE;
}
