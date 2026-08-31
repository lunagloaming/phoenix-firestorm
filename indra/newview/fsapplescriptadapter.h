/**
 * @file fsapplescriptadapter.h
 * @brief Thin typed shim between the Objective-C++ AppleScript command handlers
 *        (fsapplescriptcommands.mm) and the platform-agnostic automation
 *        dispatcher (fsautomation.h).
 *
 * This header intentionally pulls in nothing but <string> / <vector> so the
 * Objective-C++ translation unit never has to include LLSD or viewer core
 * headers next to Cocoa. The .cpp does the LLSD marshalling and calls
 * FSAutomation::call().
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

#ifndef FS_APPLESCRIPTADAPTER_H
#define FS_APPLESCRIPTADAPTER_H

#include <string>
#include <vector>

namespace FSAppleScriptAdapter
{
    // chat_type values for asSendChat().
    enum { CHAT_WHISPER = 0, CHAT_NORMAL = 1, CHAT_SHOUT = 2 };

    struct Result
    {
        bool        ok = false;
        std::string error;      // populated when ok == false
    };

    struct NearbyAvatar
    {
        std::string name;
        std::string id;
        double      global_pos[3] = {0, 0, 0};
        double      region_pos[3] = {0, 0, 0};
        double      distance = 0.0;
    };

    // Everything needed to record an external "landmark" and teleport back.
    struct Location
    {
        bool        ok = false;
        std::string error;
        std::string region;
        std::string parcel;
        std::string maturity;
        std::string slurl;
        double      region_pos[3] = {0, 0, 0};
        double      global_pos[3] = {0, 0, 0};
        double      facing[3] = {0, 0, 0};    // unit forward vector (agent body "at" axis)
        double      heading = 0.0;            // compass degrees clockwise from grid north, [0, 360)
    };

    // ---- commands ---------------------------------------------------------
    Result asSendChat(const std::string& message, int channel, int chat_type);
    Result asSendIM(const std::string& message, const std::string& target_uuid);
    Result asSendGroupIM(const std::string& message, const std::string& group_uuid);
    Result asTeleport(const std::string& region, bool have_coords,
                      double x, double y, double z);
    Result asTeleportHome();
    Result asGetSetting(const std::string& group, const std::string& key,
                        std::string& out_value);
    Result asSetSetting(const std::string& group, const std::string& key,
                        const std::string& value);
    Result asDispatchCommand(const std::string& command,
                             const std::vector<std::string>& params);
    Result asRunGesture(const std::string& name);
    Result asCallUIAction(const std::string& name, const std::string& parameter);

    // ---- read-only state (scripting properties) -------------------------
    // These never throw and return empty / false when the viewer is not ready.
    bool        asIsLoggedIn();
    std::string asRegionName();
    std::string asAvatarName();
    std::string asAvatarLegacyName();
    std::string asAvatarKey();
    bool        asPositionGlobal(double& x, double& y, double& z);
    bool        asPositionRegion(double& x, double& y, double& z);
    std::string asLocationString();

    // One atomic snapshot of the agent's location (region + region/global coords
    // + parcel + maturity + a portable SLURL).
    Location    asGetLocation();

    // max_dist <= 0 uses the RenderFarClip setting.
    std::vector<NearbyAvatar> asNearbyAvatars(double max_dist);
}

#endif // FS_APPLESCRIPTADAPTER_H
