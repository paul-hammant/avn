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

/* ae/subr/rtree/rtree.h — opaque struct + lifecycle decls. Round
 * 152 hoisted storage + add ops + read accessors to
 * ae/subr/rtree/rtree.ae; the only thing that consumes this
 * header now is the C-side opaque holder in shim.c. */

#ifndef AE_SUBR_RTREE_H
#define AE_SUBR_RTREE_H

struct svnae_rt;
struct svnae_rt *svnae_rtree_new(void);
void             svnae_rtree_free(struct svnae_rt *rt);

#endif /* AE_SUBR_RTREE_H */
