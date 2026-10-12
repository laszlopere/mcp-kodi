/* mcp-kodi — unit tests for the file-backed config paths (mk-config).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Complements test-config.c (which covers only the in-memory model) by
 * exercising mk_config_load() / mk_config_save() and the environment-override
 * path, including the load (bad shape, unresolvable default) and save (unwritable
 * directory, rename failure) error paths. All filesystem I/O is confined to a fresh g_dir_make_tmp() directory
 * that is removed at the end of each case; the user's real config is never
 * touched, and load/save are always given an explicit path (never NULL).
 */

#define MK_TEST_IMPL
#include "mk-test.h"

#include "mk-config.h"

#include <glib/gstdio.h>
#include <unistd.h>

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

/* Write @json to @path verbatim, as a hand-edited config file would be. */
static void
write_cfg (const char *path, const char *json)
{
  g_autoptr (GError) err = NULL;
  MK_CHECK (g_file_set_contents (path, json, -1, &err));
  MK_CHECK (err == NULL);
}

/* Remove @dir and everything under it (one level of subdirectories deep is all
 * these cases create), restoring write permission first so a case that made a
 * directory read-only can still clean up. */
static void
rm_tree (const char *dir)
{
  g_chmod (dir, 0700);
  GDir *d = g_dir_open (dir, 0, NULL);
  if (d != NULL)
    {
      const char *name;
      while ((name = g_dir_read_name (d)) != NULL)
        {
          g_autofree char *f = g_build_filename (dir, name, NULL);
          if (g_file_test (f, G_FILE_TEST_IS_DIR))
            rm_tree (f);
          else
            g_unlink (f);
        }
      g_dir_close (d);
    }
  g_rmdir (dir);
}

/* Number of entries in @dir whose name starts with @prefix (e.g. the
 * "config.json." temp files mk_config_save() must not leave behind). */
static int
count_prefixed (const char *dir, const char *prefix)
{
  int n = 0;
  GDir *d = g_dir_open (dir, 0, NULL);
  if (d == NULL)
    return -1;
  const char *name;
  while ((name = g_dir_read_name (d)) != NULL)
    if (g_str_has_prefix (name, prefix))
      n++;
  g_dir_close (d);
  return n;
}

static void
case_env_connection_in_place (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);

  /* Two boxes, "beta" the default. */
  write_cfg (path,
             "{ \"default\": \"beta\", \"instances\": {"
             "  \"alpha\": { \"name\": \"Alpha\", \"host\": \"a:80\","
             "               \"auth\": \"a:a\", \"scheme\": \"http\" },"
             "  \"beta\":  { \"name\": \"Beta\", \"host\": \"b:80\","
             "               \"auth\": \"b:b\", \"scheme\": \"http\" } } }");

  /* Without KODI_HOST the target box is unchanged: auth/scheme/insecure are
   * applied in place to the default instance only, its identity (label, host)
   * kept, and no synthetic "env" instance appears. */
  g_setenv ("KODI_AUTH", "env-user:env-pass", TRUE);
  g_setenv ("KODI_SCHEME", "https", TRUE);
  g_setenv ("KODI_CURL_OPTS", "--insecure", TRUE);

  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got != NULL);
  MK_CHECK (err == NULL);
  MK_CHECK_INT_EQ (mk_config_instance_count (got), 2);
  MK_CHECK_STR_EQ (mk_config_get_default (got), "beta");
  MK_CHECK (mk_config_get_instance (got, MK_CONFIG_ENV_INSTANCE) == NULL);

  MkInstance *b = mk_config_get_instance (got, "beta");
  MK_CHECK (b != NULL);
  MK_CHECK_STR_EQ (b->name, "Beta");
  MK_CHECK_STR_EQ (b->host, "b:80");
  MK_CHECK_STR_EQ (b->auth, "env-user:env-pass");
  MK_CHECK_STR_EQ (b->scheme, "https");
  MK_CHECK (b->insecure); /* "--insecure" counts like "-k" */

  /* The non-default box is untouched. */
  MkInstance *a = mk_config_get_instance (got, "alpha");
  MK_CHECK (a != NULL);
  MK_CHECK_STR_EQ (a->auth, "a:a");
  MK_CHECK_STR_EQ (a->scheme, "http");
  MK_CHECK (!a->insecure);

  clear_kodi_env ();
  cleanup (path, dir);
}

static void
case_env_connection_no_file (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);

  /* No file and no KODI_HOST, only a connection override: there is no box to
   * apply it to, so an instance named "default" is created to hold it and
   * made the default. It has no host — env alone did not name one. */
  g_setenv ("KODI_AUTH", "only:auth", TRUE);

  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got != NULL);
  MK_CHECK (err == NULL);
  MK_CHECK_INT_EQ (mk_config_instance_count (got), 1);
  MK_CHECK_STR_EQ (mk_config_get_default (got), "default");

  MkInstance *inst = mk_config_get_instance (got, NULL);
  MK_CHECK (inst != NULL);
  MK_CHECK (inst->host == NULL);
  MK_CHECK_STR_EQ (inst->auth, "only:auth");
  MK_CHECK (!inst->insecure);

  clear_kodi_env ();
  cleanup (NULL, dir);
}

static void
case_env_curl_opts_not_insecure (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);

  write_cfg (path, "{ \"host\": \"h:80\", \"auth\": \"u:p\" }");

  /* Only a whole "-k"/"--insecure" token flips insecure on: other curl opts,
   * and tokens that merely contain "-k", leave it off — and leave the rest of
   * the instance as the file had it. */
  g_setenv ("KODI_CURL_OPTS", "--connect-timeout 5 -kx --keepalive", TRUE);

  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got != NULL);
  MK_CHECK (err == NULL);
  MkInstance *inst = mk_config_get_instance (got, NULL);
  MK_CHECK (inst != NULL);
  MK_CHECK (!inst->insecure);
  MK_CHECK_STR_EQ (inst->host, "h:80");
  MK_CHECK_STR_EQ (inst->auth, "u:p");

  clear_kodi_env ();
  cleanup (path, dir);
}

static void
case_load_v1_flat_file (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);

  /* The pre-instances flat shape loads as a single instance "default". */
  write_cfg (path, "{ \"host\": \"flat:8080\", \"scheme\": \"https\","
                   "  \"insecure\": true }");

  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got != NULL);
  MK_CHECK (err == NULL);
  MK_CHECK_INT_EQ (mk_config_instance_count (got), 1);
  MK_CHECK_STR_EQ (mk_config_get_default (got), "default");
  MkInstance *inst = mk_config_get_instance (got, NULL);
  MK_CHECK (inst != NULL);
  MK_CHECK_STR_EQ (inst->host, "flat:8080");
  MK_CHECK_STR_EQ (inst->scheme, "https");
  MK_CHECK (inst->insecure);

  cleanup (path, dir);
}

/* Load @json from a fresh temp file and assert it fails with @code. */
static void
expect_load_error (const char *json, int code)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);
  write_cfg (path, json);

  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got == NULL);
  MK_CHECK (err != NULL);
  MK_CHECK (g_error_matches (err, MK_CONFIG_ERROR, code));
  /* every message names the offending file */
  MK_CHECK (err != NULL && strstr (err->message, path) != NULL);

  cleanup (path, dir);
}

static void
case_load_wrong_shape (void)
{
  /* Valid JSON, but not an object at the top. */
  expect_load_error ("[ 1, 2, 3 ]", MK_CONFIG_ERROR_INVALID);
  expect_load_error ("\"just a string\"", MK_CONFIG_ERROR_INVALID);
  /* "instances" present but null, and an instance that is not an object. */
  expect_load_error ("{ \"instances\": null }", MK_CONFIG_ERROR_INVALID);
  expect_load_error ("{ \"instances\": { \"a\": null } }",
                     MK_CONFIG_ERROR_INVALID);
  /* Non-object, non-null values: an INVALID error, not a json-glib CRITICAL
   * (make check runs with G_DEBUG=fatal-criticals, so one would abort). */
  expect_load_error ("{ \"instances\": 5 }", MK_CONFIG_ERROR_INVALID);
  expect_load_error ("{ \"instances\": { \"a\": 5 } }",
                     MK_CONFIG_ERROR_INVALID);
}

static void
case_load_default_unresolvable (void)
{
  /* "default" naming an instance the file does not define. */
  expect_load_error ("{ \"default\": \"nope\", \"instances\": {"
                     "  \"a\": { \"host\": \"a:80\" } } }",
                     MK_CONFIG_ERROR_INVALID);
  /* Several instances and no "default" to choose between them. */
  expect_load_error ("{ \"instances\": {"
                     "  \"a\": { \"host\": \"a:80\" },"
                     "  \"b\": { \"host\": \"b:80\" } } }",
                     MK_CONFIG_ERROR_INVALID);
}

static void
case_load_sole_instance_is_default (void)
{
  clear_kodi_env ();

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);

  /* Exactly one instance and no "default": that one is chosen. */
  write_cfg (path, "{ \"instances\": { \"only\": { \"host\": \"o:80\" } } }");

  g_autoptr (MkConfig) got = mk_config_load (path, &err);
  MK_CHECK (got != NULL);
  MK_CHECK (err == NULL);
  MK_CHECK_STR_EQ (mk_config_get_default (got), "only");

  cleanup (path, dir);
}

/* A config with one instance, for the save-failure cases. */
static MkConfig *
one_instance_config (void)
{
  MkConfig *cfg = mk_config_new ();
  mk_config_set_instance (cfg, "a",
                          mk_instance_new ("A", "a:80", NULL, "http",
                                           FALSE, FALSE));
  mk_config_set_default (cfg, "a");
  return cfg;
}

static void
case_save_unwritable_dir (void)
{
  /* Root ignores directory permissions, so these failures cannot be provoked;
   * record a passing check instead of a false failure. */
  if (geteuid () == 0)
    {
      MK_CHECK (TRUE);
      return;
    }

  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  MK_CHECK (g_chmod (dir, 0500) == 0);
  g_autoptr (MkConfig) cfg = one_instance_config ();

  /* The parent directory cannot be created inside a read-only one. */
  g_autofree char *deep = g_build_filename (dir, "sub", "config.json", NULL);
  MK_CHECK (!mk_config_save (cfg, deep, &err));
  MK_CHECK (g_error_matches (err, MK_CONFIG_ERROR, MK_CONFIG_ERROR_IO));
  MK_CHECK (err != NULL && strstr (err->message, "failed to create") != NULL);
  g_clear_error (&err);

  /* The directory exists but the temp file cannot be created in it. */
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);
  MK_CHECK (!mk_config_save (cfg, path, &err));
  MK_CHECK (g_error_matches (err, MK_CONFIG_ERROR, MK_CONFIG_ERROR_IO));
  MK_CHECK (err != NULL && strstr (err->message, "temp file") != NULL);
  MK_CHECK (!g_file_test (path, G_FILE_TEST_EXISTS));

  rm_tree (dir);
}

static void
case_save_rename_onto_dir (void)
{
  g_autoptr (GError) err = NULL;
  g_autofree char *dir = g_dir_make_tmp ("mk-config-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);

  /* The target path is an existing (non-empty) directory, so the final
   * rename() fails; the save must report it and remove its temp file. */
  g_autofree char *path = g_build_filename (dir, "config.json", NULL);
  MK_CHECK (g_mkdir (path, 0700) == 0);
  g_autofree char *inside = g_build_filename (path, "keep", NULL);
  write_cfg (inside, "x");

  g_autoptr (MkConfig) cfg = one_instance_config ();
  MK_CHECK (!mk_config_save (cfg, path, &err));
  MK_CHECK (g_error_matches (err, MK_CONFIG_ERROR, MK_CONFIG_ERROR_IO));
  MK_CHECK (err != NULL && strstr (err->message, "rename") != NULL);
  MK_CHECK_INT_EQ (count_prefixed (dir, "config.json."), 0);
  MK_CHECK (g_file_test (inside, G_FILE_TEST_EXISTS)); /* left intact */

  rm_tree (dir);
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
    { "env-connection-in-place",        case_env_connection_in_place },
    { "env-connection-no-file",         case_env_connection_no_file },
    { "env-curl-opts-not-insecure",     case_env_curl_opts_not_insecure },
    { "load-v1-flat-file",              case_load_v1_flat_file },
    { "load-wrong-shape",               case_load_wrong_shape },
    { "load-default-unresolvable",      case_load_default_unresolvable },
    { "load-sole-instance-is-default",  case_load_sole_instance_is_default },
    { "save-unwritable-dir",            case_save_unwritable_dir },
    { "save-rename-onto-dir",           case_save_rename_onto_dir },
  };
  return mk_test_run (argc, argv, cases, G_N_ELEMENTS (cases));
}
