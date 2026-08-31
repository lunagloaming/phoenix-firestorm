/**
 * @file fsapplescriptcommands.mm
 * @brief macOS AppleScript adapter: NSScriptCommand subclasses and the
 *        application scripting properties, wired to the cross-platform
 *        automation dispatcher through fsapplescriptadapter.h.
 *
 * Cocoa instantiates the command classes named here from Firestorm.sdef; there
 * is nothing to register at runtime. All of this code runs on the main thread
 * (Cocoa delivers scripting events on the main run loop).
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

#import <Cocoa/Cocoa.h>

#include <string>
#include <vector>

#include "fsapplescriptadapter.h"

using namespace FSAppleScriptAdapter;

// AppleEvent enumerator codes for the "chat type" enumeration in Firestorm.sdef.
enum
{
    kFSChatTypeWhisper = 'CtyW',
    kFSChatTypeNormal  = 'CtyN',
    kFSChatTypeShout   = 'CtyS'
};

// AppleEvent enumerator codes for the "location format" enumeration in Firestorm.sdef.
enum
{
    kFSLocFormatLines       = 'LfmL',
    kFSLocFormatJSON        = 'LfmJ',
    kFSLocFormatAppleScript = 'LfmR'
};

// Generic "the command failed" script error number (conventionally errAEEventFailed).
// The human-readable reason travels in scriptErrorString.
static const NSInteger kFSScriptError = -10000;

#pragma mark - helpers

static NSString* FSStr(const std::string& s)
{
    NSString* out = [NSString stringWithUTF8String:s.c_str()];
    return out ? out : @"";
}

static std::string FSStdString(id value)
{
    if ([value isKindOfClass:[NSString class]])
    {
        const char* c = [(NSString*)value UTF8String];
        return c ? std::string(c) : std::string();
    }
    if ([value respondsToSelector:@selector(stringValue)])
    {
        NSString* s = [value stringValue];
        const char* c = [s UTF8String];
        return c ? std::string(c) : std::string();
    }
    return std::string();
}

static NSArray* FSNumberArray(const double v[3])
{
    return @[ @(v[0]), @(v[1]), @(v[2]) ];
}

// Apply an adapter Result to a command: returns YES on success, or sets the
// script error and returns NO.
static BOOL FSApply(NSScriptCommand* cmd, const Result& r)
{
    if (r.ok)
    {
        return YES;
    }
    cmd.scriptErrorNumber = kFSScriptError;
    cmd.scriptErrorString = FSStr(r.error.empty() ? std::string("Firestorm could not handle the command")
                                                  : r.error);
    return NO;
}

#pragma mark - command classes

@interface FSSendChatCommand : NSScriptCommand
@end

@implementation FSSendChatCommand
- (id)performDefaultImplementation
{
    NSDictionary* args = [self evaluatedArguments];
    std::string message = FSStdString([self directParameter]);

    int channel = 0;
    id channelArg = args[@"channel"];
    if (channelArg)
        channel = [channelArg intValue];

    int chatType = CHAT_NORMAL;
    id typeArg = args[@"chatType"];
    if (typeArg)
    {
        FourCharCode code = (FourCharCode)[typeArg unsignedIntValue];
        if (code == kFSChatTypeWhisper)      chatType = CHAT_WHISPER;
        else if (code == kFSChatTypeShout)   chatType = CHAT_SHOUT;
        else                                 chatType = CHAT_NORMAL;
    }

    FSApply(self, asSendChat(message, channel, chatType));
    return nil;
}
@end


@interface FSSendIMCommand : NSScriptCommand
@end

@implementation FSSendIMCommand
- (id)performDefaultImplementation
{
    NSDictionary* args = [self evaluatedArguments];
    std::string message = FSStdString([self directParameter]);

    id groupArg = args[@"group"];
    id avatarArg = args[@"target"];

    if (groupArg)
        FSApply(self, asSendGroupIM(message, FSStdString(groupArg)));
    else if (avatarArg)
        FSApply(self, asSendIM(message, FSStdString(avatarArg)));
    else
    {
        self.scriptErrorNumber = kFSScriptError;
        self.scriptErrorString = @"Specify a recipient with 'to' (avatar) or 'to group' (group).";
    }
    return nil;
}
@end


@interface FSTeleportCommand : NSScriptCommand
@end

@implementation FSTeleportCommand
- (id)performDefaultImplementation
{
    NSDictionary* args = [self evaluatedArguments];

    std::string region;
    id regionArg = args[@"region"];
    if (regionArg)
        region = FSStdString(regionArg);

    id xArg = args[@"x"];
    id yArg = args[@"y"];
    id zArg = args[@"z"];
    bool haveCoords = (xArg != nil && yArg != nil);
    double x = xArg ? [xArg doubleValue] : 0.0;
    double y = yArg ? [yArg doubleValue] : 0.0;
    double z = zArg ? [zArg doubleValue] : 0.0;

    FSApply(self, asTeleport(region, haveCoords, x, y, z));
    return nil;
}
@end


@interface FSTeleportHomeCommand : NSScriptCommand
@end

@implementation FSTeleportHomeCommand
- (id)performDefaultImplementation
{
    FSApply(self, asTeleportHome());
    return nil;
}
@end


@interface FSGetSettingCommand : NSScriptCommand
@end

@implementation FSGetSettingCommand
- (id)performDefaultImplementation
{
    NSDictionary* args = [self evaluatedArguments];
    std::string key = FSStdString([self directParameter]);
    std::string group;
    id groupArg = args[@"group"];
    if (groupArg)
        group = FSStdString(groupArg);

    std::string value;
    Result r = asGetSetting(group, key, value);
    if (!FSApply(self, r))
        return nil;
    return FSStr(value);
}
@end


@interface FSGetLocationCommand : NSScriptCommand
@end

// Quote a string as an AppleScript literal: "..." with \ and " escaped.
static NSString* FSAppleScriptQuote(NSString* s)
{
    NSMutableString* m = [NSMutableString stringWithString:(s ? s : @"")];
    [m replaceOccurrencesOfString:@"\\" withString:@"\\\\"
                          options:0 range:NSMakeRange(0, m.length)];
    [m replaceOccurrencesOfString:@"\"" withString:@"\\\""
                          options:0 range:NSMakeRange(0, m.length)];
    return [NSString stringWithFormat:@"\"%@\"", m];
}

@implementation FSGetLocationCommand
- (id)performDefaultImplementation
{
    Location loc = asGetLocation();
    if (!loc.ok)
    {
        self.scriptErrorNumber = kFSScriptError;
        self.scriptErrorString = FSStr(loc.error.empty()
            ? std::string("Could not read the current location") : loc.error);
        return nil;
    }

    // Cocoa reliably marshals only an NSString return from a script command
    // (NSArray / NSDictionary returns silently vanish), so every encoding below
    // leaves as a string. The 'as' parameter picks which; the default is the
    // AppleScript record literal (materialise it with 'run script').
    FourCharCode fmt = kFSLocFormatAppleScript;
    id fmtArg = [self evaluatedArguments][@"format"];
    if (fmtArg)
        fmt = (FourCharCode)[fmtArg unsignedIntValue];

    NSString* region   = FSStr(loc.region);
    NSString* parcel   = FSStr(loc.parcel);
    NSString* maturity = FSStr(loc.maturity);
    NSString* slurl    = FSStr(loc.slurl);

    if (fmt == kFSLocFormatJSON)
    {
        // The dictionary is only serialised, never returned to AppleScript, so
        // the NSDictionary-return limitation does not apply. NSJSONSerialization
        // handles all string escaping.
        NSDictionary* obj = @{
            @"region"          : region,
            @"parcel"          : parcel,
            @"maturity"        : maturity,
            @"slurl"           : slurl,
            @"region_position" : FSNumberArray(loc.region_pos),
            @"global_position" : FSNumberArray(loc.global_pos),
            @"facing"          : FSNumberArray(loc.facing),
            @"heading"         : @(loc.heading),
        };
        NSData* data = [NSJSONSerialization dataWithJSONObject:obj options:0 error:NULL];
        if (!data)
        {
            self.scriptErrorNumber = kFSScriptError;
            self.scriptErrorString = @"Could not encode location as JSON";
            return nil;
        }
        return [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
    }

    if (fmt == kFSLocFormatAppleScript)
    {
        // A record literal string, materialised by the caller with 'run script'.
        return [NSString stringWithFormat:
            @"{region:%@, parcel:%@, maturity:%@, SLURL:%@, "
             "region_position:{%.6f, %.6f, %.6f}, global_position:{%.6f, %.6f, %.6f}, "
             "facing:{%.6f, %.6f, %.6f}, heading:%.6f}",
            FSAppleScriptQuote(region), FSAppleScriptQuote(parcel),
            FSAppleScriptQuote(maturity), FSAppleScriptQuote(slurl),
            loc.region_pos[0], loc.region_pos[1], loc.region_pos[2],
            loc.global_pos[0], loc.global_pos[1], loc.global_pos[2],
            loc.facing[0], loc.facing[1], loc.facing[2], loc.heading];
    }

    // kFSLocFormatLines (default): 14 newline-separated values, in order
    //   region, parcel, maturity, SLURL, region_x,y,z, global_x,y,z,
    //   facing_x,y,z, heading
    // Split in AppleScript with:  paragraphs of (capture location)
    NSMutableArray* parts = [NSMutableArray arrayWithCapacity:14];
    [parts addObject:region];
    [parts addObject:parcel];
    [parts addObject:maturity];
    [parts addObject:slurl];
    for (int i = 0; i < 3; ++i)
        [parts addObject:[NSString stringWithFormat:@"%.6f", loc.region_pos[i]]];
    for (int i = 0; i < 3; ++i)
        [parts addObject:[NSString stringWithFormat:@"%.6f", loc.global_pos[i]]];
    for (int i = 0; i < 3; ++i)
        [parts addObject:[NSString stringWithFormat:@"%.6f", loc.facing[i]]];
    [parts addObject:[NSString stringWithFormat:@"%.6f", loc.heading]];
    return [parts componentsJoinedByString:@"\n"];
}
@end


@interface FSSetSettingCommand : NSScriptCommand
@end

@implementation FSSetSettingCommand
- (id)performDefaultImplementation
{
    NSDictionary* args = [self evaluatedArguments];
    std::string key = FSStdString([self directParameter]);
    std::string group;
    id groupArg = args[@"group"];
    if (groupArg)
        group = FSStdString(groupArg);

    std::string value = FSStdString(args[@"value"]);
    FSApply(self, asSetSetting(group, key, value));
    return nil;
}
@end


@interface FSDispatchCommand : NSScriptCommand
@end

@implementation FSDispatchCommand
- (id)performDefaultImplementation
{
    NSDictionary* args = [self evaluatedArguments];
    std::string command = FSStdString([self directParameter]);

    std::vector<std::string> params;
    id paramArg = args[@"parameters"];
    if ([paramArg isKindOfClass:[NSArray class]])
    {
        for (id item in (NSArray*)paramArg)
            params.push_back(FSStdString(item));
    }
    else if (paramArg)
    {
        params.push_back(FSStdString(paramArg));
    }

    Result r = asDispatchCommand(command, params);
    if (!FSApply(self, r))
        return nil;
    return @YES;
}
@end


@interface FSRunGestureCommand : NSScriptCommand
@end

@implementation FSRunGestureCommand
- (id)performDefaultImplementation
{
    FSApply(self, asRunGesture(FSStdString([self directParameter])));
    return nil;
}
@end


@interface FSCallUIActionCommand : NSScriptCommand
@end

@implementation FSCallUIActionCommand
- (id)performDefaultImplementation
{
    NSDictionary* args = [self evaluatedArguments];
    std::string name = FSStdString([self directParameter]);
    std::string parameter = FSStdString(args[@"parameter"]);
    FSApply(self, asCallUIAction(name, parameter));
    return nil;
}
@end


#pragma mark - nearby avatar scripting object

@interface FSNearbyAvatarScriptObject : NSObject
{
    NSString* _fsName;
    NSString* _fsAvKey;
    NSArray*  _fsGlobalPosition;
    NSArray*  _fsRegionPosition;
    NSNumber* _fsDistance;
    NSUInteger _fsIndex;
}
@property (nonatomic, retain) NSString* fsName;
@property (nonatomic, retain) NSString* fsAvKey;
@property (nonatomic, retain) NSArray*  fsGlobalPosition;
@property (nonatomic, retain) NSArray*  fsRegionPosition;
@property (nonatomic, retain) NSNumber* fsDistance;
@property (nonatomic, assign) NSUInteger fsIndex;
@end

@implementation FSNearbyAvatarScriptObject
@synthesize fsName = _fsName;
@synthesize fsAvKey = _fsAvKey;
@synthesize fsGlobalPosition = _fsGlobalPosition;
@synthesize fsRegionPosition = _fsRegionPosition;
@synthesize fsDistance = _fsDistance;
@synthesize fsIndex = _fsIndex;

- (void)dealloc
{
    [_fsName release];
    [_fsAvKey release];
    [_fsGlobalPosition release];
    [_fsRegionPosition release];
    [_fsDistance release];
    [super dealloc];
}

- (NSScriptObjectSpecifier*)objectSpecifier
{
    NSScriptClassDescription* appDesc = (NSScriptClassDescription*)
        [NSClassDescription classDescriptionForClass:[NSApplication class]];
    return [[[NSIndexSpecifier alloc] initWithContainerClassDescription:appDesc
                                                   containerSpecifier:nil
                                                                  key:@"fsNearbyAvatars"
                                                                index:(NSInteger)_fsIndex]
            autorelease];
}
@end


#pragma mark - application scripting properties

@interface NSApplication (FSScripting)
@end

@implementation NSApplication (FSScripting)

- (NSNumber*)fsLoggedIn
{
    return @(asIsLoggedIn());
}

- (NSString*)fsCurrentRegion
{
    return FSStr(asRegionName());
}

- (NSString*)fsAvatarName
{
    return FSStr(asAvatarName());
}

- (NSString*)fsAvatarLegacyName
{
    return FSStr(asAvatarLegacyName());
}

- (NSString*)fsAvatarKey
{
    return FSStr(asAvatarKey());
}

- (NSArray*)fsPosition
{
    double p[3] = {0, 0, 0};
    asPositionGlobal(p[0], p[1], p[2]);
    return FSNumberArray(p);
}

- (NSArray*)fsRegionPosition
{
    double p[3] = {0, 0, 0};
    asPositionRegion(p[0], p[1], p[2]);
    return FSNumberArray(p);
}

- (NSString*)fsLocationString
{
    return FSStr(asLocationString());
}

- (NSArray*)fsNearbyAvatars
{
    std::vector<NearbyAvatar> avatars = asNearbyAvatars(0.0);
    NSMutableArray* result = [NSMutableArray arrayWithCapacity:avatars.size()];
    NSUInteger idx = 0;
    for (const auto& a : avatars)
    {
        FSNearbyAvatarScriptObject* obj =
            [[[FSNearbyAvatarScriptObject alloc] init] autorelease];
        obj.fsName = FSStr(a.name);
        obj.fsAvKey = FSStr(a.id);
        obj.fsGlobalPosition = FSNumberArray(a.global_pos);
        obj.fsRegionPosition = FSNumberArray(a.region_pos);
        obj.fsDistance = @(a.distance);
        obj.fsIndex = idx++;
        [result addObject:obj];
    }
    return result;
}

@end
