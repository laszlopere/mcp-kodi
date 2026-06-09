/* mcp-kodi — Kodi JSON-RPC client over libsoup.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * One client per process. Each call is made against a named config instance
 * (§7), resolved to scheme/host/auth/insecure, and POSTed as JSON-RPC 2.0 to
 * <scheme>://<host>/jsonrpc. See ../TODO.md §4.
 */

#ifndef MK_KODI_H
#define MK_KODI_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "mk-config.h"

G_BEGIN_DECLS

#define MK_KODI_ERROR (mk_kodi_error_quark ())
GQuark mk_kodi_error_quark (void);

typedef enum
{
  MK_KODI_ERROR_NO_INSTANCE, /* named instance not found / lacks a host */
  MK_KODI_ERROR_TRANSPORT,   /* connection / send failure */
  MK_KODI_ERROR_HTTP,        /* non-2xx HTTP status (e.g. 401) */
  MK_KODI_ERROR_PROTOCOL,    /* response is not a valid JSON-RPC reply */
  MK_KODI_ERROR_RPC,         /* Kodi returned an "error" member */
} MkKodiError;

typedef struct _MkKodi MkKodi;

/* Borrows @config (does not take ownership); it must outlive the client. */
MkKodi *mk_kodi_new  (MkConfig *config);
void    mk_kodi_free (MkKodi *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MkKodi, mk_kodi_free)

/* Make a JSON-RPC call. @instance NULL → config default. @params may be NULL
 * (borrowed, not freed). On success returns the response "result" as a newly
 * allocated JsonNode (free with json_node_unref()); on failure returns NULL
 * with @error set — an RPC error carries Kodi's code and message. */
JsonNode *mk_kodi_call (MkKodi      *self,
                        const char  *instance,
                        const char  *method,
                        JsonNode    *params,
                        GError     **error);

G_END_DECLS

#endif /* MK_KODI_H */
