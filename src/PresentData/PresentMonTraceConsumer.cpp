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

#define NOMINMAX
#include <algorithm>
#include <d3d9.h>
#include <dxgi.h>

#include "PresentMonTraceConsumer.hpp"
#include "TraceConsumer.hpp"
#include "DxgkrnlEventStructs.hpp"

PresentEvent::PresentEvent(EVENT_HEADER const& hdr, ::Runtime runtime)
    : QpcTime(*(uint64_t*) &hdr.TimeStamp)
    , SwapChainAddress(0)
    , SyncInterval(-1)
    , PresentFlags(0)
    , ProcessId(hdr.ProcessId)
    , PresentMode(PresentMode::Unknown)
    , SupportsTearing(false)
    , MMIO(false)
    , SeenDxgkPresent(false)
    , SeenWin32KEvents(false)
    , WasBatched(false)
    , DwmNotified(false)
    , Runtime(runtime)
    , TimeTaken(0)
    , ReadyTime(0)
    , ScreenTime(0)
    , FinalState(PresentResult::Unknown)
    , PlaneIndex(0)
    , QueueSubmitSequence(0)
    , RuntimeThread(hdr.ThreadId)
    , Hwnd(0)
    , TokenPtr(0)
    , Completed(false)
{
}

#ifndef NDEBUG
static bool gPresentMonTraceConsumer_Exiting = false;
#endif

PresentEvent::~PresentEvent()
{
    assert(Completed || gPresentMonTraceConsumer_Exiting);
}

PMTraceConsumer::~PMTraceConsumer()
{
#ifndef NDEBUG
    gPresentMonTraceConsumer_Exiting = true;
#endif
}

void HandleDXGIEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    enum {
        DXGIPresent_Start = 42,
        DXGIPresent_Stop,
        DXGIPresentMPO_Start = 55,
        DXGIPresentMPO_Stop = 56,
    };

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id)
    {
    case DXGIPresent_Start:
    case DXGIPresentMPO_Start:
    {
        PresentEvent event(hdr, Runtime::DXGI);
        GetEventData(pEventRecord, L"pIDXGISwapChain", &event.SwapChainAddress);
        GetEventData(pEventRecord, L"Flags",           &event.PresentFlags);
        GetEventData(pEventRecord, L"SyncInterval",    &event.SyncInterval);

        pmConsumer->RuntimePresentStart(event);
        break;
    }
    case DXGIPresent_Stop:
    case DXGIPresentMPO_Stop:
    {
        auto result = GetEventData<uint32_t>(pEventRecord, L"Result");
        bool AllowBatching = SUCCEEDED(result) && result != DXGI_STATUS_OCCLUDED && result != DXGI_STATUS_MODE_CHANGE_IN_PROGRESS && result != DXGI_STATUS_NO_DESKTOP_ACCESS;
        pmConsumer->RuntimePresentStop(hdr, AllowBatching);
        break;
    }
    }
}

void PMTraceConsumer::HandleDxgkBlt(DxgkBltEventArgs& args)
{
    auto eventIter = FindOrCreatePresent(*args.pEventHeader);

    // Check if we might have retrieved a 'stuck' present from a previous frame.
    // If the present mode isn't unknown at this point, we've already seen this present progress further
    if (eventIter->second->PresentMode != PresentMode::Unknown) {
        mPresentByThreadId.erase(eventIter);
        eventIter = FindOrCreatePresent(*args.pEventHeader);
    }

    // This could be one of several types of presents. Further events will clarify.
    // For now, assume that this is a blt straight into a surface which is already on-screen.
    eventIter->second->PresentMode = args.Present ?
        PresentMode::Composed_Copy_CPU_GDI : PresentMode::Hardware_Legacy_Copy_To_Front_Buffer;
    eventIter->second->SupportsTearing = !args.Present;
    eventIter->second->Hwnd = args.Hwnd;
}

void PMTraceConsumer::HandleDxgkFlip(DxgkFlipEventArgs& args)
{
    // A flip event is emitted during fullscreen present submission.
    // Afterwards, expect an MMIOFlip packet on the same thread, used
    // to trace the flip to screen.
    auto eventIter = FindOrCreatePresent(*args.pEventHeader);

    // Check if we might have retrieved a 'stuck' present from a previous frame.
    // The only events that we can expect before a Flip/FlipMPO are a runtime present start, or a previous FlipMPO.
    if (eventIter->second->QueueSubmitSequence != 0 || eventIter->second->SeenDxgkPresent) {
        // It's already progressed further but didn't complete, ignore it and create a new one.
        mPresentByThreadId.erase(eventIter);
        eventIter = FindOrCreatePresent(*args.pEventHeader);
    }

    if (eventIter->second->PresentMode != PresentMode::Unknown) {
        // For MPO, N events may be issued, but we only care about the first
        return;
    }

    eventIter->second->MMIO = args.MMIO;
    eventIter->second->PresentMode = PresentMode::Hardware_Legacy_Flip;

    if (eventIter->second->SyncInterval == -1) {
        eventIter->second->SyncInterval = args.FlipInterval;
    }
    if (!args.MMIO) {
        eventIter->second->SupportsTearing = args.FlipInterval == 0;
    }

    // If this is the DWM thread, piggyback these pending presents on our fullscreen present
    if (args.pEventHeader->ThreadId == DwmPresentThreadId) {
        std::swap(eventIter->second->DependentPresents, mPresentsWaitingForDWM);
        DwmPresentThreadId = 0;
    }
}

void PMTraceConsumer::HandleDxgkQueueSubmit(DxgkQueueSubmitEventArgs& args)
{
    // If we know we're never going to get a DxgkPresent event for a given blt, then let's try to determine if it's a redirected blt or not.
    // If it's redirected, then the SubmitPresentHistory event should've been emitted before submitting anything else to the same context,
    // and therefore we'll know it's a redirected present by this point. If it's still non-redirected, then treat this as if it was a DxgkPresent
    // event - the present will be considered completed once its work is done, or if the work is already done, complete it now.
    if (!args.SupportsDxgkPresentEvent) {
        auto eventIter = mBltsByDxgContext.find(args.Context);
        if (eventIter != mBltsByDxgContext.end()) {
            if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer) {
                eventIter->second->SeenDxgkPresent = true;
                if (eventIter->second->ScreenTime != 0) {
                    CompletePresent(eventIter->second);
                }
            }
            mBltsByDxgContext.erase(eventIter);
        }
    }

    // This event is emitted after a flip/blt/PHT event, and may be the only way
    // to trace completion of the present.
    if (args.PacketType == DxgKrnl_QueueSubmit_Type::MMIOFlip ||
        args.PacketType == DxgKrnl_QueueSubmit_Type::Software ||
        args.Present) {
        auto eventIter = mPresentByThreadId.find(args.pEventHeader->ThreadId);
        if (eventIter == mPresentByThreadId.end() || eventIter->second->QueueSubmitSequence != 0) {
            return;
        }

        eventIter->second->QueueSubmitSequence = args.SubmitSequence;
        mPresentsBySubmitSequence.emplace(args.SubmitSequence, eventIter->second);

        if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer && !args.SupportsDxgkPresentEvent) {
            mBltsByDxgContext[args.Context] = eventIter->second;
        }
    }
}

void PMTraceConsumer::HandleDxgkQueueComplete(DxgkQueueCompleteEventArgs& args)
{
    auto eventIter = mPresentsBySubmitSequence.find(args.SubmitSequence);
    if (eventIter == mPresentsBySubmitSequence.end()) {
        return;
    }

    auto pEvent = eventIter->second;

    uint64_t EventTime = *(uint64_t*)&args.pEventHeader->TimeStamp;

    if (pEvent->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer ||
        (pEvent->PresentMode == PresentMode::Hardware_Legacy_Flip && !pEvent->MMIO)) {
        pEvent->ScreenTime = pEvent->ReadyTime = EventTime;
        pEvent->FinalState = PresentResult::Presented;

        // Sometimes, the queue packets associated with a present will complete before the DxgKrnl present event is fired
        // In this case, for blit presents, we have no way to differentiate between fullscreen and windowed blits
        // So, defer the completion of this present until we know all events have been fired
        if (pEvent->SeenDxgkPresent || pEvent->PresentMode != PresentMode::Hardware_Legacy_Copy_To_Front_Buffer) {
            CompletePresent(pEvent);
        }
    }
}

void PMTraceConsumer::HandleDxgkMMIOFlip(DxgkMMIOFlipEventArgs& args)
{
    // An MMIOFlip event is emitted when an MMIOFlip packet is dequeued.
    // This corresponds to all GPU work prior to the flip being completed
    // (i.e. present "ready")
    // It also is emitted when an independent flip PHT is dequed,
    // and will tell us whether the present is immediate or vsync.
    auto eventIter = mPresentsBySubmitSequence.find(args.FlipSubmitSequence);
    if (eventIter == mPresentsBySubmitSequence.end()) {
        return;
    }

    uint64_t EventTime = *(uint64_t*)&args.pEventHeader->TimeStamp;
    eventIter->second->ReadyTime = EventTime;

    if (eventIter->second->PresentMode == PresentMode::Composed_Flip) {
        eventIter->second->PresentMode = PresentMode::Hardware_Independent_Flip;
    }

    if (args.Flags & DxgKrnl_MMIOFlip_Flags::FlipImmediate) {
        eventIter->second->FinalState = PresentResult::Presented;
        eventIter->second->ScreenTime = EventTime;
        eventIter->second->SupportsTearing = true;
        if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Flip) {
            CompletePresent(eventIter->second);
        }
    }
}

void PMTraceConsumer::HandleDxgkSyncDPC(DxgkSyncDPCEventArgs& args)
{
    // The VSyncDPC/HSyncDPC contains a field telling us what flipped to screen.
    // This is the way to track completion of a fullscreen present.
    auto eventIter = mPresentsBySubmitSequence.find(args.FlipSubmitSequence);
    if (eventIter == mPresentsBySubmitSequence.end()) {
        return;
    }

    uint64_t EventTime = *(uint64_t*)&args.pEventHeader->TimeStamp;
    eventIter->second->ScreenTime = EventTime;
    eventIter->second->FinalState = PresentResult::Presented;
    if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Flip) {
        CompletePresent(eventIter->second);
    }
}

void PMTraceConsumer::HandleDxgkSubmitPresentHistoryEventArgs(DxgkSubmitPresentHistoryEventArgs& args)
{
    // These events are emitted during submission of all types of windowed presents while DWM is on.
    // It gives us up to two different types of keys to correlate further.
    auto eventIter = FindOrCreatePresent(*args.pEventHeader);

    // Check if we might have retrieved a 'stuck' present from a previous frame.
    if (eventIter->second->TokenPtr != 0) {
        // It's already progressed further but didn't complete, ignore it and create a new one.
        mPresentByThreadId.erase(eventIter);
        eventIter = FindOrCreatePresent(*args.pEventHeader);
    }

    eventIter->second->ReadyTime = eventIter->second->ScreenTime = 0;
    eventIter->second->SupportsTearing = false;
    eventIter->second->FinalState = PresentResult::Unknown;
    eventIter->second->TokenPtr = args.Token;

    if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer)
    {
        eventIter->second->PresentMode = PresentMode::Composed_Copy_GPU_GDI;
        assert(args.KnownPresentMode == PresentMode::Unknown ||
               args.KnownPresentMode == PresentMode::Composed_Copy_GPU_GDI);
    }
    else if (eventIter->second->PresentMode == PresentMode::Unknown)
    {
        if (args.KnownPresentMode == PresentMode::Composed_Composition_Atlas) {
            eventIter->second->PresentMode = PresentMode::Composed_Composition_Atlas;
        }
        else {
            // When there's no Win32K events, we'll assume PHTs that aren't after a blt, and aren't composition tokens
            // are flip tokens and that they're displayed. There are no Win32K events on Win7, and they might not be
            // present in some traces - don't let presents get stuck/dropped just because we can't track them perfectly.
            assert(!eventIter->second->SeenWin32KEvents);
            eventIter->second->PresentMode = PresentMode::Composed_Flip;
        }
    }
    else if (eventIter->second->PresentMode == PresentMode::Composed_Copy_CPU_GDI) {
        if (args.TokenData == 0) {
            // This is the best we can do, we won't be able to tell how many frames are actually displayed.
            mPresentsWaitingForDWM.emplace_back(eventIter->second);
        }
        else {
            mPresentsByLegacyBlitToken[args.TokenData] = eventIter->second;
        }
    }
    mDxgKrnlPresentHistoryTokens[args.Token] = eventIter->second;
}

void PMTraceConsumer::HandleDxgkPropagatePresentHistoryEventArgs(DxgkPropagatePresentHistoryEventArgs& args)
{
    // This event is emitted when a token is being handed off to DWM, and is a good way to indicate a ready state
    auto eventIter = mDxgKrnlPresentHistoryTokens.find(args.Token);
    if (eventIter == mDxgKrnlPresentHistoryTokens.end()) {
        return;
    }

    uint64_t EventTime = *(uint64_t*)&args.pEventHeader->TimeStamp;
    auto& ReadyTime = eventIter->second->ReadyTime;
    ReadyTime = (ReadyTime == 0 ?
        EventTime : std::min(ReadyTime, EventTime));

    if (eventIter->second->PresentMode == PresentMode::Composed_Composition_Atlas ||
        (eventIter->second->PresentMode == PresentMode::Composed_Flip && !eventIter->second->SeenWin32KEvents)) {
        mPresentsWaitingForDWM.emplace_back(eventIter->second);
    }

    if (eventIter->second->PresentMode == PresentMode::Composed_Copy_GPU_GDI) {
        // Manipulate the map here
        // When DWM is ready to present, we'll query for the most recent blt targeting this window and take it out of the map
        mPresentByWindow[eventIter->second->Hwnd] = eventIter->second;
    }

    mDxgKrnlPresentHistoryTokens.erase(eventIter);
}

static PresentMode D3DKMT_TokenModel_ToPresentMode(D3DKMT_PRESENT_MODEL Model)
{
    switch (Model)
    {
    case D3DKMT_PM_REDIRECTED_BLT:
        return PresentMode::Composed_Copy_GPU_GDI;
    case D3DKMT_PM_REDIRECTED_VISTABLT:
        return PresentMode::Composed_Copy_CPU_GDI;
    case D3DKMT_PM_REDIRECTED_FLIP:
        return PresentMode::Composed_Flip;
    case D3DKMT_PM_REDIRECTED_COMPOSITION:
        return PresentMode::Composed_Composition_Atlas;
    }
    return PresentMode::Unknown;
}

void HandleDXGKEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    enum {
        DxgKrnl_Flip = 168,
        DxgKrnl_FlipMPO = 252,
        DxgKrnl_QueueSubmit = 178,
        DxgKrnl_QueueComplete = 180,
        DxgKrnl_MMIOFlip = 116,
        DxgKrnl_MMIOFlipMPO = 259,
        DxgKrnl_HSyncDPC = 382,
        DxgKrnl_VSyncDPC = 17,
        DxgKrnl_Present = 184,
        DxgKrnl_PresentHistoryDetailed = 215,
        DxgKrnl_SubmitPresentHistory = 171,
        DxgKrnl_PropagatePresentHistory = 172,
        DxgKrnl_Blit = 166,
    };

    auto const& hdr = pEventRecord->EventHeader;

    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;

    switch (hdr.EventDescriptor.Id)
    {
    case DxgKrnl_Flip:
    case DxgKrnl_FlipMPO:
    {
        DxgkFlipEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.FlipInterval = -1;
        if (hdr.EventDescriptor.Id == DxgKrnl_Flip) {
            Args.FlipInterval = GetEventData<uint32_t>(pEventRecord, L"FlipInterval");
            Args.MMIO = GetEventData<BOOL>(pEventRecord, L"MMIOFlip") != 0;
        }
        else {
            Args.MMIO = true; // All MPO flips are MMIO
        }
        pmConsumer->HandleDxgkFlip(Args);
        break;
    }
    case DxgKrnl_QueueSubmit:
    {
        DxgkQueueSubmitEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.PacketType = GetEventData<DxgKrnl_QueueSubmit_Type>(pEventRecord, L"PacketType");
        Args.SubmitSequence = GetEventData<uint32_t>(pEventRecord, L"SubmitSequence");
        Args.Present = GetEventData<BOOL>(pEventRecord, L"bPresent") != 0;
        Args.Context = GetEventData<uint64_t>(pEventRecord, L"hContext");
        Args.SupportsDxgkPresentEvent = true;
        pmConsumer->HandleDxgkQueueSubmit(Args);
        break;
    }
    case DxgKrnl_QueueComplete:
    {
        DxgkQueueCompleteEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.SubmitSequence = GetEventData<uint32_t>(pEventRecord, L"SubmitSequence");
        pmConsumer->HandleDxgkQueueComplete(Args);
        break;
    }
    case DxgKrnl_MMIOFlip:
    {
        DxgkMMIOFlipEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.FlipSubmitSequence = GetEventData<uint32_t>(pEventRecord, L"FlipSubmitSequence");
        Args.Flags = GetEventData<DxgKrnl_MMIOFlip_Flags>(pEventRecord, L"Flags");
        pmConsumer->HandleDxgkMMIOFlip(Args);
        break;
    }
    case DxgKrnl_MMIOFlipMPO:
    {
        // See above for more info about this packet.
        // Note: Event does not exist on Win7
        auto FlipFenceId = GetEventData<uint64_t>(pEventRecord, L"FlipSubmitSequence");
        uint32_t FlipSubmitSequence = (uint32_t)(FlipFenceId >> 32u);

        auto eventIter = pmConsumer->mPresentsBySubmitSequence.find(FlipSubmitSequence);
        if (eventIter == pmConsumer->mPresentsBySubmitSequence.end()) {
            return;
        }

        // Avoid double-marking a single present packet coming from the MPO API
        if (eventIter->second->ReadyTime == 0) {
            eventIter->second->ReadyTime = EventTime;
            eventIter->second->PlaneIndex = GetEventData<uint32_t>(pEventRecord, L"LayerIndex");
        }

        if (eventIter->second->PresentMode == PresentMode::Hardware_Independent_Flip ||
            eventIter->second->PresentMode == PresentMode::Composed_Flip) {
            eventIter->second->PresentMode = PresentMode::Hardware_Composed_Independent_Flip;
        }

        if (hdr.EventDescriptor.Version >= 2)
        {
            enum class DxgKrnl_MMIOFlipMPO_FlipEntryStatus {
                FlipWaitVSync = 5,
                FlipWaitComplete = 11,
                FlipWaitHSync = 15,
                // There are others, but they're more complicated to deal with.
            };

            auto FlipEntryStatusAfterFlip = GetEventData<DxgKrnl_MMIOFlipMPO_FlipEntryStatus>(pEventRecord, L"FlipEntryStatusAfterFlip");
            if (FlipEntryStatusAfterFlip != DxgKrnl_MMIOFlipMPO_FlipEntryStatus::FlipWaitVSync &&
                FlipEntryStatusAfterFlip != DxgKrnl_MMIOFlipMPO_FlipEntryStatus::FlipWaitHSync) {
                eventIter->second->FinalState = PresentResult::Presented;
                eventIter->second->SupportsTearing = true;
                if (FlipEntryStatusAfterFlip == DxgKrnl_MMIOFlipMPO_FlipEntryStatus::FlipWaitComplete) {
                    eventIter->second->ScreenTime = EventTime;
                }
                if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Flip) {
                    pmConsumer->CompletePresent(eventIter->second);
                }
            }
        }

        break;
    }
    case DxgKrnl_HSyncDPC:
    {
        // Used for Hardware Independent Flip, and Hardware Composed Flip to signal flipping to the screen 
        // on Windows 10 build numbers 17134 and up where the associated display is connected to 
        // integrated graphics
        // MMIOFlipMPO [EntryStatus:FlipWaitHSync] ->HSync DPC

        auto FlipCount = GetEventData<uint32_t>(pEventRecord, L"FlipEntryCount");
        for (uint32_t i = 0; i < FlipCount; i++)
        {
            auto FlipId = GetEventDataFromArray<uint64_t>(pEventRecord, L"FlipSubmitSequence", i);
            DxgkSyncDPCEventArgs Args = {};
            Args.pEventHeader = &hdr;
            Args.FlipSubmitSequence = (uint32_t)(FlipId >> 32u);
            pmConsumer->HandleDxgkSyncDPC(Args);
        }
        break;
    }
    case DxgKrnl_VSyncDPC:
    {
        auto FlipFenceId = GetEventData<uint64_t>(pEventRecord, L"FlipFenceId");

        DxgkSyncDPCEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.FlipSubmitSequence = (uint32_t)(FlipFenceId >> 32u);
        pmConsumer->HandleDxgkSyncDPC(Args);
        break;
    }
    case DxgKrnl_Present:
    {
        // This event is emitted at the end of the kernel present, before returning.
        // The presence of this event is used with blt presents to indicate that no
        // PHT is to be expected.
        auto eventIter = pmConsumer->mPresentByThreadId.find(hdr.ThreadId);
        if (eventIter == pmConsumer->mPresentByThreadId.end()) {
            return;
        }

        eventIter->second->SeenDxgkPresent = true;
        if (eventIter->second->Hwnd == 0) {
            eventIter->second->Hwnd = GetEventData<uint64_t>(pEventRecord, L"hWindow");
        }

        if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer &&
            eventIter->second->ScreenTime != 0) {
            // This is a fullscreen or DWM-off blit where all work associated was already done, so it's on-screen
            // It was deferred to here because there was no way to be sure it was really fullscreen until now
            pmConsumer->CompletePresent(eventIter->second);
        }

        if (eventIter->second->RuntimeThread != hdr.ThreadId) {
            if (eventIter->second->TimeTaken == 0) {
                eventIter->second->TimeTaken = EventTime - eventIter->second->QpcTime;
            }
            eventIter->second->WasBatched = true;
            pmConsumer->mPresentByThreadId.erase(eventIter);
        }
        break;
    }
    case DxgKrnl_PresentHistoryDetailed:
    case DxgKrnl_SubmitPresentHistory:
    {
        DxgkSubmitPresentHistoryEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.Token = GetEventData<uint64_t>(pEventRecord, L"Token");
        Args.TokenData = GetEventData<uint64_t>(pEventRecord, L"TokenData");
        auto KMTPresentModel = GetEventData<D3DKMT_PRESENT_MODEL>(pEventRecord, L"Model");
        Args.KnownPresentMode = D3DKMT_TokenModel_ToPresentMode(KMTPresentModel);
        if (KMTPresentModel != D3DKMT_PM_REDIRECTED_GDI)
        {
            pmConsumer->HandleDxgkSubmitPresentHistoryEventArgs(Args);
        }
        break;
    }
    case DxgKrnl_PropagatePresentHistory:
    {
        DxgkPropagatePresentHistoryEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.Token = GetEventData<uint64_t>(pEventRecord, L"Token");
        pmConsumer->HandleDxgkPropagatePresentHistoryEventArgs(Args);
        break;
    }
    case DxgKrnl_Blit:
    {
        DxgkBltEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.Hwnd = GetEventData<uint64_t>(pEventRecord, L"hwnd");
        Args.Present = GetEventData<uint32_t>(pEventRecord, L"bRedirectedPresent") != 0;
        pmConsumer->HandleDxgkBlt(Args);
        break;
    }
    }
}

namespace Win7
{
void HandleDxgkBlt(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DxgkBltEventArgs Args = {};
    Args.pEventHeader = &pEventRecord->EventHeader;
    auto pBltEvent = reinterpret_cast<DXGKETW_BLTEVENT*>(pEventRecord->UserData);
    Args.Hwnd = pBltEvent->hwnd;
    Args.Present = pBltEvent->bRedirectedPresent != 0;
    pmConsumer->HandleDxgkBlt(Args);
}

void HandleDxgkFlip(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DxgkFlipEventArgs Args = {};
    Args.pEventHeader = &pEventRecord->EventHeader;
    auto pFlipEvent = reinterpret_cast<DXGKETW_FLIPEVENT*>(pEventRecord->UserData);
    Args.FlipInterval = pFlipEvent->FlipInterval;
    Args.MMIO = pFlipEvent->MMIOFlip != 0;
    pmConsumer->HandleDxgkFlip(Args);
}

void HandleDxgkPresentHistory(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    auto pPresentHistoryEvent = reinterpret_cast<DXGKETW_PRESENTHISTORYEVENT*>(pEventRecord->UserData);
    if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_START)
    {
        DxgkSubmitPresentHistoryEventArgs Args = {};
        Args.pEventHeader = &pEventRecord->EventHeader;
        Args.KnownPresentMode = PresentMode::Unknown;
        Args.Token = pPresentHistoryEvent->Token;
        pmConsumer->HandleDxgkSubmitPresentHistoryEventArgs(Args);
    }
    else if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_INFO)
    {
        DxgkPropagatePresentHistoryEventArgs Args = {};
        Args.pEventHeader = &pEventRecord->EventHeader;
        Args.Token = pPresentHistoryEvent->Token;
        pmConsumer->HandleDxgkPropagatePresentHistoryEventArgs(Args);
    }
}

void HandleDxgkQueuePacket(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_START)
    {
        DxgkQueueSubmitEventArgs Args = {};
        Args.pEventHeader = &pEventRecord->EventHeader;
        auto pSubmitEvent = reinterpret_cast<DXGKETW_QUEUESUBMITEVENT*>(pEventRecord->UserData);
        switch (pSubmitEvent->PacketType)
        {
        case DXGKETW_MMIOFLIP_COMMAND_BUFFER:
            Args.PacketType = DxgKrnl_QueueSubmit_Type::MMIOFlip;
            break;
        case DXGKETW_SOFTWARE_COMMAND_BUFFER:
            Args.PacketType = DxgKrnl_QueueSubmit_Type::Software;
            break;
        default:
            Args.Present = pSubmitEvent->bPresent != 0;
            break;
        }
        Args.SubmitSequence = pSubmitEvent->SubmitSequence;
        Args.Context = pSubmitEvent->hContext;
        pmConsumer->HandleDxgkQueueSubmit(Args);
    }
    else if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_STOP)
    {
        DxgkQueueCompleteEventArgs Args = {};
        Args.pEventHeader = &pEventRecord->EventHeader;
        auto pCompleteEvent = reinterpret_cast<DXGKETW_QUEUECOMPLETEEVENT*>(pEventRecord->UserData);
        Args.SubmitSequence = pCompleteEvent->SubmitSequence;
        pmConsumer->HandleDxgkQueueComplete(Args);
    }
}

void HandleDxgkVSyncDPC(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DxgkSyncDPCEventArgs Args = {};
    Args.pEventHeader = &pEventRecord->EventHeader;
    auto pVSyncDPCEvent = reinterpret_cast<DXGKETW_SCHEDULER_VSYNC_DPC*>(pEventRecord->UserData);
    Args.FlipSubmitSequence = (uint32_t)(pVSyncDPCEvent->FlipFenceId.QuadPart >> 32u);
    pmConsumer->HandleDxgkSyncDPC(Args);
}

void HandleDxgkMMIOFlip(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DxgkMMIOFlipEventArgs Args = {};
    Args.pEventHeader = &pEventRecord->EventHeader;
    if (pEventRecord->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER)
    {
        auto pMMIOFlipEvent = reinterpret_cast<DXGKETW_SCHEDULER_MMIO_FLIP_32*>(pEventRecord->UserData);
        Args.Flags = static_cast<DxgKrnl_MMIOFlip_Flags>(pMMIOFlipEvent->Flags);
        Args.FlipSubmitSequence = pMMIOFlipEvent->FlipSubmitSequence;
    }
    else
    {
        auto pMMIOFlipEvent = reinterpret_cast<DXGKETW_SCHEDULER_MMIO_FLIP_64*>(pEventRecord->UserData);
        Args.Flags = static_cast<DxgKrnl_MMIOFlip_Flags>(pMMIOFlipEvent->Flags);
        Args.FlipSubmitSequence = pMMIOFlipEvent->FlipSubmitSequence;
    }
    pmConsumer->HandleDxgkMMIOFlip(Args);
}
}

void HandleWin32kEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    enum {
        Win32K_TokenCompositionSurfaceObject = 201,
        Win32K_TokenStateChanged = 301,
    };

    auto const& hdr = pEventRecord->EventHeader;

    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;

    switch (hdr.EventDescriptor.Id)
    {
    case Win32K_TokenCompositionSurfaceObject:
    {
        auto eventIter = pmConsumer->FindOrCreatePresent(hdr);

        // Check if we might have retrieved a 'stuck' present from a previous frame.
        if (eventIter->second->SeenWin32KEvents) {
            pmConsumer->mPresentByThreadId.erase(eventIter);
            eventIter = pmConsumer->FindOrCreatePresent(hdr);
        }

        eventIter->second->PresentMode = PresentMode::Composed_Flip;
        eventIter->second->SeenWin32KEvents = true;

        PMTraceConsumer::Win32KPresentHistoryTokenKey key(GetEventData<uint64_t>(pEventRecord, L"CompositionSurfaceLuid"),
            GetEventData<uint64_t>(pEventRecord, L"PresentCount"),
            GetEventData<uint64_t>(pEventRecord, L"BindId"));
        pmConsumer->mWin32KPresentHistoryTokens[key] = eventIter->second;
        break;
    }
    case Win32K_TokenStateChanged:
    {
        PMTraceConsumer::Win32KPresentHistoryTokenKey key(GetEventData<uint64_t>(pEventRecord, L"CompositionSurfaceLuid"),
            GetEventData<uint32_t>(pEventRecord, L"PresentCount"),
            GetEventData<uint64_t>(pEventRecord, L"BindId"));
        auto eventIter = pmConsumer->mWin32KPresentHistoryTokens.find(key);
        if (eventIter == pmConsumer->mWin32KPresentHistoryTokens.end()) {
            return;
        }

        enum class TokenState {
            InFrame = 3,
            Confirmed = 4,
            Retired = 5,
            Discarded = 6,
        };

        auto &event = *eventIter->second;
        auto state = GetEventData<TokenState>(pEventRecord, L"NewState");
        switch (state)
        {
        case TokenState::InFrame:
        {
            // InFrame = composition is starting
            if (event.Hwnd) {
                auto hWndIter = pmConsumer->mPresentByWindow.find(event.Hwnd);
                if (hWndIter == pmConsumer->mPresentByWindow.end()) {
                    pmConsumer->mPresentByWindow.emplace(event.Hwnd, eventIter->second);
                }
                else if (hWndIter->second != eventIter->second) {
                    hWndIter->second->FinalState = PresentResult::Discarded;
                    hWndIter->second = eventIter->second;
                }
            }

            bool iFlip = GetEventData<BOOL>(pEventRecord, L"IndependentFlip") != 0;
            if (iFlip && event.PresentMode == PresentMode::Composed_Flip) {
                event.PresentMode = PresentMode::Hardware_Independent_Flip;
            }

            break;
        }
        case TokenState::Confirmed:
        {
            // Confirmed = present has been submitted
            // If we haven't already decided we're going to discard a token, now's a good time to indicate it'll make it to screen
            if (event.FinalState == PresentResult::Unknown) {
                if (event.PresentFlags & DXGI_PRESENT_DO_NOT_SEQUENCE) {
                    // DO_NOT_SEQUENCE presents may get marked as confirmed,
                    // if a frame was composed when this token was completed
                    event.FinalState = PresentResult::Discarded;
                }
                else {
                    event.FinalState = PresentResult::Presented;
                }
            }
            if (event.Hwnd) {
                pmConsumer->mPresentByWindow.erase(event.Hwnd);
            }
            break;
        }
        case TokenState::Retired:
        {
            // Retired = present has been completed, token's buffer is now displayed
            event.ScreenTime = EventTime;
            break;
        }
        case TokenState::Discarded:
        {
            // Discarded = destroyed - discard if we never got any indication that it was going to screen
            auto sharedPtr = eventIter->second;
            pmConsumer->mWin32KPresentHistoryTokens.erase(eventIter);

            if (event.FinalState == PresentResult::Unknown || event.ScreenTime == 0) {
                event.FinalState = PresentResult::Discarded;
            }

            pmConsumer->CompletePresent(sharedPtr);
            break;
        }
        }
        break;
    }
    }
}

void HandleDWMEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    enum {
        DWM_GetPresentHistory = 64,
        DWM_Schedule_Present_Start = 15,
        DWM_FlipChain_Pending = 69,
        DWM_FlipChain_Complete = 70,
        DWM_FlipChain_Dirty = 101,
        DWM_Schedule_SurfaceUpdate = 196,
    };

    auto& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id)
    {
    case DWM_GetPresentHistory:
    {
        for (auto& hWndPair : pmConsumer->mPresentByWindow)
        {
            auto& present = hWndPair.second;
            // Pickup the most recent present from a given window
            if (present->PresentMode != PresentMode::Composed_Copy_GPU_GDI &&
                present->PresentMode != PresentMode::Composed_Copy_CPU_GDI) {
                continue;
            }
            present->DwmNotified = true;
            pmConsumer->mPresentsWaitingForDWM.emplace_back(present);
        }
        pmConsumer->mPresentByWindow.clear();
        break;
    }
    case DWM_Schedule_Present_Start:
    {
        pmConsumer->DwmPresentThreadId = hdr.ThreadId;
        break;
    }
    case DWM_FlipChain_Pending:
    case DWM_FlipChain_Complete:
    case DWM_FlipChain_Dirty:
    {
        if (InlineIsEqualGUID(hdr.ProviderId, Win7::DWM_PROVIDER_GUID)) {
            return;
        }
        // As it turns out, the 64-bit token data from the PHT submission is actually two 32-bit data chunks,		
        // corresponding to a "flip chain" id and present id		
        uint32_t flipChainId = (uint32_t)GetEventData<uint64_t>(pEventRecord, L"ulFlipChain");
        uint32_t serialNumber = (uint32_t)GetEventData<uint64_t>(pEventRecord, L"ulSerialNumber");
        uint64_t token = ((uint64_t)flipChainId << 32ull) | serialNumber;
        auto flipIter = pmConsumer->mDxgKrnlPresentHistoryTokens.find(token);
        if (flipIter == pmConsumer->mDxgKrnlPresentHistoryTokens.end()) {
            return;
        }

        // Watch for multiple legacy blits completing against the same window		
        auto hWnd = GetEventData<uint64_t>(pEventRecord, L"hwnd");
        pmConsumer->mPresentByWindow[hWnd] = flipIter->second;
        flipIter->second->DwmNotified = true;
        pmConsumer->mPresentsByLegacyBlitToken.erase(flipIter);
        break;
    }
    case DWM_Schedule_SurfaceUpdate:
    {
        PMTraceConsumer::Win32KPresentHistoryTokenKey key(GetEventData<uint64_t>(pEventRecord, L"luidSurface"),
                                                          GetEventData<uint64_t>(pEventRecord, L"PresentCount"),
                                                          GetEventData<uint64_t>(pEventRecord, L"bindId"));
        auto eventIter = pmConsumer->mWin32KPresentHistoryTokens.find(key);
        if (eventIter != pmConsumer->mWin32KPresentHistoryTokens.end()) {
            eventIter->second->DwmNotified = true;
        }
        break;
    }
    }
}

void HandleD3D9Event(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    enum {
        D3D9PresentStart = 1,
        D3D9PresentStop,
    };

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id)
    {
    case D3D9PresentStart:
    {
        PresentEvent event(hdr, Runtime::D3D9);
        GetEventData(pEventRecord, L"pSwapchain", &event.SwapChainAddress);
        uint32_t D3D9Flags = GetEventData<uint32_t>(pEventRecord, L"Flags");
        event.PresentFlags =
            ((D3D9Flags & D3DPRESENT_DONOTFLIP) ? DXGI_PRESENT_DO_NOT_SEQUENCE : 0) |
            ((D3D9Flags & D3DPRESENT_DONOTWAIT) ? DXGI_PRESENT_DO_NOT_WAIT : 0) |
            ((D3D9Flags & D3DPRESENT_FLIPRESTART) ? DXGI_PRESENT_RESTART : 0);
        if ((D3D9Flags & D3DPRESENT_FORCEIMMEDIATE) != 0) {
            event.SyncInterval = 0;
        }

        pmConsumer->RuntimePresentStart(event);
        break;
    }
    case D3D9PresentStop:
    {
        auto result = GetEventData<uint32_t>(pEventRecord, L"Result");
        bool AllowBatching = SUCCEEDED(result) && result != S_PRESENT_OCCLUDED;
        pmConsumer->RuntimePresentStop(hdr, AllowBatching);
        break;
    }
    }
}

void PMTraceConsumer::CompletePresent(std::shared_ptr<PresentEvent> p)
{
    if (p->Completed)
    {
        p->FinalState = PresentResult::Error;
        return;
    }

    // Complete all other presents that were riding along with this one (i.e. this one came from DWM)
    for (auto& p2 : p->DependentPresents) {
        p2->ScreenTime = p->ScreenTime;
        p2->FinalState = PresentResult::Presented;
        CompletePresent(p2);
    }
    p->DependentPresents.clear();

    // Remove it from any tracking maps that it may have been inserted into
    if (p->QueueSubmitSequence != 0) {
        mPresentsBySubmitSequence.erase(p->QueueSubmitSequence);
    }
    if (p->Hwnd != 0) {
        auto hWndIter = mPresentByWindow.find(p->Hwnd);
        if (hWndIter != mPresentByWindow.end() && hWndIter->second == p) {
            mPresentByWindow.erase(hWndIter);
        }
    }
    if (p->TokenPtr != 0) {
        auto iter = mDxgKrnlPresentHistoryTokens.find(p->TokenPtr);
        if (iter != mDxgKrnlPresentHistoryTokens.end() && iter->second == p) {
            mDxgKrnlPresentHistoryTokens.erase(iter);
        }
    }
    auto& processMap = mPresentsByProcess[p->ProcessId];
    processMap.erase(p->QpcTime);

    auto& presentDeque = mPresentsByProcessAndSwapChain[std::make_tuple(p->ProcessId, p->SwapChainAddress)];
    auto presentIter = presentDeque.begin();
    assert(!presentIter->get()->Completed); // It wouldn't be here anymore if it was

    if (p->FinalState == PresentResult::Presented) {
        while (*presentIter != p) {
            CompletePresent(*presentIter);
            presentIter = presentDeque.begin();
        }
    }

    p->Completed = true;
    if (*presentIter == p) {
        auto lock = scoped_lock(mMutex);
        while (presentIter != presentDeque.end() && presentIter->get()->Completed) {
            mCompletedPresents.push_back(*presentIter);
            presentDeque.pop_front();
            presentIter = presentDeque.begin();
        }
    }

}

decltype(PMTraceConsumer::mPresentByThreadId.begin()) PMTraceConsumer::FindOrCreatePresent(EVENT_HEADER const& hdr)
{
    // Easy: we're on a thread that had some step in the present process
    auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
    if (eventIter != mPresentByThreadId.end()) {
        return eventIter;
    }

    // No such luck, check for batched presents
    auto& processMap = mPresentsByProcess[hdr.ProcessId];
    auto processIter = std::find_if(processMap.begin(), processMap.end(),
        [](auto processIter) {return processIter.second->PresentMode == PresentMode::Unknown; });
    if (processIter == processMap.end()) {
        // This likely didn't originate from a runtime whose events we're tracking (DXGI/D3D9)
        // Could be composition buffers, or maybe another runtime (e.g. GL)
        auto newEvent = std::make_shared<PresentEvent>(hdr, Runtime::Other);
        processMap.emplace(newEvent->QpcTime, newEvent);

        auto& processSwapChainDeque = mPresentsByProcessAndSwapChain[std::make_tuple(hdr.ProcessId, 0ull)];
        processSwapChainDeque.emplace_back(newEvent);

        eventIter = mPresentByThreadId.emplace(hdr.ThreadId, newEvent).first;
    }
    else {
        // Assume batched presents are popped off the front of the driver queue by process in order, do the same here
        eventIter = mPresentByThreadId.emplace(hdr.ThreadId, processIter->second).first;
        processMap.erase(processIter);
    }

    return eventIter;
}

void PMTraceConsumer::RuntimePresentStart(PresentEvent &event)
{
    // Ignore PRESENT_TEST: it's just to check if you're still fullscreen, doesn't actually do anything
    if ((event.PresentFlags & DXGI_PRESENT_TEST) != 0) {
        event.Completed = true;
        return;
    }

    auto pEvent = std::make_shared<PresentEvent>(event);
    mPresentByThreadId[event.RuntimeThread] = pEvent;

    auto& processMap = mPresentsByProcess[event.ProcessId];
    processMap.emplace(event.QpcTime, pEvent);

    auto& processSwapChainDeque = mPresentsByProcessAndSwapChain[std::make_tuple(event.ProcessId, event.SwapChainAddress)];
    processSwapChainDeque.emplace_back(pEvent);

    // Set the caller's local event instance to completed so the assert
    // in ~PresentEvent() doesn't fire when it is destructed.
    event.Completed = true;
}

void PMTraceConsumer::RuntimePresentStop(EVENT_HEADER const& hdr, bool AllowPresentBatching)
{
    auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
    if (eventIter == mPresentByThreadId.end()) {
        return;
    }
    auto &event = *eventIter->second;

    assert(event.QpcTime <= *(uint64_t*) &hdr.TimeStamp);
    event.TimeTaken = *(uint64_t*) &hdr.TimeStamp - event.QpcTime;

    if (!AllowPresentBatching || mSimpleMode) {
        event.FinalState = AllowPresentBatching ? PresentResult::Presented : PresentResult::Discarded;
        CompletePresent(eventIter->second);
    }

    mPresentByThreadId.erase(eventIter);
}

void HandleNTProcessEvent(PEVENT_RECORD pEventRecord, PMTraceConsumer* pmConsumer)
{
    NTProcessEvent event;

    switch (pEventRecord->EventHeader.EventDescriptor.Opcode) {
    case EVENT_TRACE_TYPE_START:
    case EVENT_TRACE_TYPE_DC_START:
        GetEventData(pEventRecord, L"ProcessId",     &event.ProcessId);
        GetEventData(pEventRecord, L"ImageFileName", &event.ImageFileName);
        break;

    case EVENT_TRACE_TYPE_END:
    case EVENT_TRACE_TYPE_DC_END:
        GetEventData(pEventRecord, L"ProcessId", &event.ProcessId);
        break;
    }

    {
        auto lock = scoped_lock(pmConsumer->mNTProcessEventMutex);
        pmConsumer->mNTProcessEvents.emplace_back(event);
    }
}

