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

#include <map>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "..\PresentData\SwapChainData.hpp"
#include "..\PresentData\LateStageReprojectionData.hpp"
#include "..\PresentData\MixedRealityTraceConsumer.hpp"

struct ProcessInfo {
    std::string mModuleName;
    std::map<uint64_t, SwapChainData> mChainMap;
    uint64_t mLastRefreshTicks; // GetTickCount64
    bool mTargetProcess;
};

struct PresentMonData {
    char mCaptureTimeStr[18] = "";
    uint64_t mStartupQpcTime = 0;
    uint32_t mTargetPid = 0;
    std::map<uint32_t, ProcessInfo> mProcessMap;
};

#pragma pack (push, 1)
typedef struct EventScores {
    double fps;
    double flip;
    double deltaReady;
    double deltaDisplayed;
    double timeTaken;
    double screenTime;
} EventScores;
#pragma pack (pop)

typedef enum
{
    STATUS_OK = 0,
    GENERAL_ERROR = 1000,
    EVENT_RECORDING_ALREADY_RUN_ERROR,
    EVENT_RECORDING_SHOULD_QUIT_ERROR,
    EVENT_RECORDING_IS_NOT_RUNNING_ERROR,
    EVENT_RECORDING_STOP_ERROR,
    INVALID_ARGUMENTS_ERROR,
    BUFFER_IS_NOT_EMPTY_ERROR,
    PRIVILIGIES_ERROR
}EventTracerExitCodes;

extern "C" {
    __declspec(dllexport) int StartEventRecording(int TargetPid, int arraySize);
    __declspec(dllexport) int StopEventRecording();
    __declspec(dllexport) int SetLogLevel(int level);
    __declspec(dllexport) int GetCurrentData(int numSamples, EventScores *scoresOutputBuf, double *timeOutputBuf, int *returnedSamples);
    __declspec(dllexport) int GetDataCount(int *result);
    __declspec(dllexport) int GetData(int dataCount, double *tsBuf, EventScores *scoresBuf);
}
