/**
 * @file fsvbotrace.h
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

#ifndef FS_VBOTRACE_H
#define FS_VBOTRACE_H

#include "stdtypes.h"

// Debug instrumentation behind the FSVBOAllocTrace setting (LLVertexBuffer::sAllocTrace), logged once a minute
// under "VBOAllocTrace". Linux with jemalloc only; everything is a no-op elsewhere or while the setting is off.
// - Heap bytes the GL driver allocates and frees on the calling thread inside the pool's GL calls, from jemalloc's
//   per-thread counters. Only meaningful with __GL_THREADED_OPTIMIZATIONS=0, or the driver allocates on its own threads.
// - Allocator totals (live, active, dirty...) next to the process's RSS and swap, to tell untouched heap from real use.
// - Call stacks that write buffer data for upload, by bytes, split into new buffers and rewrites of uploaded ones.
namespace FSVBOTrace
{
    enum ECall
    {
        CALL_GEN,           // glGenBuffers batches
        CALL_BIND,          // glBindBuffer before creating a buffer
        CALL_CREATE_VERTEX, // glBufferData(GL_ARRAY_BUFFER, size, nullptr, ...)
        CALL_CREATE_INDEX,  // glBufferData(GL_ELEMENT_ARRAY_BUFFER, size, nullptr, ...)
        CALL_UPLOAD,        // uploads through flush_vbo
        CALL_DELETE,        // glDeleteBuffers batches
        CALL_COUNT
    };

    // Counts the driver heap allocated and freed while it is alive. requested: bytes the call asks GL for;
    // buffer creations are also binned by that size.
    class Scope
    {
    public:
        Scope(ECall which, U64 requested = 0);
        ~Scope();

    private:
        const void* mCounters = nullptr;
        ECall mWhich = CALL_COUNT;
        U64 mRequested = 0;
        U64 mAllocated = 0;
        U64 mDeallocated = 0;
    };

    // From mapVertexBuffer/mapIndexBuffer: bytes flagged for upload; again: the buffer was uploaded before.
    void traceMapSite(U32 bytes, bool again);

    // Once a minute, from the main thread
    void log();
}

#endif // FS_VBOTRACE_H
