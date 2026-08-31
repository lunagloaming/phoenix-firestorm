/**
 * @file fsautomation.h
 * @brief Platform-agnostic automation command dispatcher for Firestorm.
 *
 * A single "verb + LLSD args -> structured LLSD result" entry point that OS-facing
 * adapters (the macOS AppleScript adapter today; Linux D-Bus and Windows
 * WM_COPYDATA/COM in the future) call to drive the running viewer. Every verb runs
 * on the main loop; FSAutomation::call() marshals for callers on other threads.
 *
 * The dispatcher is also registered as an LLEventAPI on the "FSAutomation" pump so
 * in-process callers and LEAP plugins reach the same curated vocabulary.
 *
 * Result contract - every call returns an LLSD map:
 *   success:  { "ok": true, ...verb-specific payload... }
 *   failure:  { "ok": false, "error": "<human-readable message>" }
 *
 * $LicenseInfo:firstyear=2025&license=viewerlgpl$
 * Copyright (c) 2025 The Phoenix Firestorm Project, Inc.
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

#ifndef FS_AUTOMATION_H
#define FS_AUTOMATION_H

#include <string>
#include "llsd.h"

namespace FSAutomation
{
    /// LLEventPump / LLEventAPI name.
    extern const std::string PUMP_NAME;

    /**
     * Invoke an automation verb with the given arguments and return the
     * structured result (see the result contract above).
     *
     * Safe to call from any thread: when called off the main thread the work is
     * marshalled onto the "mainloop" WorkQueue and the caller blocks for the
     * result. On the main thread it runs inline.
     */
    LLSD call(const std::string& verb, const LLSD& args);
}

#endif // FS_AUTOMATION_H
