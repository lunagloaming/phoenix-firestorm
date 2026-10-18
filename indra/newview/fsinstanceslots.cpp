/**
 * @file fsinstanceslots.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsinstanceslots.h"

#include "lldir.h"
#include "llfile.h"

constexpr F32 ACTIVE_COUNT_RECHECK_SECS = 30.f;

FSInstanceSlots::FSInstanceSlots()
:   mSlot(-1),
    mActiveInstanceCount(1)
{
}

FSInstanceSlots::~FSInstanceSlots()
{
    // Closing the file releases the lock; the file itself is left for the next instance to reuse.
    mLockFile.close();
}

std::string FSInstanceSlots::getSlotFileName(S32 slot) const
{
    return gDirUtilp->add(mCacheDir, llformat("instance_%d.lock", slot));
}

S32 FSInstanceSlots::acquire(const std::string& cache_dir)
{
    if (mSlot >= 0)
    {
        return mSlot;
    }

    mCacheDir = cache_dir;
    LLFile::mkdir(mCacheDir);

    for (S32 slot = 0; slot < MAX_SLOTS; ++slot)
    {
        // Append mode: never truncate a file another instance may hold.
        if (mLockFile.open(getSlotFileName(slot), LL_APR_AB) != APR_SUCCESS || !mLockFile.getFileHandle())
        {
            mLockFile.close();
            continue;
        }
        if (apr_file_lock(mLockFile.getFileHandle(), APR_FLOCK_NONBLOCK | APR_FLOCK_EXCLUSIVE) == APR_SUCCESS)
        {
            mSlot = slot;
            break;
        }
        mLockFile.close();
    }

    mActiveInstanceCount = 0;
    mActiveCountTimer.reset();
    mActiveInstanceCount = getActiveInstanceCount();

    LL_INFOS() << "Cache instance slot " << mSlot << " in " << mCacheDir << " ("
               << (ownsSharedCache() ? "owns shared caches" : "shared caches are read-only") << "), "
               << mActiveInstanceCount << " instance(s) running" << LL_ENDL;
    return mSlot;
}

std::string FSInstanceSlots::getSlotDirName(const std::string& base_name) const
{
    return mSlot > 0 ? llformat("%s_%d", base_name.c_str(), mSlot) : base_name;
}

bool FSInstanceSlots::isSlotHeldByOther(S32 slot) const
{
    // Note: our own slot must never be probed. With POSIX record locks, closing any descriptor
    // of a file releases every lock this process holds on it.
    const std::string file_name = getSlotFileName(slot);
    if (!LLFile::isfile(file_name))
    {
        return false;
    }

    LLAPRFile probe;
    if (probe.open(file_name, LL_APR_AB) != APR_SUCCESS || !probe.getFileHandle())
    {
        return false;
    }
    const bool held = apr_file_lock(probe.getFileHandle(), APR_FLOCK_NONBLOCK | APR_FLOCK_EXCLUSIVE) != APR_SUCCESS;
    if (!held)
    {
        apr_file_unlock(probe.getFileHandle());
    }
    probe.close();
    return held;
}

S32 FSInstanceSlots::getActiveInstanceCount()
{
    if (mSlot < 0)
    {
        return 1;
    }
    if (mActiveInstanceCount > 0 && mActiveCountTimer.getElapsedTimeF32() < ACTIVE_COUNT_RECHECK_SECS)
    {
        return mActiveInstanceCount;
    }
    mActiveCountTimer.reset();

    S32 count = 1; // ourselves
    for (S32 slot = 0; slot < MAX_SLOTS; ++slot)
    {
        if (slot != mSlot && isSlotHeldByOther(slot))
        {
            ++count;
        }
    }

    if (count != mActiveInstanceCount && mActiveInstanceCount > 0)
    {
        LL_INFOS() << "Viewer instances sharing this cache: " << count << LL_ENDL;
    }
    mActiveInstanceCount = count;
    return mActiveInstanceCount;
}
