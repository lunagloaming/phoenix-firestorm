/**
 * @file fsapplescriptadapter.cpp
 * @brief LLSD marshalling between the AppleScript command handlers and the
 *        platform-agnostic automation dispatcher.
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

#include "llviewerprecompiledheaders.h"

#include "fsapplescriptadapter.h"

#include "llsd.h"
#include "llsdutil.h"

#include "fsautomation.h"

namespace
{
    using FSAppleScriptAdapter::Result;

    Result toResult(const LLSD& r)
    {
        Result out;
        out.ok = r["ok"].asBoolean();
        if (!out.ok)
        {
            out.error = r.has("error") ? r["error"].asString()
                                       : std::string("Automation call failed");
        }
        return out;
    }

    Result invoke(const std::string& verb, const LLSD& args)
    {
        return toResult(FSAutomation::call(verb, args));
    }

    // Read helper for the scripting properties: run getState once and hand back
    // the payload map (empty map on failure).
    LLSD state()
    {
        LLSD r = FSAutomation::call("getState", LLSD());
        return r["ok"].asBoolean() ? r : LLSD::emptyMap();
    }

    void readVec3(const LLSD& arr, double& x, double& y, double& z)
    {
        if (arr.isArray() && arr.size() >= 3)
        {
            x = arr[0].asReal();
            y = arr[1].asReal();
            z = arr[2].asReal();
        }
    }
}

namespace FSAppleScriptAdapter
{

Result asSendChat(const std::string& message, int channel, int chat_type)
{
    LLSD args;
    args["message"] = message;
    args["channel"] = channel;
    args["type"] = (chat_type == CHAT_WHISPER) ? "whisper"
                 : (chat_type == CHAT_SHOUT)   ? "shout"
                                               : "normal";
    return invoke("sendChat", args);
}

Result asSendIM(const std::string& message, const std::string& target_uuid)
{
    LLSD args;
    args["message"] = message;
    args["target"] = target_uuid;
    return invoke("sendIM", args);
}

Result asSendGroupIM(const std::string& message, const std::string& group_uuid)
{
    LLSD args;
    args["message"] = message;
    args["group"] = group_uuid;
    return invoke("sendGroupIM", args);
}

Result asTeleport(const std::string& region, bool have_coords,
                  double x, double y, double z)
{
    LLSD args;
    if (!region.empty())
        args["regionname"] = region;
    if (have_coords)
    {
        args["x"] = x;
        args["y"] = y;
        args["z"] = z;
    }
    return invoke("teleport", args);
}

Result asTeleportHome()
{
    return invoke("teleportHome", LLSD());
}

Result asGetSetting(const std::string& group, const std::string& key,
                    std::string& out_value)
{
    LLSD args;
    if (!group.empty())
        args["group"] = group;
    args["key"] = key;
    LLSD r = FSAutomation::call("getSetting", args);
    Result out = toResult(r);
    if (out.ok)
        out_value = r["value"].asString();
    return out;
}

Result asSetSetting(const std::string& group, const std::string& key,
                    const std::string& value)
{
    LLSD args;
    if (!group.empty())
        args["group"] = group;
    args["key"] = key;
    args["value"] = value;
    return invoke("setSetting", args);
}

Result asDispatchCommand(const std::string& command,
                         const std::vector<std::string>& params)
{
    LLSD args;
    args["command"] = command;
    LLSD sd_params = LLSD::emptyArray();
    for (const auto& p : params)
        sd_params.append(p);
    args["params"] = sd_params;
    return invoke("dispatchCommand", args);
}

Result asRunGesture(const std::string& name)
{
    LLSD args;
    args["name"] = name;
    return invoke("runGesture", args);
}

Result asCallUIAction(const std::string& name, const std::string& parameter)
{
    LLSD args;
    args["name"] = name;
    if (!parameter.empty())
        args["parameter"] = parameter;
    return invoke("callUIAction", args);
}

bool asIsLoggedIn()
{
    return state()["logged_in"].asBoolean();
}

std::string asRegionName()
{
    return state()["region"].asString();
}

std::string asAvatarName()
{
    return state()["avatar_name"].asString();
}

std::string asAvatarLegacyName()
{
    return state()["avatar_legacy_name"].asString();
}

std::string asAvatarKey()
{
    return state()["avatar_key"].asString();
}

bool asPositionGlobal(double& x, double& y, double& z)
{
    LLSD s = state();
    if (!s["logged_in"].asBoolean() || !s.has("position"))
        return false;
    readVec3(s["position"], x, y, z);
    return true;
}

bool asPositionRegion(double& x, double& y, double& z)
{
    LLSD s = state();
    if (!s["logged_in"].asBoolean() || !s.has("region_position"))
        return false;
    readVec3(s["region_position"], x, y, z);
    return true;
}

std::string asLocationString()
{
    return state()["location_string"].asString();
}

Location asGetLocation()
{
    Location out;
    LLSD r = FSAutomation::call("getLocation", LLSD());
    out.ok = r["ok"].asBoolean();
    if (!out.ok)
    {
        out.error = r.has("error") ? r["error"].asString()
                                   : std::string("Could not read location");
        return out;
    }
    out.region = r["region"].asString();
    out.parcel = r["parcel"].asString();
    out.maturity = r["maturity"].asString();
    out.slurl = r["slurl"].asString();
    readVec3(r["region_pos"], out.region_pos[0], out.region_pos[1], out.region_pos[2]);
    readVec3(r["global_pos"], out.global_pos[0], out.global_pos[1], out.global_pos[2]);
    readVec3(r["facing"], out.facing[0], out.facing[1], out.facing[2]);
    out.heading = r["heading"].asReal();
    return out;
}

std::vector<NearbyAvatar> asNearbyAvatars(double max_dist)
{
    std::vector<NearbyAvatar> result;
    LLSD args;
    if (max_dist > 0.0)
        args["dist"] = max_dist;

    LLSD r = FSAutomation::call("getNearbyAvatars", args);
    if (!r["ok"].asBoolean())
        return result;

    for (const auto& entry : llsd::inArray(r["avatars"]))
    {
        NearbyAvatar a;
        a.name = entry["name"].asString();
        a.id = entry["id"].asString();
        readVec3(entry["global_pos"], a.global_pos[0], a.global_pos[1], a.global_pos[2]);
        readVec3(entry["region_pos"], a.region_pos[0], a.region_pos[1], a.region_pos[2]);
        a.distance = entry["distance"].asReal();
        result.push_back(a);
    }
    return result;
}

} // namespace FSAppleScriptAdapter
