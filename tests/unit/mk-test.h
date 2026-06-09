/* mcp-kodi — a tiny header-only unit-test harness.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Deliberately small: each test binary is one .c file that lists its cases in a
 * table of MkTestCase and hands them to mk_test_run() from main(). A "case" is a
 * named function; inside it, MK_CHECK()/MK_CHECK_STR_EQ()/MK_CHECK_INT_EQ()
 * record individual checks.
 *
 * Output contract (the project's test convention):
 *   - default (no --verbose): one tabular row per case — case name, number of
 *     checks executed, number of checks failed — then a one-line summary.
 *   - --verbose: additionally print a PASS/FAIL line per individual check.
 *
 * mk_test_run() returns the number of *failed cases*, which main() returns as
 * the process exit status (0 = all passed). The build/run script keys off that
 * exit status to decide whether a test binary passed.
 *
 * Header-only: exactly one translation unit must define MK_TEST_IMPL before
 * including this header. Since every test binary is a single self-contained .c,
 * each simply does `#define MK_TEST_IMPL` then `#include "mk-test.h"`.
 */

#ifndef MK_TEST_H
#define MK_TEST_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

typedef struct
{
  const char *name;   /* shown in the result table */
  void (*fn) (void);  /* the case body; uses MK_CHECK* */
} MkTestCase;

/* Per-case counters and the verbose flag; defined in the MK_TEST_IMPL TU. */
extern int mk_test_verbose;
extern int mk_test_case_checks;
extern int mk_test_case_fails;

/* Record one check. OK != 0 → pass. EXPR/FILE/LINE describe it for --verbose. */
void mk_test_record (int ok, const char *expr, const char *file, int line);

/* Run all CASES (N of them), parsing --verbose from ARGV. Returns the number of
 * cases that had at least one failed check. */
int mk_test_run (int argc, char **argv, const MkTestCase *cases, size_t n);

#define MK_CHECK(expr) \
  mk_test_record ((expr) ? 1 : 0, #expr, __FILE__, __LINE__)

#define MK_CHECK_INT_EQ(a, b)                                                 \
  G_STMT_START                                                                \
  {                                                                           \
    gint64 _a = (gint64) (a), _b = (gint64) (b);                             \
    mk_test_record (_a == _b, #a " == " #b, __FILE__, __LINE__);              \
    if (_a != _b && mk_test_verbose)                                          \
      printf ("        expected %" G_GINT64_FORMAT                            \
              ", got %" G_GINT64_FORMAT "\n", _b, _a);                       \
  }                                                                           \
  G_STMT_END

#define MK_CHECK_STR_EQ(a, b)                                                 \
  G_STMT_START                                                                \
  {                                                                           \
    const char *_a = (a), *_b = (b);                                         \
    int _ok = (_a != NULL && _b != NULL && strcmp (_a, _b) == 0);            \
    mk_test_record (_ok, #a " == " #b, __FILE__, __LINE__);                   \
    if (!_ok && mk_test_verbose)                                              \
      printf ("        expected \"%s\", got \"%s\"\n",                        \
              _b ? _b : "(null)", _a ? _a : "(null)");                       \
  }                                                                           \
  G_STMT_END

#ifdef MK_TEST_IMPL

int mk_test_verbose = 0;
int mk_test_case_checks = 0;
int mk_test_case_fails = 0;

void
mk_test_record (int ok, const char *expr, const char *file, int line)
{
  mk_test_case_checks++;
  if (!ok)
    mk_test_case_fails++;
  if (mk_test_verbose)
    printf ("    [%s] %s:%d: %s\n", ok ? "PASS" : "FAIL", file, line, expr);
}

int
mk_test_run (int argc, char **argv, const MkTestCase *cases, size_t n)
{
  for (int i = 1; i < argc; i++)
    if (strcmp (argv[i], "--verbose") == 0 || strcmp (argv[i], "-v") == 0)
      mk_test_verbose = 1;

  g_autofree char *prog = g_path_get_basename (argv[0]);

  int total_checks = 0, total_fails = 0, failed_cases = 0;

  printf ("%-36s %8s %8s\n", prog, "CHECKS", "FAILED");

  for (size_t i = 0; i < n; i++)
    {
      mk_test_case_checks = 0;
      mk_test_case_fails = 0;

      if (mk_test_verbose)
        printf ("  == %s ==\n", cases[i].name);

      cases[i].fn ();

      printf ("%-36s %8d %8d\n", cases[i].name,
              mk_test_case_checks, mk_test_case_fails);

      total_checks += mk_test_case_checks;
      total_fails += mk_test_case_fails;
      if (mk_test_case_fails > 0)
        failed_cases++;
    }

  printf ("%-36s %8d %8d\n", "(total)", total_checks, total_fails);

  return failed_cases;
}

#endif /* MK_TEST_IMPL */

#endif /* MK_TEST_H */
