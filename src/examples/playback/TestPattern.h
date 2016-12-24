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

#include "DeckLinkAPI.h"
#include "Config.h"
#include "../scanner/Scanner.hh"
#include <fstream>

class TestPattern : public IDeckLinkVideoOutputCallback {
private:
    int32_t                 m_refCount;
    BMDConfig*              m_config;
    bool                    m_running;
    IDeckLink*              m_deckLink;
    IDeckLinkOutput*        m_deckLinkOutput;
    IDeckLinkDisplayMode*   m_displayMode;

    unsigned long           m_frameWidth;
    unsigned long           m_frameHeight;
    BMDTimeValue            m_frameDuration;
    BMDTimeScale            m_frameTimescale;
    unsigned long           m_framesPerSecond;
    IDeckLinkVideoFrame*    m_videoFrameBlack;
    IDeckLinkVideoFrame*    m_videoFrameBars;
    unsigned long           m_totalFramesScheduled;
    unsigned long           m_totalFramesDropped;
    unsigned long           m_totalFramesCompleted;

    Scanner                 &m_scanner;
    std::ifstream           m_infile;
    ~TestPattern();

    // Signal Generator Implementation
    void            StartRunning();
    void            StopRunning();
    void            ScheduleNextFrame(bool prerolling);

    void            PrintStatusLine(uint32_t queued);

public:
    TestPattern(BMDConfig *config, Scanner &s);
    bool Run();

    // *** DeckLink API implementation of IDeckLinkVideoOutputCallback IDeckLinkAudioOutputCallback *** //
    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv);
    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();

    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result);
    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped();

    HRESULT CreateFrame(IDeckLinkVideoFrame** theFrame, void (*fillFunc)(IDeckLinkVideoFrame*));

    TestPattern( const TestPattern & other ) = delete;
    TestPattern & operator=( const TestPattern & other ) = delete;
};

void FillColourBars(IDeckLinkVideoFrame* theFrame);
void FillBlack(IDeckLinkVideoFrame* theFrame);
int GetBytesPerPixel(BMDPixelFormat pixelFormat);
