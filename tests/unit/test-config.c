/* mcp-kodi — unit tests for the in-memory config model (mk-config).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Covers the instance value type and the MkConfig container: construction,
 * copying, set/get/remove, the default selector and the sorted name list. The
 * file-backed paths (mk_config_load / mk_config_save) are deliberately NOT
 * called, so the tests touch no filesystem — they exercise only the pure
 * in-memory logic.
 */

#define MK_TEST_IMPL
#include "mk-test.h"

#include "mk-config.h"

static void
case_instance_defaults_scheme (void)
{
  /* a NULL scheme defaults to "https" (see mk_instance_new) */
  g_autoptr (MkInstance) inst =
    mk_instance_new ("Living Room", "10.0.0.5:8080", "kodi:secret", NULL,
                     FALSE, FALSE);
  MK_CHECK_STR_EQ (inst->name, "Living Room");
  MK_CHECK_STR_EQ (inst->host, "10.0.0.5:8080");
  MK_CHECK_STR_EQ (inst->scheme, "https");
  MK_CHECK (!inst->insecure);
  MK_CHECK (!inst->allow_rpc);
}

static void
case_instance_copy_is_deep (void)
{
  g_autoptr (MkInstance) a =
    mk_instance_new ("box", "host:9090", "u:p", "http", TRUE, TRUE);
  g_autoptr (MkInstance) b = mk_instance_copy (a);

  /* same values ... */
  MK_CHECK_STR_EQ (b->name, "box");
  MK_CHECK_STR_EQ (b->host, "host:9090");
  MK_CHECK_STR_EQ (b->auth, "u:p");
  MK_CHECK_STR_EQ (b->scheme, "http");
  MK_CHECK (b->insecure);
  MK_CHECK (b->allow_rpc);
  /* ... but distinct storage (a deep copy, not an alias) */
  MK_CHECK (a->name != b->name);
  MK_CHECK (a->host != b->host);

  /* copying NULL yields NULL */
  MK_CHECK (mk_instance_copy (NULL) == NULL);
}

static void
case_set_get_count (void)
{
  g_autoptr (MkConfig) cfg = mk_config_new ();
  MK_CHECK_INT_EQ (mk_config_instance_count (cfg), 0);

  mk_config_set_instance (cfg, "alpha",
                          mk_instance_new ("Alpha", "a:80", NULL, "http",
                                           FALSE, FALSE));
  mk_config_set_instance (cfg, "beta",
                          mk_instance_new ("Beta", "b:80", NULL, "http",
                                           FALSE, FALSE));
  MK_CHECK_INT_EQ (mk_config_instance_count (cfg), 2);

  MkInstance *got = mk_config_get_instance (cfg, "beta");
  MK_CHECK (got != NULL);
  MK_CHECK_STR_EQ (got->name, "Beta");

  /* an unknown key resolves to nothing */
  MK_CHECK (mk_config_get_instance (cfg, "missing") == NULL);
}

static void
case_set_replaces_same_key (void)
{
  g_autoptr (MkConfig) cfg = mk_config_new ();
  mk_config_set_instance (cfg, "k",
                          mk_instance_new ("First", "h1:80", NULL, "http",
                                           FALSE, FALSE));
  mk_config_set_instance (cfg, "k",
                          mk_instance_new ("Second", "h2:80", NULL, "http",
                                           FALSE, FALSE));
  /* still one instance; the second set replaced the first */
  MK_CHECK_INT_EQ (mk_config_instance_count (cfg), 1);
  MK_CHECK_STR_EQ (mk_config_get_instance (cfg, "k")->name, "Second");
}

static void
case_default_selector (void)
{
  g_autoptr (MkConfig) cfg = mk_config_new ();
  mk_config_set_instance (cfg, "home",
                          mk_instance_new ("Home", "h:80", NULL, "http",
                                           FALSE, FALSE));
  mk_config_set_default (cfg, "home");
  MK_CHECK_STR_EQ (mk_config_get_default (cfg), "home");

  /* get_instance(NULL) resolves to the default instance */
  MkInstance *def = mk_config_get_instance (cfg, NULL);
  MK_CHECK (def != NULL);
  MK_CHECK_STR_EQ (def->name, "Home");
}

static void
case_remove (void)
{
  g_autoptr (MkConfig) cfg = mk_config_new ();
  mk_config_set_instance (cfg, "gone",
                          mk_instance_new ("Gone", "g:80", NULL, "http",
                                           FALSE, FALSE));
  MK_CHECK (mk_config_remove_instance (cfg, "gone"));
  MK_CHECK_INT_EQ (mk_config_instance_count (cfg), 0);
  MK_CHECK (mk_config_get_instance (cfg, "gone") == NULL);
  /* removing a key that is not present is a no-op returning FALSE */
  MK_CHECK (!mk_config_remove_instance (cfg, "gone"));
}

static void
case_names_are_sorted (void)
{
  g_autoptr (MkConfig) cfg = mk_config_new ();
  mk_config_set_instance (cfg, "charlie",
                          mk_instance_new (NULL, "c:80", NULL, "http",
                                           FALSE, FALSE));
  mk_config_set_instance (cfg, "alpha",
                          mk_instance_new (NULL, "a:80", NULL, "http",
                                           FALSE, FALSE));
  mk_config_set_instance (cfg, "bravo",
                          mk_instance_new (NULL, "b:80", NULL, "http",
                                           FALSE, FALSE));

  GList *names = mk_config_instance_names (cfg);
  MK_CHECK_INT_EQ (g_list_length (names), 3);
  MK_CHECK_STR_EQ ((const char *) g_list_nth_data (names, 0), "alpha");
  MK_CHECK_STR_EQ ((const char *) g_list_nth_data (names, 1), "bravo");
  MK_CHECK_STR_EQ ((const char *) g_list_nth_data (names, 2), "charlie");
  g_list_free (names);
}

int
main (int argc, char **argv)
{
  static const MkTestCase cases[] = {
    { "instance-defaults-scheme",  case_instance_defaults_scheme },
    { "instance-copy-is-deep",     case_instance_copy_is_deep },
    { "set-get-count",             case_set_get_count },
    { "set-replaces-same-key",     case_set_replaces_same_key },
    { "default-selector",          case_default_selector },
    { "remove",                    case_remove },
    { "names-are-sorted",          case_names_are_sorted },
  };
  return mk_test_run (argc, argv, cases, G_N_ELEMENTS (cases));
}
