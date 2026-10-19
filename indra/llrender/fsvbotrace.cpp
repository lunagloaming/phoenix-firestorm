/**
 * @file fsvbotrace.cpp
 * @brief FSVBOAllocTrace: debug instrumentation of the vertex buffer pool.
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

#include "linden_common.h"

#include "fsvbotrace.h"
#include "llvertexbuffer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sstream>

#if LL_LINUX
#include <dlfcn.h>
#include <execinfo.h>
#include <array>
#include <map>
#include <mutex>
#endif

bool LLVertexBuffer::sAllocTrace = false;

namespace
{
    typedef int (*mallctl_fn)(const char*, void*, size_t*, void*, size_t);

    mallctl_fn get_mallctl()
    {
#if LL_LINUX
        static mallctl_fn fn = (mallctl_fn)dlsym(RTLD_DEFAULT, "mallctl");
        return fn;
#else
        return nullptr;
#endif
    }

    struct ThreadAllocCounters
    {
        U64* allocated = nullptr;
        U64* deallocated = nullptr;
    };

    const ThreadAllocCounters& thread_alloc_counters()
    {
        thread_local ThreadAllocCounters counters = []
        {
            ThreadAllocCounters c;
            if (auto mallctl = get_mallctl())
            {
                size_t sz = sizeof(U64*);
                mallctl("thread.allocatedp", &c.allocated, &sz, nullptr, 0);
                sz = sizeof(U64*);
                mallctl("thread.deallocatedp", &c.deallocated, &sz, nullptr, 0);
            }
            return c;
        }();
        return counters;
    }

    struct AllocTraceStat
    {
        std::atomic<U64> calls{ 0 };
        std::atomic<U64> requested{ 0 }; // bytes the call asked GL for, where that applies
        std::atomic<U64> allocated{ 0 }; // heap bytes allocated during the call
        std::atomic<U64> freed{ 0 };     // heap bytes freed during the call
    };

    const char* const sCallNames[FSVBOTrace::CALL_COUNT] = { "gen", "bind", "create vertex", "create index", "upload", "delete" };
    AllocTraceStat sCalls[FSVBOTrace::CALL_COUNT];
    AllocTraceStat sCreateBySize[32]; // buffer creation by log2 of the requested size

    // Whole-process allocator totals next to the kernel's view of the process, to tell heap bytes that are
    // allocated but never touched (not resident) from memory that really costs RAM or swap.
    void log_allocator_totals()
    {
#if LL_LINUX
        mallctl_fn mallctl = get_mallctl();
        if (!mallctl)
        {
            return;
        }
        U64 epoch = 1;
        size_t len = sizeof(epoch);
        mallctl("epoch", &epoch, &len, &epoch, len); // refresh the cached stats
        auto stat = [mallctl](const char* name) -> size_t
        {
            size_t value = 0;
            size_t size = sizeof(value);
            return mallctl(name, &value, &size, nullptr, 0) ? 0 : value;
        };
        const size_t page = stat("arenas.page");
        // 4096 = MALLCTL_ARENAS_ALL: summed over all arenas
        const size_t dirty = stat("stats.arenas.4096.pdirty") * page;
        const size_t muzzy = stat("stats.arenas.4096.pmuzzy") * page;

        U64 rss_kb = 0, anon_kb = 0, swap_kb = 0;
        if (FILE* f = fopen("/proc/self/status", "r"))
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                sscanf(line, "VmRSS: %llu", (unsigned long long*)&rss_kb);
                sscanf(line, "RssAnon: %llu", (unsigned long long*)&anon_kb);
                sscanf(line, "VmSwap: %llu", (unsigned long long*)&swap_kb);
            }
            fclose(f);
        }

        LL_INFOS("VBOAllocTrace") << "jemalloc MB: allocated " << stat("stats.allocated") / 1048576
            << ", active " << stat("stats.active") / 1048576 << ", resident " << stat("stats.resident") / 1048576
            << ", mapped " << stat("stats.mapped") / 1048576 << ", retained " << stat("stats.retained") / 1048576
            << ", metadata " << stat("stats.metadata") / 1048576 << ", dirty " << dirty / 1048576
            << ", muzzy " << muzzy / 1048576 << "; process MB: rss " << rss_kb / 1024 << ", anon " << anon_kb / 1024
            << ", swap " << swap_kb / 1024 << ", anon+swap " << (anon_kb + swap_kb) / 1024 << LL_ENDL;
#endif
    }

    // Who writes buffer data for upload: bytes flagged by mapVertexBuffer/mapIndexBuffer, summed per call stack
    // (the frames above the map call), over the last minute. "again" counts bytes of buffers that had already
    // been uploaded, i.e. rewrites of existing geometry rather than new buffers.
#if LL_LINUX
    constexpr int MAP_SITE_FRAMES = 6;
    typedef std::array<void*, MAP_SITE_FRAMES> MapSiteKey;
    struct MapSite
    {
        U64 calls = 0;
        U64 bytes = 0;
        U64 again = 0;
    };
    std::mutex sMapSiteMutex;
    std::map<MapSiteKey, MapSite> sMapSites;
#endif

    // Frames are logged as offsets into their module; resolve with addr2line -f -C -e <module> <offsets>.
    void log_map_sites()
    {
#if LL_LINUX
        std::vector<std::pair<MapSiteKey, MapSite>> sites;
        {
            std::lock_guard<std::mutex> lock(sMapSiteMutex);
            sites.assign(sMapSites.begin(), sMapSites.end());
            sMapSites.clear();
        }
        U64 total = 0;
        U64 total_again = 0;
        for (const auto& site : sites)
        {
            total += site.second.bytes;
            total_again += site.second.again;
        }
        std::sort(sites.begin(), sites.end(),
                  [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        LL_INFOS("VBOAllocTrace") << "mapped for upload, last minute: " << total / 1048576 << " MB, "
            << total_again / 1048576 << " MB into buffers uploaded before, " << sites.size() << " call stacks" << LL_ENDL;
        for (size_t i = 0; i < sites.size() && i < 15; ++i)
        {
            std::ostringstream frames;
            for (void* frame : sites[i].first)
            {
                Dl_info info;
                if (frame && dladdr(frame, &info) && info.dli_fname)
                {
                    const char* module = strrchr(info.dli_fname, '/');
                    frames << " " << (module ? module + 1 : info.dli_fname) << "+0x" << std::hex
                           << ((uintptr_t)frame - (uintptr_t)info.dli_fbase) << std::dec;
                }
            }
            LL_INFOS("VBOAllocTrace") << "  " << sites[i].second.bytes / 1048576 << " MB (" << sites[i].second.again / 1048576
                << " MB again), " << sites[i].second.calls << " maps:" << frames.str() << LL_ENDL;
        }
#endif
    }
}

namespace FSVBOTrace
{
    Scope::Scope(ECall which, U64 requested)
    {
        if (!LLVertexBuffer::sAllocTrace)
        {
            return;
        }
        const ThreadAllocCounters& c = thread_alloc_counters();
        if (!c.allocated || !c.deallocated)
        {
            return;
        }
        mCounters = &c;
        mWhich = which;
        mRequested = requested;
        mAllocated = *c.allocated;
        mDeallocated = *c.deallocated;
    }

    Scope::~Scope()
    {
        if (!mCounters)
        {
            return;
        }
        const ThreadAllocCounters* c = static_cast<const ThreadAllocCounters*>(mCounters);
        U64 allocated = *c->allocated - mAllocated;
        U64 freed = *c->deallocated - mDeallocated;
        AllocTraceStat* bucket = nullptr;
        if ((mWhich == CALL_CREATE_VERTEX || mWhich == CALL_CREATE_INDEX) && mRequested)
        {
            U32 bin = 0;
            for (U64 size = mRequested; size > 1 && bin < 31; size >>= 1)
            {
                ++bin;
            }
            bucket = &sCreateBySize[bin];
        }
        for (AllocTraceStat* stat : { &sCalls[mWhich], bucket })
        {
            if (stat)
            {
                stat->calls.fetch_add(1, std::memory_order_relaxed);
                stat->requested.fetch_add(mRequested, std::memory_order_relaxed);
                stat->allocated.fetch_add(allocated, std::memory_order_relaxed);
                stat->freed.fetch_add(freed, std::memory_order_relaxed);
            }
        }
    }

#if LL_LINUX
    __attribute__((noinline)) void traceMapSite(U32 bytes, bool again)
    {
        void* frames[MAP_SITE_FRAMES + 2];
        int count = backtrace(frames, MAP_SITE_FRAMES + 2);
        MapSiteKey key{};
        for (int i = 2; i < count; ++i) // skip this function and the map call
        {
            key[i - 2] = frames[i];
        }
        std::lock_guard<std::mutex> lock(sMapSiteMutex);
        MapSite& site = sMapSites[key];
        ++site.calls;
        site.bytes += bytes;
        if (again)
        {
            site.again += bytes;
        }
    }
#else
    void traceMapSite(U32, bool) {}
#endif

    void log()
    {
        if (!thread_alloc_counters().allocated)
        {
            LL_INFOS("VBOAllocTrace") << "jemalloc thread counters unavailable; nothing traced" << LL_ENDL;
            return;
        }
        log_allocator_totals();
        const char* threaded = getenv("__GL_THREADED_OPTIMIZATIONS");
        LL_INFOS("VBOAllocTrace") << "driver heap during pool GL calls (MB; __GL_THREADED_OPTIMIZATIONS="
            << (threaded ? threaded : "unset") << "):" << LL_ENDL;
        for (U32 i = 0; i < CALL_COUNT; ++i)
        {
            const AllocTraceStat& t = sCalls[i];
            LL_INFOS("VBOAllocTrace") << "  " << sCallNames[i] << ": " << t.calls << " calls, requested "
                << t.requested / 1048576 << ", allocated " << t.allocated / 1048576 << ", freed " << t.freed / 1048576 << LL_ENDL;
        }
        for (U32 i = 0; i < 32; ++i)
        {
            const AllocTraceStat& t = sCreateBySize[i];
            if (t.calls)
            {
                LL_INFOS("VBOAllocTrace") << "  create " << (1ull << i) << "-" << ((2ull << i) - 1) << " B: " << t.calls
                    << " calls, requested " << t.requested / 1024 << " KB, allocated " << t.allocated / 1024
                    << " KB, freed " << t.freed / 1024 << " KB" << LL_ENDL;
            }
        }
        log_map_sites();
    }
}
