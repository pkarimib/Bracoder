/* -LICENSE-START-
** Copyright (c) 2013 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <libgen.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <cassert>
#include <list>

#include "Playback.hh"
#include "exception.hh"
#include "display.hh"
#include "chunk.hh"
#include "barcode.hh"
#include "child_process.hh"
#include "system_runner.hh"

using std::chrono::time_point;
using std::chrono::high_resolution_clock;
using std::chrono::time_point_cast;
using std::chrono::microseconds;

const BMDTimeScale ticks_per_second = (BMDTimeScale)1000000; /* microsecond resolution */ 

pthread_mutex_t         sleepMutex;
pthread_cond_t          sleepCond;
bool                do_exit = false;

const unsigned long     kAudioWaterlevel = 48000;
// std::ofstream debugf;

void sigfunc(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        do_exit = true;
    }
    pthread_cond_signal(&sleepCond);
}

int main(int argc, char *argv[])
{
    int             exitStatus = 1;
    Playback*    generator = NULL;

    pthread_mutex_init(&sleepMutex, NULL);
    pthread_cond_init(&sleepCond, NULL);

    signal(SIGINT, sigfunc);
    signal(SIGTERM, sigfunc);
    signal(SIGHUP, sigfunc);

    BMDConfig config;
    if (!config.ParseArguments(argc, argv))
    {
        config.DisplayUsage(exitStatus);
        return EXIT_FAILURE;
    }

    SystemCall( "mlockall", mlockall( MCL_CURRENT | MCL_FUTURE ) );

    std::cerr << "Loading file...";
    generator = new Playback(&config);
    std::cerr << "done!";
    
    // TODO: make this not hard coded...
    ChildProcess command_process ( "cellsim", [&]() {
            return ezexec ( { "/home/john/Work/multisend/sender/cellsim", 
                        "/home/john/Work/mahimahi/traces/Verizon-LTE-short.up",
                        "/home/john/Work/mahimahi/traces/Verizon-LTE-short.down",
                        "0",
                        "eth1",
                        "eth0"} );
        }
        );

    generator->Run();
    exitStatus = 0;

    if (generator)
    {
        generator->Release();
        generator = NULL;
    }
    return exitStatus;
}

Playback::~Playback()
{
}

Playback::Playback(BMDConfig *config) :
    m_refCount(1),
    m_config(config),
    m_running(false),
    m_deckLink(),
    m_deckLinkOutput(),
    m_displayMode(),
    m_frameWidth(0),
    m_frameHeight(0),
    m_frameDuration(0),
    m_frameTimescale(0),
    m_framesPerSecond(0),
    m_totalFramesScheduled(0),
    m_totalFramesDropped(0),
    m_totalFramesCompleted(0),
    m_logfile(),
    m_infile(m_config->m_videoInputFile),
    scheduled_timestamp_cpu(),
    scheduled_timestamp_decklink()
{}

bool Playback::Run()
{
    HRESULT                         result;
    int                             idx;
    bool                            success = false;

    IDeckLinkIterator*              deckLinkIterator = NULL;
    IDeckLinkDisplayModeIterator*   displayModeIterator = NULL;
    char*                           displayModeName = NULL;

    // Get the DeckLink device
    deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (!deckLinkIterator)
    {
        fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
        goto bail;
    }

    idx = m_config->m_deckLinkIndex;

    while ((result = deckLinkIterator->Next(&m_deckLink)) == S_OK)
    {
        if (idx == 0)
            break;
        --idx;

        m_deckLink->Release();
    }

    if (result != S_OK || m_deckLink == NULL)
    {
        fprintf(stderr, "Unable to get DeckLink device %u\n", m_config->m_deckLinkIndex);
        goto bail;
    }

    // Get the output (display) interface of the DeckLink device
    if (m_deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&m_deckLinkOutput) != S_OK)
        goto bail;

    // Get the display mode
    idx = m_config->m_displayModeIndex;

    result = m_deckLinkOutput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK)
        goto bail;

    while ((result = displayModeIterator->Next(&m_displayMode)) == S_OK)
    {
        if (idx == 0)
            break;
        --idx;

        m_displayMode->Release();
    }

    if (result != S_OK || m_displayMode == NULL)
    {
        fprintf(stderr, "Unable to get display mode %d\n", m_config->m_displayModeIndex);
        goto bail;
    }

    // Get display mode name
    result = m_displayMode->GetName((const char**)&displayModeName);
    if (result != S_OK)
    {
        displayModeName = (char *)malloc(32);
        snprintf(displayModeName, 32, "[index %d]", m_config->m_displayModeIndex);
    }

    if (m_config->m_videoInputFile == NULL) {
        fprintf(stderr, "-v <video filename> flag required\n");
        exit(1);
    }

    if (m_config->m_logFilename != NULL) {
        m_logfile.open(m_config->m_logFilename, std::ios::out);
        if (!m_logfile.is_open()) {
            fprintf(stderr, "Could not open logfile.\n");
            goto bail;
        } 
    }

    /* IMPORTANT: print log file csv headers */
    if (m_logfile.is_open()) {
        std::time_t result = std::time(nullptr);
        
        m_logfile << "# Writing video to decklink interface: " << m_config->m_videoInputFile << std::endl
                  << "# Time stamp: " << std::asctime(std::localtime(&result))
                  << "# frame_index,upper_left_barcode,lower_right_barcode,cpu_time_scheduled,cpu_time_completed,decklink_hardwaretime_scheduled,decklink_hardwaretime_completed_callback,decklink_frame_completed_reference_time"
                  << "\n";
    }
    else {
        std::time_t result = std::time(nullptr);

        std::cout << "# Writing video to decklink interface: " << m_config->m_videoInputFile << std::endl
                  << "# Time stamp: " << std::asctime(std::localtime(&result)) << std::endl 
                  << "# frame_index,upper_left_barcode,lower_right_barcode,cpu_time_scheduled,cpu_time_completed,decklink_hardwaretime_scheduled,decklink_hardwaretime_completed_callback,decklink_frame_completed_reference_time"
                  << "\n";
    }

    m_config->DisplayConfiguration();

    // Provide this class as a delegate to the audio and video output interfaces
    m_deckLinkOutput->SetScheduledFrameCompletionCallback(this);

    success = true;

    // Start.
    while (!do_exit)
    {
        StartRunning();
        fprintf(stderr, "Starting playback\n");

        pthread_mutex_lock(&sleepMutex);
        pthread_cond_wait(&sleepCond, &sleepMutex);
        pthread_mutex_unlock(&sleepMutex);

        fprintf(stderr, "Stopping playback\n");
        StopRunning();
    }

    printf("\n");

    m_running = false;

bail:
    if (displayModeName != NULL)
        free(displayModeName);

    if (m_displayMode != NULL)
        m_displayMode->Release();

    if (displayModeIterator != NULL)
        displayModeIterator->Release();

    if (m_deckLinkOutput != NULL)
        m_deckLinkOutput->Release();

    if (m_deckLink != NULL)
        m_deckLink->Release();

    if (deckLinkIterator != NULL)
        deckLinkIterator->Release();

    return success;
}

void Playback::StartRunning()
{
    HRESULT                 result;

    m_frameWidth = m_displayMode->GetWidth();
    m_frameHeight = m_displayMode->GetHeight();
    m_displayMode->GetFrameRate(&m_frameDuration, &m_frameTimescale);

    // Calculate the number of frames per second, rounded up to the nearest integer.  For example, for NTSC (29.97 FPS), framesPerSecond == 30.
    m_framesPerSecond = (unsigned long)((m_frameTimescale + (m_frameDuration-1))  /  m_frameDuration);
    assert(m_framesPerSecond == 60);
    m_framesPerSecond = 60;
    std::cout << "m_framesPerSecond: "  << m_framesPerSecond << std::endl;

    // Set the video output mode
    result = m_deckLinkOutput->EnableVideoOutput(m_displayMode->GetDisplayMode(), m_config->m_outputFlags);
    if (result != S_OK)
    {
        fprintf(stderr, "Failed to enable video output. Is another application using the card?\n");
        goto bail;
    } 

    // Begin video preroll by scheduling a second of frames in hardware
    m_totalFramesScheduled = 0;
    m_totalFramesDropped = 0;
    m_totalFramesCompleted = 0;
    for (unsigned i = 0; i < m_framesPerSecond; i++)
        ScheduleNextFrame(true);

    m_deckLinkOutput->StartScheduledPlayback(0, m_frameTimescale, 1.0);

    m_running = true;

    return;

bail:
    // *** Error-handling code.  Cleanup any resources that were allocated. *** //
    StopRunning();
}

void Playback::StopRunning()
{
    // Stop the audio and video output streams immediately
    m_deckLinkOutput->StopScheduledPlayback(0, NULL, 0); 
    // debugf.close();
    // Success; update the UI
    m_running = false;
}

void Playback::ScheduleNextFrame(bool prerolling)
{
    if (prerolling == false)
    {
        // If not prerolling, make sure that playback is still active
        if (m_running == false)
            return;
    }

    void* frameBytes = NULL;
    IDeckLinkMutableVideoFrame* newFrame;
    int bytesPerPixel = GetBytesPerPixel(m_config->m_pixelFormat);
    HRESULT result = m_deckLinkOutput->CreateVideoFrame(m_frameWidth, m_frameHeight, 
                                                                              m_frameWidth * bytesPerPixel, 
                                                                              m_config->m_pixelFormat, bmdFrameFlagDefault, &newFrame);
    if (result != S_OK) {
        fprintf(stderr, "Failed to create video frame\n");
        return;
    }

    newFrame->GetBytes(&frameBytes);

    const unsigned int frame_size = 4 * m_frameWidth * m_frameHeight;
    const unsigned int frame_count = m_infile.size() / (uint64_t)frame_size;
    
    if ( m_totalFramesScheduled < frame_count ) {
        Chunk c = m_infile(m_totalFramesScheduled * frame_size, frame_size);

        std::memcpy(frameBytes, c.buffer(), c.size());
        if (m_deckLinkOutput->ScheduleVideoFrame(newFrame, (m_totalFramesScheduled * m_frameDuration), m_frameDuration, m_frameTimescale) != S_OK)
            return;
        
        /* IMPORTANT: get the scheduled frame timestamps */
        time_point<high_resolution_clock> tp = high_resolution_clock::now();

        BMDTimeValue decklink_hardware_timestamp;
        BMDTimeValue decklink_time_in_frame;
        BMDTimeValue decklink_ticks_per_frame;
        HRESULT ret;
        if ( (ret = m_deckLinkOutput->GetHardwareReferenceClock(ticks_per_second,
                                                                &decklink_hardware_timestamp,
                                                                &decklink_time_in_frame,
                                                                &decklink_ticks_per_frame) ) != S_OK) {
            std::cerr << "ScheduleNextFrame: could not get GetHardwareReferenceClock timestamp" << std::endl;
            m_running = false;
            return;
        }
        
        /* IMPORTANT: store the scheduled fram timestamps */
        scheduled_timestamp_cpu.push_back(tp);
        scheduled_timestamp_decklink.push_back(decklink_hardware_timestamp);

        

        m_totalFramesScheduled += 1;
    }
    else {
        m_running = false;
    }
}

HRESULT Playback::CreateFrame(IDeckLinkVideoFrame** frame, void (*fillFunc)(IDeckLinkVideoFrame*))
{
    HRESULT                     result;
    int                         bytesPerPixel = GetBytesPerPixel(m_config->m_pixelFormat);
    IDeckLinkMutableVideoFrame* newFrame = NULL;
    IDeckLinkMutableVideoFrame* referenceFrame = NULL;
    IDeckLinkVideoConversion*   frameConverter = NULL;

    *frame = NULL;

    result = m_deckLinkOutput->CreateVideoFrame(m_frameWidth, m_frameHeight, m_frameWidth * bytesPerPixel, m_config->m_pixelFormat, bmdFrameFlagDefault, &newFrame);
    if (result != S_OK)
    {
        fprintf(stderr, "Failed to create video frame\n");
        goto bail;
    }

    if (m_config->m_pixelFormat == bmdFormat8BitBGRA)
    {
        fillFunc(newFrame);
    }
    else
    {
        // Create a black frame in 8 bit YUV and convert to desired format
        result = m_deckLinkOutput->CreateVideoFrame(m_frameWidth, m_frameHeight, m_frameWidth * GetBytesPerPixel(bmdFormat8BitBGRA), bmdFormat8BitBGRA, bmdFrameFlagDefault, &referenceFrame);
        if (result != S_OK)
        {
            fprintf(stderr, "Failed to create reference video frame\n");
            goto bail;
        }

        fillFunc(referenceFrame);

        frameConverter = CreateVideoConversionInstance();

        result = frameConverter->ConvertFrame(referenceFrame, newFrame);
        if (result != S_OK)
        {
            fprintf(stderr, "Failed to convert frame\n");
            goto bail;
        }
    }

    *frame = newFrame;
    newFrame = NULL;

bail:
    if (referenceFrame != NULL)
        referenceFrame->Release();

    if (frameConverter != NULL)
        frameConverter->Release();

    if (newFrame != NULL)
        newFrame->Release();

    return result;
}

void Playback::PrintStatusLine(uint32_t queued)
{
    printf("scheduled %-16lu completed %-16lu dropped %-16lu frame level %-16u\n",
        m_totalFramesScheduled, m_totalFramesCompleted, m_totalFramesDropped, queued);
}

/************************* DeckLink API Delegate Methods *****************************/


HRESULT Playback::QueryInterface(REFIID, LPVOID *ppv)
{
    *ppv = NULL;
    return E_NOINTERFACE;
}

ULONG Playback::AddRef()
{
    // gcc atomic operation builtin
    return __sync_add_and_fetch(&m_refCount, 1);
}

ULONG Playback::Release()
{
    // gcc atomic operation builtin
    ULONG newRefValue = __sync_sub_and_fetch(&m_refCount, 1);
    if (!newRefValue)
        delete this;
    return newRefValue;
}

HRESULT Playback::ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result)
{
    /* IMPORTANT: get the time stamps for when a frame is completed */
    time_point<high_resolution_clock> tp = high_resolution_clock::now();

    BMDTimeValue decklink_hardware_timestamp;
    BMDTimeValue decklink_time_in_frame;
    BMDTimeValue decklink_ticks_per_frame;
    HRESULT ret;
    if ( (ret = m_deckLinkOutput->GetHardwareReferenceClock(ticks_per_second,
                                                            &decklink_hardware_timestamp,
                                                            &decklink_time_in_frame,
                                                            &decklink_ticks_per_frame) ) != S_OK) {
        std::cerr << "ScheduledFrameCompleted: could not get GetHardwareReferenceClock timestamp" << std::endl;
        return ret;
    }
    
    BMDTimeValue decklink_frame_completed_timestamp;
    if( (ret = m_deckLinkOutput->GetFrameCompletionReferenceTimestamp(completedFrame, 
                                                                      ticks_per_second,
                                                                      &decklink_frame_completed_timestamp) ) != S_OK ) {
        
        std::cerr << "ScheduledFrameCompleted: could not get FrameCompletionReference timestamp" << std::endl;
        return ret;
    }
    
    if (do_exit) {
        ++m_totalFramesCompleted;
        completedFrame->Release();
        return S_OK;
    }
    
    void *frameBytes = NULL;
    completedFrame->GetBytes(&frameBytes);

    switch (result) {
        case bmdOutputFrameCompleted: 
        {
            Chunk chunk((uint8_t*)frameBytes, completedFrame->GetRowBytes() * completedFrame->GetHeight());
            XImage img(chunk, completedFrame->GetWidth(), completedFrame->GetHeight());
            auto barcodes = Barcode::readBarcodes(img);

            /* IMPORTANT: print timestamps for fram was completed */
            if (m_logfile.is_open()) {
                m_logfile   << m_totalFramesCompleted << "," 
                            << barcodes.first << "," << barcodes.second << ","
                            << time_point_cast<microseconds>(scheduled_timestamp_cpu.front()).time_since_epoch().count() << ","
                            << time_point_cast<microseconds>(tp).time_since_epoch().count() << ","
                            << scheduled_timestamp_decklink.front()  << ","
                            << decklink_hardware_timestamp << ","
                            << decklink_frame_completed_timestamp
                            << std::endl;

                scheduled_timestamp_cpu.pop_front();
                scheduled_timestamp_decklink.pop_front();
            }
            else { 
                std::cout   << m_totalFramesCompleted << "," 
                            << barcodes.first << "," << barcodes.second << ","
                            << time_point_cast<microseconds>(scheduled_timestamp_cpu.front()).time_since_epoch().count() << ","
                            << time_point_cast<microseconds>(tp).time_since_epoch().count() << ","
                            << scheduled_timestamp_decklink.front()  << ","
                            << decklink_hardware_timestamp << ","
                            << decklink_frame_completed_timestamp
                            << std::endl;

                scheduled_timestamp_cpu.pop_front();
                scheduled_timestamp_decklink.pop_front();
            }

            std::cout << "Frame #" << m_totalFramesCompleted << " on time." << std::endl;
            break;
        }
        case bmdOutputFrameDisplayedLate:
            std::cout << "Warning: Frame " << m_totalFramesCompleted << " Displayed Late. " << std::endl;
            throw std::runtime_error("Frame Displayed Late.");
            break;
        case bmdOutputFrameDropped:
            std::cout  << "Warning: Frame " << m_totalFramesCompleted << " Dropped. " << std::endl;
            m_totalFramesDropped++;
            throw std::runtime_error("Frame Dropped");
            break;
        case bmdOutputFrameFlushed:
            std::cout << "Warning: Frame " << m_totalFramesCompleted << " Flushed. " << std::endl;
            throw std::runtime_error("Frame Flushed.");
            break;
        default:
            std::cerr << "Error in ScheduledFrameCompleted" << std::endl;
            throw std::runtime_error("Error in ScheduledFrameCompleted");
            break;
    }
    completedFrame->Release();
    ++m_totalFramesCompleted;
    
    ScheduleNextFrame(false);

    return S_OK;
}

HRESULT Playback::ScheduledPlaybackHasStopped()
{
    return S_OK;
}

/*****************************************/

int GetBytesPerPixel(BMDPixelFormat pixelFormat)
{
    int bytesPerPixel = 2;

    switch(pixelFormat)
    {
    case bmdFormat8BitYUV:
        bytesPerPixel = 2;
        break;
    case bmdFormat8BitARGB:
    case bmdFormat10BitYUV:
    case bmdFormat10BitRGB:
    case bmdFormat8BitBGRA:
        bytesPerPixel = 4;
        break;
    }

    return bytesPerPixel;
}

