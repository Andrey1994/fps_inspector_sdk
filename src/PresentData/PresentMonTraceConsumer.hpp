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

#pragma once

#include <assert.h>
#include <deque>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <vector>
#include <windows.h>
#include <evntcons.h> // must include after windows.h

struct __declspec(uuid("{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}")) DXGI_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{802ec45a-1e99-4b83-9920-87c98277ba9d}")) DXGKRNL_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{8c416c79-d49b-4f01-a467-e56d3aa8234c}")) WIN32K_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{9e9bba3c-2e38-40cb-99f4-9e8281425164}")) DWM_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{783ACA0A-790E-4d7f-8451-AA850511C6B9}")) D3D9_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c}")) NT_PROCESS_EVENT_GUID_HOLDER;
static const auto DXGI_PROVIDER_GUID = __uuidof(DXGI_PROVIDER_GUID_HOLDER);
static const auto DXGKRNL_PROVIDER_GUID = __uuidof(DXGKRNL_PROVIDER_GUID_HOLDER);
static const auto WIN32K_PROVIDER_GUID = __uuidof(WIN32K_PROVIDER_GUID_HOLDER);
static const auto DWM_PROVIDER_GUID = __uuidof(DWM_PROVIDER_GUID_HOLDER);
static const auto D3D9_PROVIDER_GUID = __uuidof(D3D9_PROVIDER_GUID_HOLDER);
static const auto NT_PROCESS_EVENT_GUID = __uuidof(NT_PROCESS_EVENT_GUID_HOLDER);

// These are only for Win7 support
namespace Win7
{
    struct __declspec(uuid("{65cd4c8a-0848-4583-92a0-31c0fbaf00c0}")) DXGKRNL_PROVIDER_GUID_HOLDER;
    struct __declspec(uuid("{069f67f2-c380-4a65-8a61-071cd4a87275}")) DXGKBLT_GUID_HOLDER;
    struct __declspec(uuid("{22412531-670b-4cd3-81d1-e709c154ae3d}")) DXGKFLIP_GUID_HOLDER;
    struct __declspec(uuid("{c19f763a-c0c1-479d-9f74-22abfc3a5f0a}")) DXGKPRESENTHISTORY_GUID_HOLDER;
    struct __declspec(uuid("{295e0d8e-51ec-43b8-9cc6-9f79331d27d6}")) DXGKQUEUEPACKET_GUID_HOLDER;
    struct __declspec(uuid("{5ccf1378-6b2c-4c0f-bd56-8eeb9e4c5c77}")) DXGKVSYNCDPC_GUID_HOLDER;
    struct __declspec(uuid("{547820fe-5666-4b41-93dc-6cfd5dea28cc}")) DXGKMMIOFLIP_GUID_HOLDER;
    struct __declspec(uuid("{8c9dd1ad-e6e5-4b07-b455-684a9d879900}")) DWM_PROVIDER_GUID_HOLDER;
    static const auto DXGKRNL_PROVIDER_GUID = __uuidof(DXGKRNL_PROVIDER_GUID_HOLDER);
    static const auto DXGKBLT_GUID = __uuidof(DXGKBLT_GUID_HOLDER);
    static const auto DXGKFLIP_GUID = __uuidof(DXGKFLIP_GUID_HOLDER);
    static const auto DXGKPRESENTHISTORY_GUID = __uuidof(DXGKPRESENTHISTORY_GUID_HOLDER);
    static const auto DXGKQUEUEPACKET_GUID = __uuidof(DXGKQUEUEPACKET_GUID_HOLDER);
    static const auto DXGKVSYNCDPC_GUID = __uuidof(DXGKVSYNCDPC_GUID_HOLDER);
    static const auto DXGKMMIOFLIP_GUID = __uuidof(DXGKMMIOFLIP_GUID_HOLDER);
    static const auto DWM_PROVIDER_GUID = __uuidof(DWM_PROVIDER_GUID_HOLDER);
};

// Forward-declare structs that will be used by both modern and legacy dxgkrnl events.
struct DxgkBltEventArgs;
struct DxgkFlipEventArgs;
struct DxgkQueueSubmitEventArgs;
struct DxgkQueueCompleteEventArgs;
struct DxgkMMIOFlipEventArgs;
struct DxgkSyncDPCEventArgs;
struct DxgkSubmitPresentHistoryEventArgs;
struct DxgkPropagatePresentHistoryEventArgs;

template <typename mutex_t> std::unique_lock<mutex_t> scoped_lock(mutex_t &m)
{
    return std::unique_lock<mutex_t>(m);
}

enum class PresentMode
{
    Unknown,
    Hardware_Legacy_Flip,
    Hardware_Legacy_Copy_To_Front_Buffer,
    Hardware_Direct_Flip,
    Hardware_Independent_Flip,
    Composed_Flip,
    Composed_Copy_GPU_GDI,
    Composed_Copy_CPU_GDI,
    Composed_Composition_Atlas,
    Hardware_Composed_Independent_Flip,
};

enum class PresentResult
{
    Unknown, Presented, Discarded, Error
};

enum class Runtime
{
    DXGI, D3D9, Other
};

struct NTProcessEvent {
    uint32_t ProcessId;
    std::string ImageFileName;  // If ImageFileName.empty(), then event is that process ending
};

struct PresentEvent {
    // Available from DXGI Present
    uint64_t QpcTime;
    uint64_t SwapChainAddress;
    int32_t SyncInterval;
    uint32_t PresentFlags;
    uint32_t ProcessId;

    PresentMode PresentMode;
    bool SupportsTearing;
    bool MMIO;
    bool SeenDxgkPresent;
    bool SeenWin32KEvents;
    bool WasBatched;
    bool DwmNotified;

    Runtime Runtime;

    // Time spent in DXGI Present call
    uint64_t TimeTaken;

    // Timestamp of "ready" state (GPU work completed)
    uint64_t ReadyTime;

    // Timestamp of "complete" state (data on screen or discarded)
    uint64_t ScreenTime;
    PresentResult FinalState;
    uint32_t PlaneIndex;

    // Additional transient state
    uint32_t QueueSubmitSequence;
    uint32_t RuntimeThread;
    uint64_t Hwnd;
    uint64_t TokenPtr;
    std::deque<std::shared_ptr<PresentEvent>> DependentPresents;
    bool Completed;

    PresentEvent(EVENT_HEADER const& hdr, ::Runtime runtime);
    ~PresentEvent();
};

struct PMTraceConsumer
{
    PMTraceConsumer(bool simple) : mSimpleMode(simple) { }
    ~PMTraceConsumer();

    bool mSimpleMode;

    std::mutex mMutex;
    // A set of presents that are "completed":
    // They progressed as far as they can through the pipeline before being either discarded or hitting the screen.
    // These will be handed off to the consumer thread.
    std::vector<std::shared_ptr<PresentEvent>> mCompletedPresents;

    // A high-level description of the sequence of events for each present type, ignoring runtime end:
    // Hardware Legacy Flip:
    //   Runtime PresentStart -> Flip (by thread/process, for classification) -> QueueSubmit (by thread, for submit sequence) ->
    //    MMIOFlip (by submit sequence, for ready time and immediate flags) [-> VSyncDPC (by submit sequence, for screen time)]
    // Composed Flip (FLIP_SEQUENTIAL, FLIP_DISCARD, FlipEx),
    //   Runtime PresentStart -> TokenCompositionSurfaceObject (by thread/process, for classification and token key) ->
    //    PresentHistoryDetailed (by thread, for token ptr) -> QueueSubmit (by thread, for submit sequence) ->
    //    PropagatePresentHistory (by token ptr, for ready time) and TokenStateChanged (by token key, for discard status and screen time)
    // Hardware Direct Flip,
    //   N/A, not currently uniquely detectable (follows the same path as composed_flip)
    // Hardware Independent Flip,
    //   Follows composed flip, TokenStateChanged indicates IndependentFlip -> MMIOFlip (by submit sequence, for immediate flags) [->
    //   VSyncDPC (by submit sequence, for screen time), or HSync DPC (if system is Windows 10 April 2018, and using integrated graphics)]
    // Hardware Composed Independent Flip,
    //   Identical to IndependentFlip, but MMIOFlipMPO is received instead
    // Composed Copy with GPU GDI (a.k.a. Win7 Blit),
    //   Runtime PresentStart -> Blt (by thread/process, for classification) -> PresentHistoryDetailed (by thread, for token ptr and classification) ->
    //    DxgKrnl Present (by thread, for hWnd) -> PropagatePresentHistory (by token ptr, for ready time) ->
    //    DWM UpdateWindow (by hWnd, marks hWnd active for composition) -> DWM Present (consumes most recent present per hWnd, marks DWM thread ID) ->
    //    A fullscreen present is issued by DWM, and when it completes, this present is on screen
    // Hardware Copy to front buffer,
    //   Runtime PresentStart -> Blt (by thread/process, for classification) -> QueueSubmit (by thread, for submit sequence) ->
    //    QueueComplete (by submit sequence, indicates ready and screen time)
    //    Distinction between FS and windowed blt is done by LACK of other events
    // Composed Copy with CPU GDI (a.k.a. Vista Blit),
    //   Runtime PresentStart -> Blt (by thread/process, for classification) -> SubmitPresentHistory (by thread, for token ptr, legacy blit token, and classification) ->
    //    PropagatePresentHsitory (by token ptr, for ready time) -> DWM FlipChain (by legacy blit token, for hWnd and marks hWnd active for composition) ->
    //    Follows the Windowed_Blit path for tracking to screen
    // Composed Composition Atlas (DirectComposition),
    //   SubmitPresentHistory (use model field for classification, get token ptr) -> PropagatePresentHistory (by token ptr) ->
    //    Assume DWM will compose this buffer on next present (missing InFrame event), follow windowed blit paths to screen time

    // For each process, stores each present started. Used for present batching
    std::map<uint32_t, std::map<uint64_t, std::shared_ptr<PresentEvent>>> mPresentsByProcess;

    // For each (process, swapchain) pair, stores each present started. Used to ensure consumer sees presents targeting the same swapchain in the order they were submitted.
    typedef std::tuple<uint32_t, uint64_t> ProcessAndSwapChainKey;
    std::map<ProcessAndSwapChainKey, std::deque<std::shared_ptr<PresentEvent>>> mPresentsByProcessAndSwapChain;

    // Presents in the process of being submitted
    // The first map contains a single present that is currently in-between a set of expected events on the same thread:
    //   (e.g. DXGI_Present_Start/DXGI_Present_Stop, or Flip/QueueSubmit)
    // Used for mapping from runtime events to future events, and thread map used extensively for correlating kernel events
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mPresentByThreadId;

    // Maps from queue packet submit sequence
    // Used for Flip -> MMIOFlip -> VSyncDPC for FS, for PresentHistoryToken -> MMIOFlip -> VSyncDPC for iFlip,
    // and for Blit Submission -> Blit completion for FS Blit
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mPresentsBySubmitSequence;

    // Win32K present history tokens are uniquely identified by (composition surface pointer, present count, bind id)
    // Using a tuple instead of named struct simply to have auto-generated comparison operators
    // These tokens are used for "flip model" presents (windowed flip, dFlip, iFlip) only
    typedef std::tuple<uint64_t, uint64_t, uint64_t> Win32KPresentHistoryTokenKey;
    std::map<Win32KPresentHistoryTokenKey, std::shared_ptr<PresentEvent>> mWin32KPresentHistoryTokens;

    // DxgKrnl present history tokens are uniquely identified by a single pointer
    // These are used for all types of windowed presents to track a "ready" time
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mDxgKrnlPresentHistoryTokens;

    // For blt presents on Win7, it's not possible to distinguish between DWM-off or fullscreen blts, and the DWM-on blt to redirection bitmaps.
    // The best we can do is make the distinction based on the next packet submitted to the context. If it's not a PHT, it's not going to DWM.
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mBltsByDxgContext;

    // Present by window, used for determining superceding presents
    // For windowed blit presents, when DWM issues a present event, we choose the most recent event as the one that will make it to screen
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mPresentByWindow;

    // Presents that will be completed by DWM's next present
    std::deque<std::shared_ptr<PresentEvent>> mPresentsWaitingForDWM;
    // Used to understand that a flip event is coming from the DWM
    uint32_t DwmPresentThreadId = 0;

    // Yet another unique way of tracking present history tokens, this time from DxgKrnl -> DWM, only for legacy blit
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mPresentsByLegacyBlitToken;

    // Process events
    std::mutex mNTProcessEventMutex;
    std::vector<NTProcessEvent> mNTProcessEvents;

    bool DequeueProcessEvents(std::vector<NTProcessEvent>& outProcessEvents)
    {
        if (mNTProcessEvents.empty()) {
            return false;
        }

        auto lock = scoped_lock(mNTProcessEventMutex);
        outProcessEvents.swap(mNTProcessEvents);
        return true;
    }

    bool DequeuePresents(std::vector<std::shared_ptr<PresentEvent>>& outPresents)
    {
        if (mCompletedPresents.empty()) {
            return false;
        }

        auto lock = scoped_lock(mMutex);
        outPresents.swap(mCompletedPresents);
        return true;
    }

    void HandleDxgkBlt(DxgkBltEventArgs& args);
    void HandleDxgkFlip(DxgkFlipEventArgs& args);
    void HandleDxgkQueueSubmit(DxgkQueueSubmitEventArgs& args);
    void HandleDxgkQueueComplete(DxgkQueueCompleteEventArgs& args);
    void HandleDxgkMMIOFlip(DxgkMMIOFlipEventArgs& args);
    void HandleDxgkSyncDPC(DxgkSyncDPCEventArgs& args);
    void HandleDxgkSubmitPresentHistoryEventArgs(DxgkSubmitPresentHistoryEventArgs& args);
    void HandleDxgkPropagatePresentHistoryEventArgs(DxgkPropagatePresentHistoryEventArgs& args);

    void CompletePresent(std::shared_ptr<PresentEvent> p);
    decltype(mPresentByThreadId.begin()) FindOrCreatePresent(EVENT_HEADER const& hdr);
    void RuntimePresentStart(PresentEvent &event);
    void RuntimePresentStop(EVENT_HEADER const& hdr, bool AllowPresentBatching);
};

void HandleNTProcessEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
void HandleDXGIEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
void HandleD3D9Event(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
void HandleDXGKEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
void HandleWin32kEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
void HandleDWMEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);

// These are only for Win7 support
namespace Win7
{
    void HandleDxgkBlt(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
    void HandleDxgkFlip(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
    void HandleDxgkPresentHistory(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
    void HandleDxgkQueuePacket(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
    void HandleDxgkVSyncDPC(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
    void HandleDxgkMMIOFlip(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer);
}
