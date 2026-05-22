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

#include <yarp/os/BufferedPort.h>
#include <yarp/sig/Sound.h>

class ReferenceReader
{
public:
    // Construct a reader for the default reference port.
    ReferenceReader();
    // Construct a reader that will listen on a custom YARP port.
    explicit ReferenceReader(const std::string &portName);
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
    // Pop one queued block if available and return its samples and sample rate.
    bool tryPopBlock(std::vector<short> &samples, int &sampleRate);
    // Return the currently configured YARP port name.
    std::string portName() const;

    // concurrent-safe method to flush staled blocks from the queue based on a provided index, which can be used to discard old reference blocks that are no longer relevant for delay estimation.
    void flushStaledBlocks(int index);

    yarp::sig::Sound getReferenceBlock(int index, int size);

    yarp::sig::Sound getRecordedReferenceBlocks();

private:
    struct ReferenceBlock
    {
        std::vector<short> samples;
        int sampleRate = 0;
    };

    void readerThreadFunction();
    void enqueueBlock(const yarp::sig::Sound &sound);

    std::string m_portName;
    yarp::os::BufferedPort<yarp::sig::Sound> m_referencePort;
    std::thread m_readerThread;
    mutable std::mutex m_mutex;
    std::deque<ReferenceBlock> m_queue;
    std::atomic<bool> m_running;
    std::atomic<bool> m_shouldExit;
};

#endif // REFERENCE_READER__HPP