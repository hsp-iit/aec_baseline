/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#ifndef AEC_COMPONENT__HPP
#define AEC_COMPONENT__HPP

#include <mutex>
#include <thread>
#include <memory>
#include <cstddef>
#include <vector>
#include <string>
#include <chrono>
#include <yarp/os/Network.h>
#include <yarp/os/LogStream.h>
#include <yarp/os/RFModule.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/sig/Sound.h>
#include <yarp/sig/SoundFilters.h>

#include "api/scoped_refptr.h"
#include "modules/audio_processing/include/audio_processing.h"
#include <deque>

#include "referenceReader.hpp"
#include "delayEstimator.hpp"

class AECComponent : public yarp::os::RFModule,
                     public yarp::os::TypedReaderCallback<yarp::sig::Sound>
{
public:
    AECComponent();
    ~AECComponent();

    bool configure(yarp::os::ResourceFinder &rf) override;
    bool close() override;
    bool updateModule() override;
    double getPeriod() override;

    void onRead(yarp::sig::Sound &msg) override;

private:
    // YARP network and ports
    yarp::os::BufferedPort<yarp::sig::Sound> m_microphoneAudioInputPort;  // Microphone input
    yarp::os::BufferedPort<yarp::sig::Sound> m_audioOutputPort;           // Echo-canceled output

    // WebRTC AEC
    rtc::scoped_refptr<webrtc::AudioProcessing> m_apm;
    webrtc::StreamConfig *m_streamConfig;

    // Synchronization
    std::mutex m_mutex;
    std::thread m_processingThread;
    bool m_shouldExit;

    // Buffers to accumulate samples when incoming frames are larger than AEC frame size
    std::vector<short> m_micBuffer;
    std::vector<short> m_refBuffer;
    std::vector<short> m_outBuffer;
    std::vector<short> m_audioSaveBuffer;
    std::vector<short> m_microphoneSaveBuffer;
    std::vector<short> m_referenceSaveBuffer;

    // Audio buffer management
    void processingThreadFunction();
    bool initializeAEC(int sample_rate, int num_channels);
    bool saveAudioBlockToDisk(const std::vector<short> &samples,
                              const std::string &streamName,
                              std::size_t sequence);
    void flushAudioSaveBuffer(std::vector<short> &buffer,
                              const std::string &streamName,
                              std::size_t &sequence,
                              bool flushRemainder = false);
    void flushAllAudioSaveBuffers(bool flushRemainder = false);
    void sendFilteredAudio(const std::vector<short> &samples, int sampleRate);

    // Configuration parameters
    int m_sample_rate;
    int m_num_channels;
    int m_block_ms;
    bool m_saveIterativeAudioToDisk;
    int m_audioSaveMaxSeconds;
    double m_referenceBufferSeconds;
    std::string m_audioSaveDirectory;
    std::string m_audioSavePrefix;
    std::size_t m_audioSaveSequence;
    std::size_t m_microphoneSaveSequence;
    std::size_t m_referenceSaveSequence;

    // AEC tuning and diagnostics
    bool m_aecMobileMode;
    int m_aecStreamDelayMs;
    bool m_logAecStats;
    int m_lastEstimatedDelayIndex{-1};
    std::chrono::steady_clock::time_point m_lastAecStatsLog;

    ReferenceReader m_referenceReader;
    DelayEstimator m_delayEstimator;


};

#endif // AEC_COMPONENT__HPP
