/* mcp-kodi — configuration loader/saver.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Loads the server's multi-instance config from
 * ${XDG_CONFIG_HOME:-~/.config}/mcp-kodi/config.json, applies environment
 * overrides to the default instance, and writes the effective config back
 * atomically. See ../TODO.md §7.
 */

#ifndef MK_CONFIG_H
#define MK_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

/* One configured Kodi box. name is a human-readable display label (e.g.
 * "Living Room TV"), distinct from the instance's config key that tools
 * reference; NULL when unset. host is "host[:port]"; auth is "user:pass" for
 * HTTP Basic (or NULL); scheme is "http"/"https"; insecure accepts a
 * self-signed cert (curl -k). allow_rpc opts this box into the generic `rpc`
 * escape hatch (§7.7, §11.6.6): off by default, set only by hand-editing the
 * config file — never written by the `instances` tool. */
typedef struct _MkInstance MkInstance;
struct _MkInstance
{
  char     *name;
  char     *host;
  char     *auth;
  char     *scheme;
  gboolean  insecure;
  gboolean  allow_rpc;
};

MkInstance *mk_instance_new  (const char *name,
                              const char *host,
                              const char *auth,
                              const char *scheme,
                              gboolean    insecure,
                              gboolean    allow_rpc);
MkInstance *mk_instance_copy (const MkInstance *inst);
void        mk_instance_free (MkInstance *inst);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MkInstance, mk_instance_free)

/* The config object: a set of named instances plus the chosen default. */
typedef struct _MkConfig MkConfig;

#define MK_CONFIG_ERROR (mk_config_error_quark ())
GQuark mk_config_error_quark (void);

typedef enum
{
  MK_CONFIG_ERROR_NOT_CONFIGURED, /* no config file and no env → nothing to use */
  MK_CONFIG_ERROR_PARSE,          /* file is not valid JSON */
  MK_CONFIG_ERROR_INVALID,        /* JSON is valid but structurally wrong */
  MK_CONFIG_ERROR_IO,             /* read/write/rename failure */
} MkConfigError;

/* Path of the default config file (cached, owned by the library). */
const char *mk_config_default_path (void);

MkConfig *mk_config_new  (void);
void      mk_config_free (MkConfig *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MkConfig, mk_config_free)

/* Load config: read PATH (NULL → mk_config_default_path()) if it exists, then
 * apply KODI_HOST/KODI_AUTH/KODI_SCHEME and -k in KODI_CURL_OPTS to the default
 * instance. With no file and no env this fails with NOT_CONFIGURED. Returns a
 * new MkConfig or NULL with ERROR set. */
MkConfig *mk_config_load (const char *path, GError **error);

/* Write the effective config to PATH (NULL → default) atomically: dir created
 * 0700, temp file 0600, fsync, rename over target. Always written as version 2. */
gboolean mk_config_save (MkConfig *self, const char *path, GError **error);

/* Accessors. mk_config_get_instance(self, NULL) returns the default instance. */
const char  *mk_config_get_default    (MkConfig *self);
void         mk_config_set_default    (MkConfig *self, const char *name);
MkInstance  *mk_config_get_instance   (MkConfig *self, const char *name);
/* Takes ownership of INST, replacing any existing instance of that name. */
void         mk_config_set_instance   (MkConfig *self, const char *name, MkInstance *inst);
/* Remove the instance stored under NAME (the default name, if any, is left
 * untouched). Returns TRUE if an instance was removed, FALSE if none existed. */
gboolean     mk_config_remove_instance (MkConfig *self, const char *name);
guint        mk_config_instance_count (MkConfig *self);
/* Sorted list of instance names; free the list (not the strings) with
 * g_list_free(). */
GList       *mk_config_instance_names (MkConfig *self);

G_END_DECLS

#endif /* MK_CONFIG_H */
