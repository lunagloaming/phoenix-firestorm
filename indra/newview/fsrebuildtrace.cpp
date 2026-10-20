/**
 * @file fsrebuildtrace.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsrebuildtrace.h"

#include "fsvbotrace.h"
#include "lldrawable.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llvovolume.h"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

#if LL_LINUX
#include <execinfo.h>
#endif

namespace
{
    constexpr int STACK_FRAMES = 8;
    typedef std::array<void*, STACK_FRAMES> StackKey;

    // skip: frames to drop at the top, counting this function
#if LL_LINUX
    __attribute__((noinline)) StackKey capture_stack(int skip)
    {
        void* frames[STACK_FRAMES + 4];
        int count = backtrace(frames, STACK_FRAMES + llmin(skip, 4));
        StackKey key{};
        for (int i = skip; i < count; ++i)
        {
            key[i - skip] = frames[i];
        }
        return key;
    }
#else
    StackKey capture_stack(int) { return StackKey{}; }
#endif

    struct Trigger
    {
        U64 calls = 0;
        U64 bytes = 0; // geometry rebuilt in groups this stack queued (group triggers only)
        U32 flags = 0; // LLDrawable::REBUILD_* flags it asked for (drawable triggers only)
    };

    enum ECategory
    {
        CAT_STATIC,         // region objects in the region's octree
        CAT_MOVING,         // linksets with their own spatial bridge: physical or moving objects
        CAT_ATTACHMENT,     // other avatars' attachments
        CAT_OWN_ATTACHMENT,
        CAT_HUD,
        CAT_ANIMESH,
        CAT_COUNT
    };
    const char* const sCategoryNames[CAT_COUNT] = { "static", "moving", "attachment", "own attachment", "HUD", "animesh" };

    enum EFeature
    {
        F_FLEXI = 1 << 0,
        F_RIGGED = 1 << 1,
        F_TEXANIM = 1 << 2,
        F_MEDIA = 1 << 3,
        F_ANIMATED_CHILD = 1 << 4, // child prim moving relative to its root
        F_COUNT = 5
    };
    const char* const sFeatureNames[F_COUNT] = { "flexi", "rigged", "texture anim", "media", "moving child" };

    // Drawable rebuild flags pending at rebuild time; NONE means the group itself was dirtied
    // (objects added or removed, textures changed, alpha re-sort...).
    const U32 sRebuildBits[] = { LLDrawable::REBUILD_VOLUME, LLDrawable::REBUILD_POSITION, LLDrawable::REBUILD_TCOORD,
                                 LLDrawable::REBUILD_COLOR, LLDrawable::REBUILD_RIGGED };
    const char* const sRebuildBitNames[] = { "volume", "position", "tcoord", "color", "rigged" };
    constexpr int REBUILD_BIT_COUNT = sizeof(sRebuildBits) / sizeof(sRebuildBits[0]);

    std::string describe_bits(U32 bits, const U32* values, const char* const* names, int count)
    {
        std::string out;
        for (int i = 0; i < count; ++i)
        {
            if (bits & values[i])
            {
                out += out.empty() ? "" : "+";
                out += names[i];
            }
        }
        return out.empty() ? "-" : out;
    }

    std::string describe_features(U32 features)
    {
        static const U32 values[F_COUNT] = { F_FLEXI, F_RIGGED, F_TEXANIM, F_MEDIA, F_ANIMATED_CHILD };
        return describe_bits(features, values, sFeatureNames, F_COUNT);
    }

    struct GroupStat
    {
        U64 rebuilds = 0;
        U64 bytes = 0;
        U64 alpha_only = 0; // rebuilds for an alpha re-sort only
        U32 rebuild_bits = 0;
        U32 features = 0;
        ECategory category = CAT_STATIC;
        std::string description;
    };

    std::mutex sMutex;
    std::unordered_map<const LLSpatialGroup*, GroupStat> sGroups;
    std::unordered_map<const LLSpatialGroup*, StackKey> sQueuedBy;
    std::map<StackKey, Trigger> sGroupTriggers;
    std::map<StackKey, Trigger> sDrawableTriggers;
    // Octree moves (remove and re-insert) by the group's category, and why the drawable no longer fit
    struct MoveStat
    {
        U64 moves = 0;
        U64 same_group = 0; // re-inserted into the group it was removed from
        U64 outside = 0;    // its centre left the node
        U64 radius = 0;     // centre inside, but its bin radius breaks the node's size rule
        U64 at_root = 0;    // it was in the octree's root node
    };
    MoveStat sMoves[CAT_COUNT];
    struct PendingMove
    {
        const LLDrawable* drawable = nullptr;
        const LLSpatialGroup* from = nullptr;
        ECategory category = CAT_STATIC;
    } sPendingMove;

    // Colour changes by old and new alpha class (invisible, blended, nearly opaque, opaque), and how many were in place
    U64 sColourChanges[4][4] = {};
    U64 sColourInPlace[4][4] = {};

    U64 sBytesByRebuildBit[REBUILD_BIT_COUNT + 1] = {}; // last: no drawable flags pending
    U64 sAlphaOnlyBytes = 0;
    U64 sUnattributedBytes = 0;

    LLViewerObject* bridge_root(LLSpatialGroup* group)
    {
        LLSpatialBridge* bridge = group->getSpatialPartition()->asBridge();
        return bridge && bridge->mDrawable ? bridge->mDrawable->getVObj().get() : nullptr;
    }

    ECategory categorize(LLViewerObject* root)
    {
        if (!root)
        {
            return CAT_STATIC;
        }
        if (root->isHUDAttachment())
        {
            return CAT_HUD;
        }
        if (root->isAttachment())
        {
            LLVOAvatar* avatar = root->getAvatar();
            return avatar && avatar->isSelf() ? CAT_OWN_ATTACHMENT : CAT_ATTACHMENT;
        }
        return root->isAnimatedObject() ? CAT_ANIMESH : CAT_MOVING;
    }

    // Root object (or, in the region octree, the first object), owner for attachments, region and position
    std::string describe_group(LLSpatialGroup* group, LLViewerObject* root, ECategory category)
    {
        LLViewerObject* object = root;
        for (auto it = group->getDataBegin(); !object && it != group->getDataEnd(); ++it)
        {
            LLDrawable* drawable = (LLDrawable*)(*it)->getDrawable();
            object = drawable ? drawable->getVObj().get() : nullptr;
        }
        std::ostringstream out;
        out << sCategoryNames[category] << " " << (object ? object->getID().asString() : "?");
        if (object && (category == CAT_ATTACHMENT || category == CAT_OWN_ATTACHMENT))
        {
            if (LLVOAvatar* avatar = object->getAvatar())
            {
                out << " on " << avatar->getFullname();
            }
        }
        else if (object)
        {
            const LLVector3& pos = object->getPositionRegion();
            out << " in " << (object->getRegion() ? object->getRegion()->getName() : "?") << " at "
                << ll_round(pos.mV[VX]) << "," << ll_round(pos.mV[VY]) << "," << ll_round(pos.mV[VZ]);
        }
        out << ", " << group->getElementCount() << " drawables";
        return out.str();
    }

    template <typename T>
    std::vector<std::pair<typename T::key_type, typename T::mapped_type>> sorted_by(const T& map,
        U64 (*weight)(const typename T::mapped_type&))
    {
        std::vector<std::pair<typename T::key_type, typename T::mapped_type>> items(map.begin(), map.end());
        std::sort(items.begin(), items.end(), [weight](const auto& a, const auto& b) { return weight(a.second) > weight(b.second); });
        return items;
    }

    void log()
    {
        std::lock_guard<std::mutex> lock(sMutex);

        struct CategoryTotal
        {
            U64 groups = 0, rebuilds = 0, bytes = 0, churn_groups = 0, churn_bytes = 0;
        } totals[CAT_COUNT];
        U64 feature_bytes[F_COUNT] = {};
        U64 all_bytes = 0, all_rebuilds = 0;
        for (const auto& entry : sGroups)
        {
            const GroupStat& stat = entry.second;
            CategoryTotal& total = totals[stat.category];
            ++total.groups;
            total.rebuilds += stat.rebuilds;
            total.bytes += stat.bytes;
            if (stat.rebuilds >= 6) // every 10 seconds or more often
            {
                ++total.churn_groups;
                total.churn_bytes += stat.bytes;
            }
            for (int i = 0; i < F_COUNT; ++i)
            {
                if (stat.features & (1 << i))
                {
                    feature_bytes[i] += stat.bytes;
                }
            }
            all_bytes += stat.bytes;
            all_rebuilds += stat.rebuilds;
        }

        LL_INFOS("RebuildTrace") << "volume geometry rebuilt, last minute: " << all_bytes / 1048576 << " MB in "
            << all_rebuilds << " rebuilds of " << sGroups.size() << " groups; alpha re-sort only " << sAlphaOnlyBytes / 1048576
            << " MB; queued by an unknown caller " << sUnattributedBytes / 1048576 << " MB" << LL_ENDL;
        for (int i = 0; i < CAT_COUNT; ++i)
        {
            const CategoryTotal& t = totals[i];
            if (t.rebuilds)
            {
                LL_INFOS("RebuildTrace") << "  " << sCategoryNames[i] << ": " << t.bytes / 1048576 << " MB, " << t.rebuilds
                    << " rebuilds of " << t.groups << " groups; rebuilt 6+ times: " << t.churn_groups << " groups, "
                    << t.churn_bytes / 1048576 << " MB" << LL_ENDL;
            }
        }
        std::ostringstream features;
        for (int i = 0; i < F_COUNT; ++i)
        {
            features << (i ? ", " : "") << sFeatureNames[i] << " " << feature_bytes[i] / 1048576;
        }
        LL_INFOS("RebuildTrace") << "  MB in groups holding: " << features.str() << LL_ENDL;
        for (int i = 0; i < CAT_COUNT; ++i)
        {
            const MoveStat& m = sMoves[i];
            if (m.moves)
            {
                LL_INFOS("RebuildTrace") << "  octree moves, " << sCategoryNames[i] << ": " << m.moves << ", back into the same group "
                    << m.same_group << "; centre left the node " << m.outside << ", radius rule only " << m.radius
                    << ", from the root node " << m.at_root << LL_ENDL;
            }
        }
        std::ostringstream bits;
        for (int i = 0; i < REBUILD_BIT_COUNT; ++i)
        {
            bits << sRebuildBitNames[i] << " " << sBytesByRebuildBit[i] / 1048576 << ", ";
        }
        LL_INFOS("RebuildTrace") << "  MB by pending drawable flags: " << bits.str() << "none (group dirtied) "
            << sBytesByRebuildBit[REBUILD_BIT_COUNT] / 1048576 << LL_ENDL;

        static const char* const class_names[4] = { "invisible", "blended", "nearly opaque", "opaque" };
        std::ostringstream colours;
        for (int from = 0; from < 4; ++from)
        {
            for (int to = 0; to < 4; ++to)
            {
                if (sColourChanges[from][to])
                {
                    colours << " " << class_names[from] << "->" << class_names[to] << " " << sColourChanges[from][to]
                        << " (" << sColourInPlace[from][to] << " in place);";
                }
            }
        }
        LL_INFOS("RebuildTrace") << "  colour changes:" << colours.str() << LL_ENDL;

        auto groups = sorted_by(sGroups, [](const GroupStat& s) { return s.bytes; });
        for (size_t i = 0; i < groups.size() && i < 20; ++i)
        {
            const GroupStat& s = groups[i].second;
            LL_INFOS("RebuildTrace") << "  " << s.bytes / 1024 << " KB, " << s.rebuilds << " rebuilds (" << s.alpha_only
                << " alpha only), flags " << describe_bits(s.rebuild_bits, sRebuildBits, sRebuildBitNames, REBUILD_BIT_COUNT)
                << ", holds " << describe_features(s.features) << ": " << s.description << LL_ENDL;
        }

        LL_INFOS("RebuildTrace") << "group rebuilds queued by (KB rebuilt, times):" << LL_ENDL;
        auto group_triggers = sorted_by(sGroupTriggers, [](const Trigger& t) { return t.bytes; });
        for (size_t i = 0; i < group_triggers.size() && i < 12; ++i)
        {
            LL_INFOS("RebuildTrace") << "  " << group_triggers[i].second.bytes / 1024 << " KB, " << group_triggers[i].second.calls
                << ":" << FSVBOTrace::describeFrames(group_triggers[i].first.data(), STACK_FRAMES) << LL_ENDL;
        }

        LL_INFOS("RebuildTrace") << "drawable rebuilds requested by (times, flags):" << LL_ENDL;
        auto drawable_triggers = sorted_by(sDrawableTriggers, [](const Trigger& t) { return t.calls; });
        for (size_t i = 0; i < drawable_triggers.size() && i < 12; ++i)
        {
            const Trigger& t = drawable_triggers[i].second;
            LL_INFOS("RebuildTrace") << "  " << t.calls << ", "
                << describe_bits(t.flags, sRebuildBits, sRebuildBitNames, REBUILD_BIT_COUNT) << ":"
                << FSVBOTrace::describeFrames(drawable_triggers[i].first.data(), STACK_FRAMES) << LL_ENDL;
        }

        sGroups.clear();
        sQueuedBy.clear();
        sGroupTriggers.clear();
        sDrawableTriggers.clear();
        std::fill(std::begin(sMoves), std::end(sMoves), MoveStat());
        memset(sColourChanges, 0, sizeof(sColourChanges));
        memset(sColourInPlace, 0, sizeof(sColourInPlace));
        std::fill(std::begin(sBytesByRebuildBit), std::end(sBytesByRebuildBit), 0);
        sAlphaOnlyBytes = 0;
        sUnattributedBytes = 0;
    }
}

namespace FSRebuildTrace
{
    void markGroup(const LLSpatialGroup* group)
    {
        if (!LLVertexBuffer::sAllocTrace)
        {
            return;
        }
        StackKey key = capture_stack(3); // skip this, markGroup and LLPipeline::markRebuild
        std::lock_guard<std::mutex> lock(sMutex);
        ++sGroupTriggers[key].calls;
        sQueuedBy[group] = key;
    }

    void markDrawable(const LLDrawable* drawable, U32 flag)
    {
        if (!LLVertexBuffer::sAllocTrace)
        {
            return;
        }
        // Only count requests that add something: a new queue entry or rebuild flags not already pending
        const U32 state = drawable->getState();
        if ((state & LLDrawable::IN_REBUILD_Q) && !(flag & ~state))
        {
            return;
        }
        StackKey key = capture_stack(3);
        std::lock_guard<std::mutex> lock(sMutex);
        Trigger& trigger = sDrawableTriggers[key];
        ++trigger.calls;
        trigger.flags |= flag;
    }

    void moving(LLDrawable* drawable, LLSpatialGroup* from)
    {
        sPendingMove = PendingMove();
        if (!LLVertexBuffer::sAllocTrace || !from || !from->getOctreeNode())
        {
            return;
        }
        auto* node = from->getOctreeNode();
        const ECategory category = categorize(bridge_root(from));
        std::lock_guard<std::mutex> lock(sMutex);
        MoveStat& stat = sMoves[category];
        ++stat.moves;
        if (!node->isInside(drawable->getPositionGroup()))
        {
            ++stat.outside;
        }
        else if (!node->contains(drawable->getEntry()))
        {
            ++stat.radius;
        }
        if (!node->getParent())
        {
            ++stat.at_root;
        }
        sPendingMove.drawable = drawable;
        sPendingMove.from = from;
        sPendingMove.category = category;
    }

    void moved(LLDrawable* drawable)
    {
        if (sPendingMove.drawable != drawable)
        {
            return;
        }
        if (drawable->getSpatialGroup() == sPendingMove.from)
        {
            std::lock_guard<std::mutex> lock(sMutex);
            ++sMoves[sPendingMove.category].same_group;
        }
        sPendingMove = PendingMove();
    }

    void colourChange(S32 old_class, S32 new_class, bool in_place)
    {
        if (!LLVertexBuffer::sAllocTrace || old_class < 0 || old_class > 3 || new_class < 0 || new_class > 3)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(sMutex);
        ++sColourChanges[old_class][new_class];
        if (in_place)
        {
            ++sColourInPlace[old_class][new_class];
        }
    }

    void rebuilt(LLSpatialGroup* group, U32 bytes)
    {
        if (!LLVertexBuffer::sAllocTrace)
        {
            return;
        }
        U32 rebuild_bits = 0;
        U32 features = 0;
        for (auto it = group->getDataBegin(); it != group->getDataEnd(); ++it)
        {
            LLDrawable* drawable = (LLDrawable*)(*it)->getDrawable();
            if (!drawable)
            {
                continue;
            }
            rebuild_bits |= drawable->getState() & (LLDrawable::REBUILD_ALL | LLDrawable::REBUILD_RIGGED);
            if (drawable->isState(LLDrawable::ANIMATED_CHILD))
            {
                features |= F_ANIMATED_CHILD;
            }
            if (LLVOVolume* volume = drawable->getVOVolume())
            {
                features |= (volume->isFlexible() ? F_FLEXI : 0) | (volume->isRiggedMesh() ? F_RIGGED : 0)
                    | (volume->mTextureAnimp ? F_TEXANIM : 0) | (volume->hasMedia() ? F_MEDIA : 0);
            }
        }
        const bool alpha_only = !group->hasState(LLSpatialGroup::GEOM_DIRTY);

        std::lock_guard<std::mutex> lock(sMutex);
        GroupStat& stat = sGroups[group];
        if (!stat.rebuilds)
        {
            LLViewerObject* root = bridge_root(group);
            stat.category = categorize(root);
            stat.description = describe_group(group, root, stat.category);
        }
        ++stat.rebuilds;
        stat.bytes += bytes;
        stat.rebuild_bits |= rebuild_bits;
        stat.features |= features;
        if (alpha_only)
        {
            ++stat.alpha_only;
            sAlphaOnlyBytes += bytes;
        }
        for (int i = 0; i < REBUILD_BIT_COUNT; ++i)
        {
            if (rebuild_bits & sRebuildBits[i])
            {
                sBytesByRebuildBit[i] += bytes;
            }
        }
        if (!rebuild_bits)
        {
            sBytesByRebuildBit[REBUILD_BIT_COUNT] += bytes;
        }

        auto queued = sQueuedBy.find(group);
        if (queued != sQueuedBy.end())
        {
            sGroupTriggers[queued->second].bytes += bytes;
            sQueuedBy.erase(queued);
        }
        else
        {
            sUnattributedBytes += bytes;
        }
    }

    void idle()
    {
        static F64 last_log = 0.0;
        if (!LLVertexBuffer::sAllocTrace)
        {
            return;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (last_log == 0.0)
        {
            last_log = now;
        }
        else if (now - last_log >= 60.0)
        {
            last_log = now;
            log();
        }
    }
}
