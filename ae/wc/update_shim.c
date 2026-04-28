/*
 * Copyright 2026 Paul Hammant (portions).
 * Portions copyright Apache Subversion project contributors (2001-2026).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

/* ae/wc/update_shim.c — retired in Round 152.
 *
 * The svnae_rtree_* + rtree_* forwards onto svnae_rt_* lived here
 * because rtree storage lived in ae/subr/rtree/shim.c and the
 * public ABI needed a different name. Round 152 hoisted the
 * storage to ae/subr/rtree/rtree.ae which exports both name
 * flavours directly, eliminating the forward layer.
 *
 * (svnae_update_set_wc_root + getter retired earlier in Round
 * 140; update_sha1_of_file in Round 130.)
 */
