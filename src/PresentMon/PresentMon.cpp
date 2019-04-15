/*
Copyright 2017 Intel Corporation

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

#include <algorithm>
#include <shlwapi.h>

#include "TraceSession.hpp"
#include "PresentMon.hpp"
#include "Logger.hpp"
#include "Privilege.hpp"

#include "DataBuffer.h"
#include "timing.h"

namespace spd = spdlog;

#define MAX_CAPTURE_SAMPLES (60*86400*7)

extern bool CheckPriviliges();
void EtwConsumingThread(uint32_t TargetPid);
void PresentMon_Init(uint32_t TargetPid, PresentMonData& data);
void PresentMon_Update(PresentMonData& data, std::vector<std::shared_ptr<PresentEvent>>& presents, std::vector<std::shared_ptr<LateStageReprojectionEvent>>& lsrs, uint64_t perfFreq);
void PresentMon_Shutdown(PresentMonData& data, bool log_corrupted);
bool EtwThreadsShouldQuit();

std::thread g_EtwConsumingThread;
bool g_StopEtwThreads = true;
DataBuffer<EventScores> *g_ScoreBuffer = NULL;
double g_FirstTimestamp = 0;
uint64_t g_QpcFirst = 0;

extern "C" {
    BOOL WINAPI DllMain (HANDLE hInst, ULONG reason, LPVOID reserved) {
        switch (reason) {
            case DLL_PROCESS_DETACH:
                StopEventRecording ();
                break;
            default:
                break;
        }
        return TRUE;
    }
}

int SetLogLevel(int level) {
    int log_level = level;
    if (level > 6)
        log_level = 6;
    if (level < 0)
        log_level = 0;
    g_InspectorLogger->set_level(spd::level::level_enum(log_level));
    return STATUS_OK;
}

int StartEventRecording(int TargetPid, int arraySize) {
    if (arraySize <= 0 || arraySize > MAX_CAPTURE_SAMPLES) {
        g_InspectorLogger->error("Incorrect number of capture samples");
        return INVALID_ARGUMENTS_ERROR;
    }

    if (g_EtwConsumingThread.joinable())
        return EVENT_RECORDING_ALREADY_RUN_ERROR;
    if (!EtwThreadsShouldQuit())
        return EVENT_RECORDING_SHOULD_QUIT_ERROR;

    if (g_ScoreBuffer) {
        delete g_ScoreBuffer;
        g_ScoreBuffer = nullptr;
    }

    if (!CheckPriviliges())
        return PRIVILIGIES_ERROR;

    g_ScoreBuffer = new DataBuffer<EventScores>(arraySize);

    g_StopEtwThreads = false;
    g_EtwConsumingThread = std::thread(EtwConsumingThread, TargetPid);
    return STATUS_OK;
}

int StopEventRecording() {
    if (!g_EtwConsumingThread.joinable())
        return EVENT_RECORDING_IS_NOT_RUNNING_ERROR;
    if(g_StopEtwThreads)
        return EVENT_RECORDING_STOP_ERROR;

    g_StopEtwThreads = true;
    g_EtwConsumingThread.join();
    return STATUS_OK;
}

int GetCurrentData(int numSamples, EventScores *OutputBuf, double *timeOutputBuf, int *returnedSamples) {
    if (g_ScoreBuffer && OutputBuf && timeOutputBuf && returnedSamples) {
        size_t result = g_ScoreBuffer->getCurrentData(numSamples, timeOutputBuf, OutputBuf);
        (*returnedSamples) = int (result);
        return STATUS_OK;
    } else
        return INVALID_ARGUMENTS_ERROR;
}

int GetDataCount(int *result) {
    if (!g_ScoreBuffer)
    {
        g_InspectorLogger->error("buffer is uninitialized.");
        return INVALID_ARGUMENTS_ERROR;
    }
    if (!result)
    {
        g_InspectorLogger->error("output array is uninitialized.");
        return INVALID_ARGUMENTS_ERROR;
    }
    *result = int(g_ScoreBuffer->getDataCount());
    return STATUS_OK;
}

int GetData(int count, double *tsBuf, EventScores *Buf) {
    if (!g_ScoreBuffer)
    {
        g_InspectorLogger->error("buffer is uninitialized.");
        return INVALID_ARGUMENTS_ERROR;
    }
    if ((!tsBuf) || (!Buf))
    {
        g_InspectorLogger->error("output array is uninitialized.");
        return INVALID_ARGUMENTS_ERROR;
    }
    g_ScoreBuffer->getData(count, tsBuf, Buf);
    return STATUS_OK;
}

bool EtwThreadsShouldQuit()
{
    return g_StopEtwThreads;
}

template <typename Map, typename F>
static void map_erase_if(Map& m, F pred)
{
    typename Map::iterator i = m.begin();
    while ((i = std::find_if(i, m.end(), pred)) != m.end()) {
        m.erase(i++);
    }
}

static bool IsTargetProcess(uint32_t targetPid, uint32_t processId)
{
    // -capture_all
    if (targetPid == 0) {
        return true;
    }

    // -process_id
    if (targetPid != 0 && targetPid == processId) {
        return true;
    }

    return false;
}

static void StopProcess(PresentMonData& pm, std::map<uint32_t, ProcessInfo>::iterator it)
{
    pm.mProcessMap.erase(it);
}

static void StopProcess(PresentMonData& pm, uint32_t processId)
{
    auto it = pm.mProcessMap.find(processId);
    if (it != pm.mProcessMap.end()) {
        StopProcess(pm, it);
    }
}

static ProcessInfo* StartNewProcess(PresentMonData& pm, ProcessInfo* proc, uint32_t processId, std::string const& imageFileName, uint64_t now)
{
    proc->mModuleName = imageFileName;
    proc->mLastRefreshTicks = now;
    proc->mTargetProcess = IsTargetProcess(pm.mTargetPid, processId);

    if (!proc->mTargetProcess) {
        return nullptr;
    }
    return proc;
}

static ProcessInfo* StartProcess(PresentMonData& pm, uint32_t processId, std::string const& imageFileName, uint64_t now)
{
    auto it = pm.mProcessMap.find(processId);
    if (it != pm.mProcessMap.end()) {
        StopProcess(pm, it);
    }

    auto proc = &pm.mProcessMap.emplace(processId, ProcessInfo()).first->second;
    return StartNewProcess(pm, proc, processId, imageFileName, now);
}

static ProcessInfo* StartProcessIfNew(PresentMonData& pm, uint32_t processId, uint64_t now)
{
    auto it = pm.mProcessMap.find(processId);
    if (it != pm.mProcessMap.end()) {
        auto proc = &it->second;
        return proc->mTargetProcess ? proc : nullptr;
    }

    std::string imageFileName("<error>");
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (h) {
        char path[MAX_PATH] = "<error>";
        char* name = path;
        DWORD numChars = sizeof(path);
        if (QueryFullProcessImageNameA(h, 0, path, &numChars) == TRUE) {
            name = PathFindFileNameA(path);
        }
        imageFileName = name;
        CloseHandle(h);
    }

    auto proc = &pm.mProcessMap.emplace(processId, ProcessInfo()).first->second;
    return StartNewProcess(pm, proc, processId, imageFileName, now);
}

static bool UpdateProcessInfo_Realtime(PresentMonData& pm, ProcessInfo& info, uint64_t now, uint32_t thisPid)
{
    // Check periodically if the process has exited
    if (now - info.mLastRefreshTicks > 1000) {
        info.mLastRefreshTicks = now;

        auto running = false;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, thisPid);
        if (h) {
            char path[MAX_PATH] = "<error>";
            char* name = path;
            DWORD numChars = sizeof(path);
            if (QueryFullProcessImageNameA(h, 0, path, &numChars) == TRUE) {
                name = PathFindFileNameA(path);
            }
            if (info.mModuleName.compare(name) != 0) {
                // Image name changed, which means that our process exited and another
                // one started with the same PID.
                StartNewProcess(pm, &info, thisPid, name, now);
            }

            DWORD dwExitCode = 0;
            if (GetExitCodeProcess(h, &dwExitCode) == TRUE && dwExitCode == STILL_ACTIVE) {
                running = true;
            }
            CloseHandle(h);
        }

        if (!running) {
            return false;
        }
    }

    // remove chains without recent updates
    map_erase_if(info.mChainMap, [now](const std::pair<const uint64_t, SwapChainData>& entry) {
        return entry.second.IsStale(now);
    });

    return true;
}

void AddPresent(PresentMonData& pm, PresentEvent& p, uint64_t now, uint64_t perfFreq)
{
    const uint32_t appProcessId = p.ProcessId;
    auto proc = StartProcessIfNew(pm, appProcessId, now);
    if (proc == nullptr) {
        return; // process is not a target
    }

    auto& chain = proc->mChainMap[p.SwapChainAddress];
    chain.AddPresentToSwapChain(p);

    auto len = chain.mPresentHistory.size();
    auto displayedLen = chain.mDisplayedPresentHistory.size();
    if (len > 1) {
        auto& curr = chain.mPresentHistory[len - 1];
        auto& prev = chain.mPresentHistory[len - 2];
        double deltaMilliseconds = 1000 * double(curr.QpcTime - prev.QpcTime) / perfFreq;
        double deltaReady = curr.ReadyTime == 0 ? 0.0 : (1000 * double(curr.ReadyTime - curr.QpcTime) / perfFreq);
        double deltaDisplayed = curr.FinalState == PresentResult::Presented ? (1000 * double(curr.ScreenTime - curr.QpcTime) / perfFreq) : 0.0;
        double timeTakenMilliseconds = 1000 * double(curr.TimeTaken) / perfFreq;

        double timeSincePreviousDisplayed = 0.0;
        if (curr.FinalState == PresentResult::Presented && displayedLen > 1) {
            if (chain.mDisplayedPresentHistory[displayedLen - 1].QpcTime != curr.QpcTime){
                g_InspectorLogger->error("Incorrect QpcTime");
            }
            auto& prevDisplayed = chain.mDisplayedPresentHistory[displayedLen - 2];
            timeSincePreviousDisplayed = 1000 * double(curr.ScreenTime - prevDisplayed.ScreenTime) / perfFreq;
        }

        EventScores currentScores;
        currentScores.fps = 1000. / deltaMilliseconds;
        currentScores.flip = 1000. / timeSincePreviousDisplayed;
        currentScores.deltaReady = deltaReady;
        currentScores.deltaDisplayed = deltaDisplayed;
        currentScores.timeTaken = timeTakenMilliseconds;
        currentScores.screenTime = (double)curr.ScreenTime;

        if (!g_FirstTimestamp) {
            g_FirstTimestamp = getCurrentTime();
            g_QpcFirst = curr.QpcTime;
            g_ScoreBuffer->addData(g_FirstTimestamp, currentScores);
        }
        else {
            double timeSinceFirst = g_FirstTimestamp + double(curr.QpcTime - g_QpcFirst) / perfFreq;
            g_ScoreBuffer->addData(timeSinceFirst, currentScores);
        }
    }

    chain.UpdateSwapChainInfo(p, now, perfFreq);
}

void PresentMon_Init(uint32_t TargetPid, PresentMonData& pm)
{
    pm.mTargetPid = TargetPid;
    QueryPerformanceCounter((PLARGE_INTEGER)&pm.mStartupQpcTime);
}

void PresentMon_Update(PresentMonData& pm, std::vector<std::shared_ptr<PresentEvent>>& presents, std::vector<std::shared_ptr<LateStageReprojectionEvent>>& lsrs, uint64_t now, uint64_t perfFreq)
{
    // store the new presents into processes
    for (auto& p : presents)
    {
        AddPresent(pm, *p, now, perfFreq);
    }

    // Update realtime process info
    std::vector<std::map<uint32_t, ProcessInfo>::iterator> remove;
    for (auto ii = pm.mProcessMap.begin(), ie = pm.mProcessMap.end(); ii != ie; ++ii) {
        if (!UpdateProcessInfo_Realtime(pm, ii->second, now, ii->first)) {
            remove.emplace_back(ii);
        }
    }
    for (auto ii : remove) {
        StopProcess(pm, ii);
    }
}

void PresentMon_Shutdown(PresentMonData& pm)
{
    pm.mTargetPid = 0;

    pm.mProcessMap.clear();
}

static bool g_EtwProcessingThreadProcessing = false;
static void EtwProcessingThread(TraceSession *session)
{
    if (!g_EtwProcessingThreadProcessing)
    {
        g_InspectorLogger->error ("EtwProcessingThread error");
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    auto status = ProcessTrace(&session->traceHandle_, 1, NULL, NULL);
    (void) status; // check: _status == ERROR_SUCCESS;

    // Notify EtwConsumingThread that processing is complete
    g_EtwProcessingThreadProcessing = false;
}

void EtwConsumingThread(uint32_t targetPid)
{
    if (EtwThreadsShouldQuit()) {
        return;
    }

    PresentMonData data;
    PMTraceConsumer pmConsumer(false);
    MRTraceConsumer mrConsumer(false);

    TraceSession session;

    session.AddProviderAndHandler(DXGI_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0, 0, (EventHandlerFn) &HandleDXGIEvent, &pmConsumer);
    session.AddProviderAndHandler(D3D9_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0, 0, (EventHandlerFn) &HandleD3D9Event, &pmConsumer);
    session.AddProviderAndHandler(DXGKRNL_PROVIDER_GUID,   TRACE_LEVEL_INFORMATION, 1,      0, (EventHandlerFn) &HandleDXGKEvent,   &pmConsumer);
    session.AddProviderAndHandler(WIN32K_PROVIDER_GUID,    TRACE_LEVEL_INFORMATION, 0x1000, 0, (EventHandlerFn) &HandleWin32kEvent, &pmConsumer);
    session.AddProviderAndHandler(DWM_PROVIDER_GUID,       TRACE_LEVEL_VERBOSE,     0,      0, (EventHandlerFn) &HandleDWMEvent,    &pmConsumer);
    session.AddProviderAndHandler(Win7::DWM_PROVIDER_GUID, TRACE_LEVEL_VERBOSE, 0, 0, (EventHandlerFn) &HandleDWMEvent, &pmConsumer);
    session.AddProvider(Win7::DXGKRNL_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 1, 0);
    session.AddHandler(NT_PROCESS_EVENT_GUID,         (EventHandlerFn) &HandleNTProcessEvent,           &pmConsumer);
    session.AddHandler(Win7::DXGKBLT_GUID,            (EventHandlerFn) &Win7::HandleDxgkBlt,            &pmConsumer);
    session.AddHandler(Win7::DXGKFLIP_GUID,           (EventHandlerFn) &Win7::HandleDxgkFlip,           &pmConsumer);
    session.AddHandler(Win7::DXGKPRESENTHISTORY_GUID, (EventHandlerFn) &Win7::HandleDxgkPresentHistory, &pmConsumer);
    session.AddHandler(Win7::DXGKQUEUEPACKET_GUID,    (EventHandlerFn) &Win7::HandleDxgkQueuePacket,    &pmConsumer);
    session.AddHandler(Win7::DXGKVSYNCDPC_GUID,       (EventHandlerFn) &Win7::HandleDxgkVSyncDPC,       &pmConsumer);
    session.AddHandler(Win7::DXGKMMIOFLIP_GUID,       (EventHandlerFn) &Win7::HandleDxgkMMIOFlip,       &pmConsumer);


    session.InitializeRealtime("PresentMon", &EtwThreadsShouldQuit);

    {
        // Launch the ETW producer thread
        g_EtwProcessingThreadProcessing = true;
        std::thread etwProcessingThread(EtwProcessingThread, &session);

        // Consume / Update based on the ETW output
        {

            PresentMon_Init(targetPid, data);
            auto timerRunning = false;
            auto timerEnd = GetTickCount64();

            std::vector<std::shared_ptr<PresentEvent>> presents;
            std::vector<std::shared_ptr<LateStageReprojectionEvent>> lsrs;
            std::vector<NTProcessEvent> ntProcessEvents;

            uint32_t totalEventsLost = 0;
            uint32_t totalBuffersLost = 0;
            for (;;) {
                presents.clear();
                lsrs.clear();
                ntProcessEvents.clear();

                uint64_t now = GetTickCount64();

                // Dequeue any captured NTProcess events; if ImageFileName is
                // empty then the process stopped, otherwise it started.
                pmConsumer.DequeueProcessEvents(ntProcessEvents);
                for (auto ntProcessEvent : ntProcessEvents) {
                    if (!ntProcessEvent.ImageFileName.empty()) {
                        StartProcess(data, ntProcessEvent.ProcessId, ntProcessEvent.ImageFileName, now);
                    }
                }

                pmConsumer.DequeuePresents(presents);
                mrConsumer.DequeueLSRs(lsrs);

                auto doneProcessingEvents = g_EtwProcessingThreadProcessing ? false : true;
                PresentMon_Update(data, presents, lsrs, now, session.frequency_);

                for (auto ntProcessEvent : ntProcessEvents) {
                    if (ntProcessEvent.ImageFileName.empty()) {
                        StopProcess(data, ntProcessEvent.ProcessId);
                    }
                }

                uint32_t eventsLost = 0;
                uint32_t buffersLost = 0;
                if (session.CheckLostReports(&eventsLost, &buffersLost)) {
                    printf("Lost %u events, %u buffers.", eventsLost, buffersLost);

                    totalEventsLost += eventsLost;
                    totalBuffersLost += buffersLost;
                }

                if (timerRunning) {
                    if (GetTickCount64() >= timerEnd) {
                        timerRunning = false;
                    }
                }

                if (doneProcessingEvents) {
                    if (!EtwThreadsShouldQuit())
                        g_InspectorLogger->error("EtwThreadsShouldQuit returned non-zero exit code");
                    break;
                }

                Sleep(100);
            }

            PresentMon_Shutdown(data);
        }

        if (!etwProcessingThread.joinable()) {
            g_InspectorLogger->error("Thread is not joinable");
            session.Finalize();
        }
        if (g_EtwProcessingThreadProcessing) {
            g_InspectorLogger->error("incorrect g_EtwProcessingThreadProcessing");
        }
        etwProcessingThread.join();
    }

    session.Finalize();
}
