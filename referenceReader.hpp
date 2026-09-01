/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#ifndef REFERENCE_READER__HPP
#define REFERENCE_READER__HPP

#include <atomic>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <chrono>

#include <yarp/os/BufferedPort.h>
#include <yarp/sig/Sound.h>
#include <yarp/sig/AudioPlayerStatus.h>

class ReferenceReader
{
public:
    // Construct a reader for the default reference port.
    ReferenceReader();
    // Construct a reader that will listen on a custom YARP port.
    explicit ReferenceReader(const std::string &portName, bool logAecStats = false);
    // Stop the reader thread and release the port.
    ~ReferenceReader();

    // Open the configured port and start the background reader thread.
    bool open();
    // Open a specific port and start the background reader thread.
    bool open(const std::string &portName);
    // Stop the background thread and close the YARP port.
    void close();

    // Return whether the reader thread is currently running.
    bool isRunning() const;
    // Return the number of queued audio blocks waiting to be consumed.
    std::size_t queuedBlocks() const;
    // Return the currently configured YARP port name.
    std::string portName() const;

    void setLogAecStats(bool logAecStats);


    bool isPlayerActive();

    yarp::sig::Sound getRecordedReferenceBlocks();

    float getLastEstimatedAudioPlayerDelay();

private:

    void readerThreadFunction();

    bool m_logAecStats;

    std::string m_portName;
    yarp::os::BufferedPort<yarp::sig::Sound> m_referencePort;
    std::thread m_readerThread;
    mutable std::mutex m_mutex;
    std::deque<yarp::sig::Sound> m_queue;
    std::atomic<bool> m_running;
    std::atomic<bool> m_shouldExit;

    yarp::os::BufferedPort<yarp::sig::AudioPlayerStatus> m_statusPort;

    float m_audioPlayerDelayMs;

    int m_slidingWindowIndex;
};

#endif // REFERENCE_READER__HPP