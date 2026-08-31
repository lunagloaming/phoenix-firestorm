/**
 * @file fsautomation.cpp
 * @brief Implementation of the platform-agnostic automation command dispatcher.
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

#include "fsautomation.h"

#include <cmath>
#include <exception>

#include "indra_constants.h"        // CHAT_CHANNEL_DEBUG
#include "llevents.h"
#include "lleventapi.h"
#include "lleventdispatcher.h"      // sendReply()
#include "llmainthreadtask.h"
#include "llsdutil.h"               // llsd::inArray()
#include "llstring.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

#include "llagent.h"
#include "llagentui.h"
#include "llavataractions.h"
#include "llavatarnamecache.h"
#include "llchat.h"
#include "llcommandhandler.h"
#include "llcontrol.h"
#include "llgesturemgr.h"
#include "llgroupactions.h"
#include "llimview.h"
#include "llslurl.h"
#include "llstartup.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"

// send_chat_from_viewer() is a free function defined in fsnearbychathub.cpp; it
// applies RLV filtering and UTF-8 length splitting before it hits the wire.
extern void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel);

namespace
{
    LLSD makeOk()
    {
        LLSD r;
        r["ok"] = true;
        return r;
    }

    LLSD makeOk(LLSD payload)
    {
        payload["ok"] = true;
        return payload;
    }

    LLSD makeError(const std::string& msg)
    {
        LLSD r;
        r["ok"] = false;
        r["error"] = msg;
        return r;
    }

    bool viewerReady()
    {
        return LLStartUp::getStartupState() == STATE_STARTED && isAgentAvatarValid();
    }

    bool parseUUID(const std::string& in, LLUUID& out)
    {
        if (!LLUUID::validate(in))
            return false;
        out.set(in);
        return out.notNull();
    }

    void fillVec3d(LLSD& target, const LLVector3d& v)
    {
        target = LLSD::emptyArray();
        target.append(v.mdV[VX]);
        target.append(v.mdV[VY]);
        target.append(v.mdV[VZ]);
    }

    void fillVec3(LLSD& target, const LLVector3& v)
    {
        target = LLSD::emptyArray();
        target.append(v.mV[VX]);
        target.append(v.mV[VY]);
        target.append(v.mV[VZ]);
    }
}

// ----------------------------------------------------------------------------
// The dispatcher / LLEventAPI
// ----------------------------------------------------------------------------
namespace
{

class FSAutomationAPI : public LLEventAPI
{
public:
    FSAutomationAPI();

    static FSAutomationAPI& instance();

    // Main-thread synchronous verb invocation. Always returns a result map.
    LLSD invoke(const std::string& verb, const LLSD& args);

private:
    LLSD verbSendChat(const LLSD& args);
    LLSD verbSendIM(const LLSD& args, bool group);
    LLSD verbTeleport(const LLSD& args);
    LLSD verbTeleportHome(const LLSD& args);
    LLSD verbGetSetting(const LLSD& args);
    LLSD verbSetSetting(const LLSD& args);
    LLSD verbDispatchCommand(const LLSD& args);
    LLSD verbRunGesture(const LLSD& args);
    LLSD verbCallUIAction(const LLSD& args);
    LLSD verbGetState(const LLSD& args);
    LLSD verbGetLocation(const LLSD& args);
    LLSD verbGetNearbyAvatars(const LLSD& args);
};

FSAutomationAPI::FSAutomationAPI()
:   LLEventAPI(FSAutomation::PUMP_NAME,
               "Firestorm platform-agnostic automation vocabulary.\n"
               "Every operation returns { \"ok\": bool, ... } or\n"
               "{ \"ok\": false, \"error\": \"...\" } in its reply.")
{
    // Register every verb as its own operation so LEAP plugins get the same
    // vocabulary the OS adapters use. Each op forwards to invoke().
    static const char* const verbs[] = {
        "sendChat", "sendIM", "sendGroupIM", "teleport", "teleportHome",
        "getSetting", "setSetting", "dispatchCommand", "runGesture",
        "callUIAction", "getState", "getLocation", "getNearbyAvatars"
    };
    for (const char* const name : verbs)
    {
        const std::string verb(name);
        add(verb,
            "See the FSAutomation source for the arguments accepted by \"" + verb + "\".",
            [this, verb](const LLSD& event)
            {
                LLEventAPI::Response response(this->invoke(verb, event), event);
            });
    }
}

// File-scope instance, mirroring "LLTeleportHandler gTeleportHandler;" in
// llurldispatcher.cpp: constructing it registers the "FSAutomation" LLEventPump
// so LEAP plugins and in-process callers can reach the vocabulary from startup.
FSAutomationAPI gFSAutomationAPI;

//static
FSAutomationAPI& FSAutomationAPI::instance()
{
    return gFSAutomationAPI;
}

LLSD FSAutomationAPI::invoke(const std::string& verb, const LLSD& args)
{
    LL_DEBUGS("FSAutomation") << "invoke: verb='" << verb << "'" << LL_ENDL;
    try
    {
        if (verb == "sendChat")          return verbSendChat(args);
        if (verb == "sendIM")            return verbSendIM(args, false);
        if (verb == "sendGroupIM")       return verbSendIM(args, true);
        if (verb == "teleport")          return verbTeleport(args);
        if (verb == "teleportHome")      return verbTeleportHome(args);
        if (verb == "getSetting")        return verbGetSetting(args);
        if (verb == "setSetting")        return verbSetSetting(args);
        if (verb == "dispatchCommand")   return verbDispatchCommand(args);
        if (verb == "runGesture")        return verbRunGesture(args);
        if (verb == "callUIAction")      return verbCallUIAction(args);
        if (verb == "getState")          return verbGetState(args);
        if (verb == "getLocation")       return verbGetLocation(args);
        if (verb == "getNearbyAvatars")  return verbGetNearbyAvatars(args);
        return makeError("Unknown automation verb '" + verb + "'");
    }
    catch (const std::exception& e)
    {
        return makeError(std::string("Automation verb '") + verb + "' threw: " + e.what());
    }
    catch (...)
    {
        return makeError(std::string("Automation verb '") + verb + "' threw a non-standard exception");
    }
}

LLSD FSAutomationAPI::verbSendChat(const LLSD& args)
{
    if (!viewerReady())
        return makeError("Firestorm is not logged in yet");

    std::string text = args["message"].asString();
    if (text.empty())
        return makeError("Nothing to say: 'message' is empty");

    S32 channel = args.has("channel") ? args["channel"].asInteger() : 0;
    if (channel < 0 || channel >= CHAT_CHANNEL_DEBUG)
        return makeError("'channel' is out of range");

    EChatType type = CHAT_TYPE_NORMAL;
    std::string type_str = args["type"].asString();
    if (type_str == "whisper")   type = CHAT_TYPE_WHISPER;
    else if (type_str == "shout") type = CHAT_TYPE_SHOUT;

    // Mirror FSNearbyChatBarListener: non-zero channels need a "/42 " prefix so
    // the normal chat pipeline routes the text correctly.
    if (channel != 0)
        text = "/" + std::to_string(channel) + " " + text;

    send_chat_from_viewer(text, type, channel);
    return makeOk();
}

LLSD FSAutomationAPI::verbSendIM(const LLSD& args, bool group)
{
    if (!viewerReady())
        return makeError("Firestorm is not logged in yet");

    std::string text = args["message"].asString();
    if (text.empty())
        return makeError("Nothing to say: 'message' is empty");

    const std::string key = group ? "group" : "target";
    LLUUID target;
    if (!parseUUID(args[key].asString(), target))
        return makeError("'" + key + "' must be a " + (group ? "group" : "avatar") + " UUID");

    if (group)
    {
        if (!LLGroupActions::isInGroup(target))
            return makeError("You are not a member of that group");
        LLUUID session_id = gIMMgr->computeSessionID(IM_SESSION_GROUP_START, target);
        LLIMModel::sendMessage(text, session_id, target, IM_SESSION_SEND);
    }
    else
    {
        LLUUID session_id = LLIMMgr::computeSessionID(IM_NOTHING_SPECIAL, target);
        LLAvatarActions::startIM(target);
        LLIMModel::sendMessage(text, session_id, target, IM_NOTHING_SPECIAL);
    }
    return makeOk();
}

LLSD FSAutomationAPI::verbTeleport(const LLSD& args)
{
    if (!viewerReady())
        return makeError("Firestorm is not logged in yet");

    std::string region = args["regionname"].asString();
    bool have_coords = args.has("x") && args.has("y");

    LLSD request;
    request["op"] = "teleport";
    if (!region.empty())
    {
        request["regionname"] = region;
        if (have_coords)
        {
            request["x"] = args["x"];
            request["y"] = args["y"];
            request["z"] = args.has("z") ? args["z"] : LLSD(0.0);
        }
    }
    else
    {
        if (!have_coords)
            return makeError("Specify either 'regionname' or global 'x'/'y'/'z'");
        request["x"] = args["x"];
        request["y"] = args["y"];
        request["z"] = args.has("z") ? args["z"] : LLSD(0.0);
    }

    // LLTeleportHandler's "from_event" does the region-name lookup (when given)
    // and teleports with no confirmation dialog. Fire and forget.
    LLEventPumps::instance().obtain("LLTeleportHandler").post(request);
    return makeOk();
}

LLSD FSAutomationAPI::verbTeleportHome(const LLSD& args)
{
    if (!viewerReady())
        return makeError("Firestorm is not logged in yet");
    gAgent.teleportHome();
    return makeOk();
}

LLSD FSAutomationAPI::verbGetSetting(const LLSD& args)
{
    std::string groupname = args.has("group") ? args["group"].asString() : std::string("Global");
    auto group = LLControlGroup::getInstance(groupname);
    if (!group)
        return makeError("Unrecognized settings group '" + groupname + "'");

    std::string key = args["key"].asString();
    LLControlVariablePtr control = group->getControl(key);
    if (!control)
        return makeError("In group '" + groupname + "', unknown setting '" + key + "'");

    LLSD payload;
    payload["value"] = control->get();
    payload["type"] = LLControlGroup::typeEnumToString(control->type());
    return makeOk(payload);
}

LLSD FSAutomationAPI::verbSetSetting(const LLSD& args)
{
    std::string groupname = args.has("group") ? args["group"].asString() : std::string("Global");
    auto group = LLControlGroup::getInstance(groupname);
    if (!group)
        return makeError("Unrecognized settings group '" + groupname + "'");

    std::string key = args["key"].asString();
    LLControlVariablePtr control = group->getControl(key);
    if (!control)
        return makeError("In group '" + groupname + "', unknown setting '" + key + "'");

    // Coerce the incoming value into the control's declared type so that e.g. a
    // string "128" lands as an integer.
    LLSD value = args["value"];
    LLSD typed;
    switch (control->type())
    {
        case TYPE_U32:
        case TYPE_S32:
            typed = value.asInteger();
            break;
        case TYPE_F32:
            typed = value.asReal();
            break;
        case TYPE_BOOLEAN:
        {
            if (value.isString())
            {
                std::string v = value.asString();
                LLStringUtil::toLower(v);
                typed = (v == "1" || v == "true" || v == "yes" || v == "on");
            }
            else
            {
                typed = value.asBoolean();
            }
            break;
        }
        default:
            typed = value;
            break;
    }

    control->setValue(typed);
    return makeOk();
}

LLSD FSAutomationAPI::verbDispatchCommand(const LLSD& args)
{
    if (!viewerReady())
        return makeError("Firestorm is not logged in yet");

    std::string command = args["command"].asString();

    // Accept either a bare command ("agent") or a full "secondlife:///app/..." URL.
    const std::string prefix("secondlife:///app/");
    if (command.compare(0, prefix.size(), prefix) == 0)
        command = command.substr(prefix.size());
    while (!command.empty() && command[0] == '/')
        command.erase(0, 1);

    // "cmd/a/b/c" - split the leading token as the command, fold the rest into
    // the parameter array, then append any explicit params.
    LLSD params = LLSD::emptyArray();
    std::string::size_type slash = command.find('/');
    if (slash != std::string::npos)
    {
        std::string rest = command.substr(slash + 1);
        command = command.substr(0, slash);
        std::string::size_type start = 0;
        while (start <= rest.size())
        {
            std::string::size_type next = rest.find('/', start);
            if (next == std::string::npos)
            {
                params.append(rest.substr(start));
                break;
            }
            params.append(rest.substr(start, next - start));
            start = next + 1;
        }
    }
    if (args.has("params") && args["params"].isArray())
    {
        for (const auto& p : llsd::inArray(args["params"]))
            params.append(p);
    }

    if (command.empty())
        return makeError("No command given");

    // trusted_browser = true: scripted commands are not subject to the
    // untrusted-browser block / throttle policy.
    bool handled = LLCommandDispatcher::dispatch(
        command, params, LLSD::emptyMap(), std::string(), NULL,
        LLCommandHandler::NAV_TYPE_CLICKED, true);

    if (!handled)
        return makeError("Command '" + command + "' was not handled");

    LLSD payload;
    payload["handled"] = true;
    return makeOk(payload);
}

LLSD FSAutomationAPI::verbRunGesture(const LLSD& args)
{
    if (!viewerReady())
        return makeError("Firestorm is not logged in yet");

    std::string name = args["name"].asString();
    if (name.empty())
        return makeError("No gesture 'name' given");

    std::string revised;
    if (!LLGestureMgr::instance().triggerAndReviseString(name, &revised))
        return makeError("No active gesture matches '" + name + "'");
    return makeOk();
}

LLSD FSAutomationAPI::verbCallUIAction(const LLSD& args)
{
    std::string name = args["name"].asString();
    if (name.empty())
        return makeError("No UI action 'name' given");

    // Do NOT route through the "UI" LLEventAPI: that path calls LL_ERRS (a fatal
    // crash) when the function name is unknown. Look it up ourselves.
    LLUICtrl::commit_callback_t* func = LLUICtrl::CommitCallbackRegistry::getValue(name);
    if (!func)
        return makeError("Unknown UI action '" + name + "'");

    (*func)(NULL, args["parameter"]);
    return makeOk();
}

LLSD FSAutomationAPI::verbGetState(const LLSD& args)
{
    LLSD payload;
    bool ready = viewerReady();
    payload["logged_in"] = ready;

    std::string legacy;
    LLAgentUI::buildFullname(legacy);
    payload["avatar_legacy_name"] = legacy;
    payload["avatar_key"] = gAgentID.asString();

    LLAvatarName av_name;
    if (LLAvatarNameCache::get(gAgentID, &av_name))
        payload["avatar_name"] = av_name.getUserName();
    else
        payload["avatar_name"] = legacy;

    if (ready)
    {
        if (LLViewerRegion* region = gAgent.getRegion())
            payload["region"] = region->getName();

        fillVec3d(payload["position"], gAgent.getPositionGlobal());
        fillVec3(payload["region_position"], gAgent.getPositionAgent());

        std::string loc;
        if (LLAgentUI::buildLocationString(loc, LLAgentUI::LOCATION_FORMAT_FULL))
            payload["location_string"] = loc;
    }

    return makeOk(payload);
}

LLSD FSAutomationAPI::verbGetLocation(const LLSD& args)
{
    if (!viewerReady())
        return makeError("Firestorm is not logged in yet");

    LLSD payload;

    LLViewerRegion* region = gAgent.getRegion();
    payload["region"] = region ? region->getName() : std::string();
    payload["maturity"] = region ? region->getSimAccessString() : std::string();
    payload["parcel"] = LLViewerParcelMgr::getInstance()->getAgentParcelName();

    fillVec3(payload["region_pos"], gAgent.getPositionAgent());
    fillVec3d(payload["global_pos"], gAgent.getPositionGlobal());

    // Which way the avatar's body is facing: the agent coordinate frame's "at"
    // axis (a unit forward vector) plus a compass heading in degrees measured
    // clockwise from grid north (+Y), normalised to [0, 360). This is local
    // body rotation, not the LLHUDEffectLookAt gaze target, so the "private
    // look at" settings do not affect it.
    const LLVector3 at_axis = gAgent.getAtAxis();
    fillVec3(payload["facing"], at_axis);

    F32 heading = atan2(at_axis.mV[VX], at_axis.mV[VY]) * RAD_TO_DEG;
    if (heading < 0.f)
        heading += 360.f;
    payload["heading"] = heading;

    // Portable landmark token: a maps.secondlife.com / secondlife:// style URL
    // that round-trips through the "dispatchCommand" verb or an in-viewer click.
    LLSLURL slurl;
    LLAgentUI::buildSLURL(slurl);
    payload["slurl"] = slurl.getSLURLString();

    return makeOk(payload);
}

LLSD FSAutomationAPI::verbGetNearbyAvatars(const LLSD& args)
{
    LLSD payload;
    payload["avatars"] = LLSD::emptyArray();
    if (!viewerReady())
        return makeOk(payload);

    F32 radius = args.has("dist") && args["dist"].asReal() > 0.0
        ? (F32)args["dist"].asReal()
        : gSavedSettings.getF32("RenderFarClip");

    const LLVector3d self = gAgent.getPositionGlobal();

    uuid_vec_t ids;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&ids, &positions, self, radius);

    for (size_t i = 0; i < ids.size() && i < positions.size(); ++i)
    {
        if (ids[i] == gAgentID)
            continue;

        LLSD entry;
        entry["id"] = ids[i].asString();

        LLAvatarName av_name;
        if (LLAvatarNameCache::get(ids[i], &av_name))
            entry["name"] = av_name.getUserName();

        const LLVector3d& p = positions[i];
        fillVec3d(entry["global_pos"], p);

        // Region-local coordinates: the horizontal grid is 256 m region tiles;
        // Z has no regional offset.
        LLSD region_pos = LLSD::emptyArray();
        region_pos.append(std::fmod(p.mdV[VX], 256.0));
        region_pos.append(std::fmod(p.mdV[VY], 256.0));
        region_pos.append(p.mdV[VZ]);
        entry["region_pos"] = region_pos;

        entry["distance"] = (p - self).magVec();

        payload["avatars"].append(entry);
    }
    return makeOk(payload);
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// Public entry point
// ----------------------------------------------------------------------------
namespace FSAutomation
{

const std::string PUMP_NAME("FSAutomation");

LLSD call(const std::string& verb, const LLSD& args)
{
    // LLMainThreadTask::dispatch() runs the lambda inline when already on the
    // main thread, otherwise schedules it on the main loop and blocks for the
    // result.
    return LLMainThreadTask::dispatch(
        [&verb, &args]() -> LLSD
        {
            return FSAutomationAPI::instance().invoke(verb, args);
        });
}

} // namespace FSAutomation
