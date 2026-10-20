/**
 * @file fsrebuildtrace.h
 * @brief FSVBOAllocTrace: which spatial groups rebuild their geometry, and why.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026 Luna Witch
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef FS_REBUILDTRACE_H
#define FS_REBUILDTRACE_H

#include "stdtypes.h"

class LLDrawable;
class LLSpatialGroup;

// Debug instrumentation behind the FSVBOAllocTrace setting, logged once a minute under "RebuildTrace". Every volume
// group rebuild writes the whole group's geometry into new vertex buffers; this records which groups rebuild, what
// they hold (attachments, animesh, static objects, flexi prims...), and the call stacks that queued the rebuilds.
// Call stacks are Linux only; everything is a no-op while the setting is off.
namespace FSRebuildTrace
{
    // LLPipeline::markRebuild(group), when the group joins the build queue
    void markGroup(const LLSpatialGroup* group);

    // LLPipeline::markRebuild(drawable, flag), before the flags are applied
    void markDrawable(const LLDrawable* drawable, U32 flag);

    // LLSpatialPartition::move, before a drawable that no longer fits its group is removed and re-inserted, and
    // after: counts those moves, why they happened, and how many put the drawable back into the same group
    void moving(LLDrawable* drawable, LLSpatialGroup* from);
    void moved(LLDrawable* drawable);

    // LLVOVolume::setTEColor: a face's colour changed from one alpha class to another (LLFace::fsAlphaClass),
    // updated in place or by rebuilding the group
    void colourChange(S32 old_class, S32 new_class, bool in_place);

    // LLVolumeGeometryManager::rebuildGeom, once the group's new buffers are filled
    void rebuilt(LLSpatialGroup* group, U32 bytes);

    // Once a frame from LLAppViewer::idle()
    void idle();
}

#endif // FS_REBUILDTRACE_H
