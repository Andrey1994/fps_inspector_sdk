/*
Copyright 2017-2018 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "TraceSession.hpp"
#include "Logger.hpp"

namespace {

VOID WINAPI EventRecordCallback(EVENT_RECORD* pEventRecord)
{
    auto session = (TraceSession*) pEventRecord->UserContext;
    auto const& hdr = pEventRecord->EventHeader;

    if (session->startTime_ == 0) {
        session->startTime_ = hdr.TimeStamp.QuadPart;
    }

    auto iter = session->eventHandler_.find(hdr.ProviderId);
    if (iter != session->eventHandler_.end()) {
        auto const& h = iter->second;
        (*h.fn_)(pEventRecord, h.ctxt_);
    }
}

ULONG WINAPI BufferCallback(EVENT_TRACE_LOGFILEA* pLogFile)
{
    auto session = (TraceSession*) pLogFile->Context;
    auto shouldStopFn = session->shouldStopProcessingEventsFn_;
    if (shouldStopFn && (*shouldStopFn)()) {
        return FALSE; // break out of ProcessTrace()
    }

    return TRUE; // continue processing events
}

bool OpenLogger(
    TraceSession* session,
    char const* name,
    bool realtime)
{
    // Open trace
    EVENT_TRACE_LOGFILEA loggerInfo = {};
    /* Filled out below based on realtime:
    loggerInfo.LogFileName = nullptr;
    loggerInfo.LoggerName = nullptr;
    */
    loggerInfo.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    loggerInfo.BufferCallback = BufferCallback;
    loggerInfo.EventRecordCallback = EventRecordCallback;
    loggerInfo.Context = session;
    /* Output members (passed also to BufferCallback()):
    loggerInfo.CurrentTime
    loggerInfo.BuffersRead
    loggerInfo.CurrentEvent
    loggerInfo.LogfileHeader
    loggerInfo.BufferSize
    loggerInfo.Filled
    loggerInfo.IsKernelTrace
    */
    /* Not used:
    loggerInfo.EventsLost
    */

    if (realtime) {
        loggerInfo.LoggerName = (LPSTR) name;
        loggerInfo.ProcessTraceMode |= PROCESS_TRACE_MODE_REAL_TIME;
    } else {
        loggerInfo.LogFileName = (LPSTR) name;
    }

    session->traceHandle_ = OpenTraceA(&loggerInfo);
    if (session->traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
        g_InspectorLogger->error("failed to open trace");
        auto lastError = GetLastError();
        switch (lastError) {
        case ERROR_INVALID_PARAMETER: g_InspectorLogger->error("(Logfile is NULL)"); break;
        case ERROR_BAD_PATHNAME:      g_InspectorLogger->error("(invalid LoggerName)"); break;
        case ERROR_ACCESS_DENIED:     g_InspectorLogger->error("(access denied)"); break;
        default:                      g_InspectorLogger->error("(error={})", lastError); break;
        }
        return false;
    }

    // Copy desired state from loggerInfo
    session->frequency_ = loggerInfo.LogfileHeader.PerfFreq.QuadPart;
    return true;
}

}

size_t TraceSession::GUIDHash::operator()(GUID const& g) const
{
    static_assert((sizeof(g) % sizeof(size_t)) == 0, "sizeof(GUID) must be multiple of sizeof(size_t)");
    auto p = (size_t const*) &g;
    auto h = (size_t) 0;
    for (size_t i = 0; i < sizeof(g) / sizeof(size_t); ++i) {
        h ^= p[i];
    }
    return h;
}

bool TraceSession::GUIDEqual::operator()(GUID const& lhs, GUID const& rhs) const
{
    return IsEqualGUID(lhs, rhs) != FALSE;
}

bool TraceSession::AddProvider(GUID providerId, UCHAR level,
                               ULONGLONG matchAnyKeyword, ULONGLONG matchAllKeyword)
{
    auto p = eventProvider_.emplace(std::make_pair(providerId, Provider()));
    if (!p.second) {
        return false;
    }

    auto h = &p.first->second;
    h->matchAny_ = matchAnyKeyword;
    h->matchAll_ = matchAllKeyword;
    h->level_    = level;
    return true;
}

bool TraceSession::AddHandler(GUID providerId, EventHandlerFn handlerFn, void* handlerContext)
{
    auto p = eventHandler_.emplace(std::make_pair(providerId, Handler()));
    if (!p.second) {
        return false;
    }

    auto h = &p.first->second;
    h->fn_ = handlerFn;
    h->ctxt_ = handlerContext;
    return true;
}

bool TraceSession::AddProviderAndHandler(GUID providerId, UCHAR level,
                                         ULONGLONG matchAnyKeyword, ULONGLONG matchAllKeyword,
                                         EventHandlerFn handlerFn, void* handlerContext)
{
    if (!AddProvider(providerId, level, matchAnyKeyword, matchAllKeyword))
        return false;
    if (!AddHandler(providerId, handlerFn, handlerContext)) {
        RemoveProvider(providerId);
        return false;
    }
    return true;
}

bool TraceSession::RemoveProvider(GUID providerId)
{
    if (sessionHandle_ != 0) {
        auto status = EnableTraceEx2(sessionHandle_, &providerId, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
        (void) status;
    }

    return eventProvider_.erase(providerId) != 0;
}

bool TraceSession::RemoveHandler(GUID providerId)
{
    return eventHandler_.erase(providerId) != 0;
}

bool TraceSession::RemoveProviderAndHandler(GUID providerId)
{
    return RemoveProvider(providerId) || RemoveHandler(providerId);
}

bool TraceSession::InitializeEtlFile(char const* inputEtlPath, ShouldStopProcessingEventsFn shouldStopFn)
{
    // Open the trace
    if (!OpenLogger(this, inputEtlPath, false)) {
        Finalize();
        return false;
    }

    // Initialize state
    shouldStopProcessingEventsFn_ = shouldStopFn;
    eventsLostCount_ = 0;
    buffersLostCount_ = 0;
    return true;
}

bool TraceSession::InitializeRealtime(char const* traceSessionName, ShouldStopProcessingEventsFn shouldStopFn)
{
    // Set up and start a real-time collection session
    memset(&properties_, 0, sizeof(properties_));

    properties_.Wnode.BufferSize = (ULONG) offsetof(TraceSession, sessionHandle_);
    //properties_.Wnode.Guid                 // ETW will create Guid
    properties_.Wnode.ClientContext = 1;   // Clock resolution to use when logging the timestamp for each event
                                           // 1 == query performance counter
    properties_.Wnode.Flags = 0;
    //properties_.BufferSize = 0;
    properties_.MinimumBuffers = 200;
    //properties_.MaximumBuffers = 0;
    //properties_.MaximumFileSize = 0;
    properties_.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    //properties_.FlushTimer = 0;
    //properties_.EnableFlags = 0;
    properties_.LogFileNameOffset = 0;
    properties_.LoggerNameOffset = offsetof(TraceSession, loggerName_);

    auto status = StartTraceA(&sessionHandle_, traceSessionName, &properties_);

    // If a session with this same name is already running, we exit it
    if (status == ERROR_ALREADY_EXISTS) {
            g_InspectorLogger->warn("warning: a trace session named {} is already running and it will be stopped",
                traceSessionName);

        status = ControlTraceA((TRACEHANDLE) 0, traceSessionName, &properties_, EVENT_TRACE_CONTROL_STOP);
        if (status == ERROR_SUCCESS) {
            status = StartTraceA(&sessionHandle_, traceSessionName, &properties_);
        }
    }

    // Report error if we failed to start a new session
    if (status != ERROR_SUCCESS) {
        g_InspectorLogger->error("error: failed to start trace session (error={}).", status);
        return false;
    }

    // Enable desired providers
    for (auto const& p : eventProvider_) {
        auto pGuid = &p.first;
        auto const& h = p.second;

        status = EnableTraceEx2(sessionHandle_, pGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, h.level_, h.matchAny_, h.matchAll_, 0, nullptr);
        if (status != ERROR_SUCCESS) {
            g_InspectorLogger->error("error: failed to enable provider");
            Finalize();
            return false;
        }
    }

    // Open the trace
    if (!OpenLogger(this, traceSessionName, true)) {
        Finalize();
        return false;
    }

    // Initialize state
    shouldStopProcessingEventsFn_ = shouldStopFn;
    eventsLostCount_ = 0;
    buffersLostCount_ = 0;

    return true;
}

void TraceSession::Finalize()
{
    ULONG status = ERROR_SUCCESS;

    if (traceHandle_ != INVALID_PROCESSTRACE_HANDLE) {
        status = CloseTrace(traceHandle_);
        traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
    }

    while (!eventProvider_.empty()) {
        RemoveProvider(eventProvider_.begin()->first);
    }
    while (!eventHandler_.empty()) {
        RemoveHandler(eventHandler_.begin()->first);
    }

    sessionHandle_ = 0;
}

void TraceSession::Stop()
{
    if (sessionHandle_ == 0) {
        return;
    }

    auto status = ControlTraceW(sessionHandle_, nullptr, &properties_, EVENT_TRACE_CONTROL_STOP);
    (void) status;

    sessionHandle_ = 0;
}

bool TraceSession::CheckLostReports(uint32_t* eventsLost, uint32_t* buffersLost)
{
    bool ret = false;

    if (sessionHandle_ != 0) {
        auto status = ControlTraceW(sessionHandle_, nullptr, &properties_, EVENT_TRACE_CONTROL_QUERY);
        switch (status) {
        case ERROR_SUCCESS:
            *eventsLost = properties_.EventsLost - eventsLostCount_;
            *buffersLost = properties_.RealTimeBuffersLost - buffersLostCount_;
            eventsLostCount_ = properties_.EventsLost;
            buffersLostCount_ = properties_.RealTimeBuffersLost;
            ret = *eventsLost + *buffersLost > 0;
            break;

        // The buffer &properties_ is too small to hold all the information for
        // the session.  If you don't need the session's property information
        // you can ignore this error.
        case ERROR_MORE_DATA:

        // The session is no longer running
        case ERROR_WMI_INSTANCE_NOT_FOUND:

            *eventsLost = 0;
            *buffersLost = 0;
            break;

        default:
            g_InspectorLogger->error("failed to query trace status {}", status);
            *eventsLost = 0;
            *buffersLost = 0;
            break;
        }
    }

    return ret;
}

