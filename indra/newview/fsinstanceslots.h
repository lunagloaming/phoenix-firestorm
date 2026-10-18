/**
 * @file fsinstanceslots.h
 * @brief Coordinates several viewer instances that share one cache directory.
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

#ifndef FS_INSTANCESLOTS_H
#define FS_INSTANCESLOTS_H

#include "llapr.h"
#include "llframetimer.h"
#include "llsingleton.h"

// Each running viewer holds an exclusive lock on one "instance_<n>.lock" file in the
// cache directory it uses. Slot 0 owns the shared caches (texture cache, disk cache purge);
// higher slots use them read-only and keep per-slot caches where sharing is not possible
// (e.g. the object cache). Unlike the exec marker in the logs directory, this follows the
// cache directory actually in use, so instances with separate user directories but one
// CacheLocation still coordinate, and instances with separate CacheLocations each own theirs.
class FSInstanceSlots : public LLSingleton<FSInstanceSlots>
{
    LOG_CLASS(FSInstanceSlots);

    LLSINGLETON(FSInstanceSlots);
    ~FSInstanceSlots();

public:
    static constexpr S32 MAX_SLOTS = 8;

    // Takes the lowest free slot in cache_dir. Returns the slot, or -1 if none could be locked.
    S32 acquire(const std::string& cache_dir);

    S32 getSlot() const { return mSlot; }

    // True if this instance may write to the shared caches.
    bool ownsSharedCache() const { return mSlot == 0; }

    // Name for a per-slot cache directory, e.g. "objectcache" for slot 0 and "objectcache_1" for slot 1.
    std::string getSlotDirName(const std::string& base_name) const;

    // Running instances using this cache directory, including this one. Re-checked at most every 30 seconds.
    S32 getActiveInstanceCount();

private:
    std::string getSlotFileName(S32 slot) const;
    bool isSlotHeldByOther(S32 slot) const;

    std::string mCacheDir;
    S32 mSlot;
    LLAPRFile mLockFile;
    S32 mActiveInstanceCount;
    LLFrameTimer mActiveCountTimer;
};

#endif // FS_INSTANCESLOTS_H
