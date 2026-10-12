/* mcp-kodi — the build ID of this server binary.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * The release version alone cannot tell builds apart (every build between two
 * releases reports the same version), so the build captures
 * `git describe --always --dirty` and appends it. A build from a tarball
 * without git reports the plain version.
 */

#ifndef MK_BUILD_ID_H
#define MK_BUILD_ID_H

#include <glib.h>

G_BEGIN_DECLS

const char *mk_build_id (void);

G_END_DECLS

#endif /* MK_BUILD_ID_H */
