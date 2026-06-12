/* mcp-kodi — unit tests for the file-backed config paths (mk-config).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Complements test-config.c (which covers only the in-memory model) by
 * exercising mk_config_load() / mk_config_save() and the environment-override
 * path. All filesystem I/O is confined to a fresh g_dir_make_tmp() directory
 * that is removed at the end of each case; the user's real config is never
 * touched, and load/save are always given an explicit path (never NULL).
 */

#define MK_TEST_IMPL
#include "mk-test.h"

#include "mk-config.h"

#include <glib/gstdio.h>

/* Clear the four env vars mk_config_load() consults, so a case starts from a
 * known state regardless of what the surrounding shell exported. */
static void
clear_kodi_env (void)
{
  g_unsetenv ("KODI_HOST");
  g_unsetenv ("KODI_AUTH");
  g_unsetenv ("KODI_SCHEME");
  g_unsetenv ("KODI_CURL_OPTS");
}

/* Remove a config file (if present) and its enclosing temp directory. */
static void
cleanup (const char *path, const char *dir)
{
  if (path != NULL)
    g_remove (path);
  if (dir != NULL)
    g_rmdir (dir);
}

static void
case_save_load_roundtrip (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);

  /* Build a config with two instances and a chosen default; exercise the
   * optional members (auth omitted on one, allow_rpc set on the other). */
  g_autoptr (MkConfig) cfg = mk_config_new ();
  mk_config_set_instance (cfg, "alpha",
                          mk_instance_new ("Alpha", "10.0.0.1:8080",
                                           "kodi:secret", "https",
                                           FALSE, FALSE));
  mk_config_set_instance (cfg, "beta",
                          mk_instance_new ("Beta", "10.0.0.2:9090",
                                           NULL, "http", TRUE, TRUE));
  mk_config_set_default (cfg, "beta");

  MK_CHECK (mk_config_save (cfg, path, &err));
  MK_CHECK (err == NULL);
  MK_CHECK (g_file_test (path, G_FILE_TEST_EXISTS));

  /* Reload and assert every field survived the round-trip. */
  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got != NULL);
  MK_CHECK (err == NULL);
  MK_CHECK_INT_EQ (mk_config_instance_count (got), 2);
  MK_CHECK_STR_EQ (mk_config_get_default (got), "beta");

  MkInstance *a = mk_config_get_instance (got, "alpha");
  MK_CHECK (a != NULL);
  MK_CHECK_STR_EQ (a->name, "Alpha");
  MK_CHECK_STR_EQ (a->host, "10.0.0.1:8080");
  MK_CHECK_STR_EQ (a->auth, "kodi:secret");
  MK_CHECK_STR_EQ (a->scheme, "https");
  MK_CHECK (!a->insecure);
  MK_CHECK (!a->allow_rpc);

  /* beta: NULL auth stays NULL, http scheme, insecure + allow_rpc preserved. */
  MkInstance *b = mk_config_get_instance (got, "beta");
  MK_CHECK (b != NULL);
  MK_CHECK_STR_EQ (b->name, "Beta");
  MK_CHECK_STR_EQ (b->host, "10.0.0.2:9090");
  MK_CHECK (b->auth == NULL);
  MK_CHECK_STR_EQ (b->scheme, "http");
  MK_CHECK (b->insecure);
  MK_CHECK (b->allow_rpc);

  /* get_instance(NULL) resolves to the default (beta). */
  MK_CHECK_STR_EQ (mk_config_get_instance (got, NULL)->name, "Beta");

  cleanup (path, dir);
}

static void
case_load_missing_is_empty (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  /* A path that we deliberately never create. */
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);
  MK_CHECK (!g_file_test (path, G_FILE_TEST_EXISTS));

  /* No file and no env is not an error: the server must still boot so the
   * assistant can register the MCP endpoint before any config.json exists.
   * load() returns a valid empty config with no default instance. */
  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got != NULL);
  MK_CHECK (err == NULL);
  MK_CHECK (mk_config_instance_count (got) == 0);
  MK_CHECK (mk_config_get_default (got) == NULL);
  MK_CHECK (mk_config_get_instance (got, NULL) == NULL);

  cleanup (NULL, dir);
}

static void
case_load_invalid_json (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);

  /* Not valid JSON → PARSE error from the parser. */
  MK_CHECK (g_file_set_contents (path, "{ this is not json", -1, &err));
  MK_CHECK (err == NULL);

  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got == NULL);
  MK_CHECK (err != NULL);
  MK_CHECK (g_error_matches (err, MK_CONFIG_ERROR, MK_CONFIG_ERROR_PARSE));

  cleanup (path, dir);
}

static void
case_env_overrides_no_file (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  /* No file on disk: the config must come entirely from the environment. */
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);

  g_setenv ("KODI_HOST", "192.168.1.50:8080", TRUE);
  g_setenv ("KODI_AUTH", "env-user:env-pass", TRUE);
  g_setenv ("KODI_SCHEME", "https", TRUE);
  g_setenv ("KODI_CURL_OPTS", "--connect-timeout 5 -k", TRUE);

  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got != NULL);
  MK_CHECK (err == NULL);
  /* One synthetic, unlabelled instance keyed "env", made the default. */
  MK_CHECK_INT_EQ (mk_config_instance_count (got), 1);
  MK_CHECK_STR_EQ (mk_config_get_default (got), MK_CONFIG_ENV_INSTANCE);

  MkInstance *inst = mk_config_get_instance (got, NULL);
  MK_CHECK (inst != NULL);
  MK_CHECK (inst->name == NULL); /* no display label: not a named box */
  MK_CHECK_STR_EQ (inst->host, "192.168.1.50:8080");
  MK_CHECK_STR_EQ (inst->auth, "env-user:env-pass");
  MK_CHECK_STR_EQ (inst->scheme, "https");
  /* "-k" among the curl opts flips insecure on. */
  MK_CHECK (inst->insecure);

  /* Don't leak the environment into other cases. */
  clear_kodi_env ();
  cleanup (NULL, dir);
}

static void
case_env_overrides_default_instance (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);

  /* Save a config whose default instance has on-disk values ... */
  g_autoptr (MkConfig) cfg = mk_config_new ();
  mk_config_set_instance (cfg, "home",
                          mk_instance_new ("Home", "file-host:1000",
                                           "file-user:file-pass", "http",
                                           FALSE, FALSE));
  mk_config_set_default (cfg, "home");
  MK_CHECK (mk_config_save (cfg, path, &err));
  MK_CHECK (err == NULL);

  /* ... then load with KODI_HOST set: it redirects to a box "home" does not
   * name, so the configured instance is superseded by a synthetic, unlabelled
   * "env" default. The host comes from the env; auth/scheme (no env of their
   * own) fall back to the former default's file values so a sibling-box
   * redirect keeps working — but the "Home" label is NOT borrowed. */
  g_setenv ("KODI_HOST", "override-host:2000", TRUE);

  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got != NULL);
  MK_CHECK (err == NULL);

  /* The configured instance is gone; only the env target remains. */
  MK_CHECK_INT_EQ (mk_config_instance_count (got), 1);
  MK_CHECK (mk_config_get_instance (got, "home") == NULL);
  MK_CHECK_STR_EQ (mk_config_get_default (got), MK_CONFIG_ENV_INSTANCE);

  MkInstance *inst = mk_config_get_instance (got, NULL);
  MK_CHECK (inst != NULL);
  MK_CHECK (inst->name == NULL); /* the "Home" label is not borrowed */
  MK_CHECK_STR_EQ (inst->host, "override-host:2000");
  MK_CHECK_STR_EQ (inst->auth, "file-user:file-pass");
  MK_CHECK_STR_EQ (inst->scheme, "http");

  clear_kodi_env ();
  cleanup (path, dir);
}

int
main (int argc, char **argv)
{
  static const MkTestCase cases[] = {
    { "save-load-roundtrip",            case_save_load_roundtrip },
    { "load-missing-is-empty",          case_load_missing_is_empty },
    { "load-invalid-json",              case_load_invalid_json },
    { "env-overrides-no-file",          case_env_overrides_no_file },
    { "env-overrides-default-instance", case_env_overrides_default_instance },
  };
  return mk_test_run (argc, argv, cases, G_N_ELEMENTS (cases));
}
