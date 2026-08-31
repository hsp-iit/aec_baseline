/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include "referenceReader.hpp"

#include <yarp/os/LogStream.h>

ReferenceReader::ReferenceReader()
    : ReferenceReader("/aecComponent/reference:i")
{
}

ReferenceReader::ReferenceReader(const std::string &portName)
    : m_portName(portName),
      m_running(false),
      m_shouldExit(false)
{
    m_slidingWindowIndex = 0;
    m_audioPlayerDelayMs = 0.0f;
}

ReferenceReader::~ReferenceReader()
{
    // Make shutdown safe even if the caller forgets to call close().
    close();
}

bool ReferenceReader::open()
{
    // Reuse the configured default port name.
    return open(m_portName);
}

bool ReferenceReader::open(const std::string &portName)
{
    // If the reader was already running, stop it before reopening.
    close();

    m_portName = portName;
    m_shouldExit = false;
    m_running = false;

    if (!m_referencePort.open(m_portName))
    {
        yError() << "[ReferenceReader::open] Unable to open reference port:" << m_portName;
        return false;
    }

    if (!m_statusPort.open("/aecComponent/audioPlayerStatus:i"))
    {
        yError() << "[ReferenceReader::open] Unable to open status port: /aecComponent/audioPlayerStatus:i";
        return false;
    }

    // Start a dedicated blocking reader thread so incoming data is queued.
    m_readerThread = std::thread(&ReferenceReader::readerThreadFunction, this);
    m_running = true;

    yInfo() << "[ReferenceReader::open] Reference port opened:" << m_portName;
    return true;
}

void ReferenceReader::close()
{
    // Tell the reader loop to exit, then close the port so blocking reads unblock.
    m_shouldExit = true;

    m_referencePort.close();

    if (m_readerThread.joinable())
    {
        m_readerThread.join();
    }

    m_running = false;
}

bool ReferenceReader::isRunning() const
{
    // Expose the thread state without taking the mutex.
    return m_running.load();
}

std::size_t ReferenceReader::queuedBlocks() const
{
    // Read the queue size under lock so the count is consistent.
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

std::string ReferenceReader::portName() const
{
    // Return the port name currently associated with this reader.
    return m_portName;
}

void ReferenceReader::readerThreadFunction()
{
    while (!m_shouldExit)
    {
        // Block on the YARP port and enqueue every sound block that arrives.

        yarp::sig::Sound *sound = m_referencePort.read(true);

        yInfo() << "[ReferenceReader::readerThreadFunction] Received reference block with"
                << (sound ? sound->getSamples() : 0)
                << "samples at"
                << (sound ? sound->getFrequency() : 0)
                << "Hz";

        if (!sound)
        {
            if (m_shouldExit)
            {
                break;
            }
            continue;
        }

        auto now = std::chrono::system_clock::now();

        double soundSeconds = static_cast<double>(sound->getSamples()) / sound->getFrequency();

        // std::chrono::seconds soundSeconds = std::chrono::seconds(static_cast<int>(sound->getSamples()) / sound->getFrequency());
        // record in a variable called soundTTL the time when the sound will be considered expired, which is now + soundSeconds if 
        // the sound list is empty, or the last timestamp in the queue + soundSeconds if the sound list is not empty
        std::chrono::time_point<std::chrono::system_clock> soundTTL;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_timestamps.empty())
            {
                soundTTL = now + std::chrono::milliseconds(static_cast<int>(soundSeconds * 1000));
            }
            else
            {
                soundTTL = m_timestamps.back() + std::chrono::milliseconds(static_cast<int>(soundSeconds * 1000));
            }
        }

        // auto now = std::chrono::system_clock::now();

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch())
                          .count();

        auto ttl_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          soundTTL.time_since_epoch())
                          .count();

        yInfo() << "[ReferenceReader::readerThreadFunction] Current time:"
                << now_ms << "ms since epoch";

        yInfo() << "[ReferenceReader::readerThreadFunction] Sound duration:"
                << soundSeconds << "seconds";

        yInfo() << "[ReferenceReader::readerThreadFunction] Sound TTL:"
                << ttl_ms << "ms since epoch";

        yInfo() << "[ReferenceReader::readerThreadFunction] Enqueued reference block with" << sound->getSamples() << "samples at" << sound->getFrequency() << "Hz";

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(*sound);
            m_timestamps.push_back(soundTTL);
        }

        yInfo() << "[ReferenceReader::readerThreadFunction] Added reference block to queue. Queue size is now" << m_queue.size();
    }
}

yarp::sig::Sound ReferenceReader::getRecordedReferenceBlocks()
{
    // auto now = std::chrono::system_clock::now();
    // // remove stale reference blocks from the queue before returning the first one
    // deleteStaleReferenceBlocks();
    if (m_queue.empty())
    {
        yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] Reference queue is empty, returning empty Sound";
        return yarp::sig::Sound();
    }

    if (!isPlayerActive())
    {
        yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] Audio player is not active, returning empty Sound";
        return yarp::sig::Sound();
    }

    if (m_queue.front().getSamples() <= m_slidingWindowIndex)
    {
        m_slidingWindowIndex = 0;

        std::lock_guard<std::mutex> lock(m_mutex);
        // m_timestamps.erase(m_timestamps.begin(), m_timestamps.begin() + maxIndex + 1);
        // m_queue.erase(m_queue.begin(), m_queue.begin() + maxIndex + 1);
        // remove the first element of the m_queue
        m_queue.pop_front();
        m_timestamps.pop_front();
        yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] Removed first reference block from queue due to sliding window index exceeding its size. Queue size is now" << m_queue.size();

    }
    

    // Return the first audio in the queue that has a valid timestamp, or an empty Sound if the queue is empty.
    std::deque<yarp::sig::Sound> blocks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        blocks = std::deque<yarp::sig::Sound>(m_queue.begin(), m_queue.end());
    }

    // return a neighborhood of the first block in the queue that has a valid timestamp, or an empty Sound if the queue is empty
    // Note: the first block is the oldest one in the queue, having a timestamp that is less than or equal to the current time.
    // this means that ideally it has not been completely consumed yet, and may be still be played by the audio player
    yarp::sig::Sound blockNeighborhood;
    yarp::sig::Sound firstBlock;
    if (!blocks.empty())
    {
        firstBlock = blocks.front();
        int soundFrequency = firstBlock.getFrequency();
        // // compute the time passed since the first block was recorded, and compute how many samples have not been consumed yet, and return a neighborhood of the first block that has a valid timestamp, starting from the sample that has not been consumed yet
        
        // auto firstBlockTimestamp = m_timestamps.front();
        // auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(firstBlockTimestamp - now).count();

        // int consumedSamples = static_cast<int>(firstBlock.getSamples()) - static_cast<int>(remaining_ms * soundFrequency / 1000);

        // use consumedSamples to get a neighborhood of size 8000 samples at 16000 Hz from the first block, starting from the sample that has not been consumed yet. If the block has sound frequency different than 16000 Hz, scale the neighborhood size accordingly. If the block has less than 8000 samples remaining, return all remaining samples.
        int neighborhoodSize = static_cast<int>(4000 * soundFrequency / 16000);
        // int startSample = std::max(0, static_cast<int>(consumedSamples));
        int startSample = m_slidingWindowIndex;

        // yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] First block has" << firstBlock.getSamples() << "samples at" << soundFrequency << "Hz, consumed samples:" << consumedSamples << ", neighborhood size:" << neighborhoodSize << ", start sample:" << startSample;
        yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] First block has" << firstBlock.getSamples() << "samples at" << soundFrequency << "Hz, neighborhood size:" << neighborhoodSize << ", start sample:" << startSample;
        int endSample = std::min(static_cast<int>(firstBlock.getSamples()), startSample + neighborhoodSize);
        m_slidingWindowIndex += endSample - startSample;

        blockNeighborhood.resize(endSample - startSample, firstBlock.getChannels());
        blockNeighborhood.setFrequency(soundFrequency);
        for (int i = startSample; i < endSample; ++i)
        {
            for (int j = 0; j < firstBlock.getChannels(); ++j)
            {
                blockNeighborhood.set(firstBlock.get(i, j), i - startSample, j);
            }
        }

    }

    return blocks.empty() ? yarp::sig::Sound() : blockNeighborhood;
}

void ReferenceReader::deleteStaleReferenceBlocks()
{

    auto now = std::chrono::system_clock::now();
    int maxIndex = -1;

    // find the index of the last timestamp that is less than or equal to the current time, and remove all timestamps and corresponding audio blocks before that index
    for (std::size_t i = 0; i < m_timestamps.size(); ++i)
    {
        const auto &timestamp = m_timestamps[i];

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch())
                          .count();

        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         timestamp.time_since_epoch())
                         .count();

        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                timestamp - now)
                                .count();

        yInfo() << "[ReferenceReader] now =" << now_ms << "ms,"
                << "timestamp =" << ts_ms << "ms,"
                << "remaining =" << remaining_ms << "ms";

        if (now > timestamp)
        {
            maxIndex = static_cast<int>(i);
            yInfo() << "[ReferenceReader] Timestamp at index" << i << "has expired";
            m_slidingWindowIndex = 0;
        }
        else
        {
            yInfo() << "[ReferenceReader] First element still valid.";
            break;
        }
    }

    if (maxIndex >= 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_timestamps.erase(m_timestamps.begin(), m_timestamps.begin() + maxIndex + 1);
        m_queue.erase(m_queue.begin(), m_queue.begin() + maxIndex + 1);
        yInfo() << "[ReferenceReader] Removed" << (maxIndex + 1) << "expired reference blocks from queue. Queue size is now" << m_queue.size();
    }
}

// void ReferenceReader::updateSlidingWindowIndex(int index)
// {   
//     yInfo() << "[ReferenceReader::updateSlidingWindowIndex] Updating sliding window index by" << index << "samples. Previous index:" << m_slidingWindowIndex;
//     std::lock_guard<std::mutex> lock(m_mutex);
//     m_slidingWindowIndex = m_slidingWindowIndex + index;
//     yInfo() << "[ReferenceReader::updateSlidingWindowIndex] New sliding window index:" << m_slidingWindowIndex;
// }

bool ReferenceReader::isPlayerActive()
{
    yarp::sig::AudioPlayerStatus *status = m_statusPort.read(false);
    // yInfo() << "[ReferenceReader::isPlayerActive] Checking audio player status: " << (status ->current_buffer_size) << "elements in buffer";
    if (status && status->current_buffer_size > 0)
    {
        // std::string statusStr = status->get(0).asString();
        int firstBlockSize = 0;
        yInfo() << "[ReferenceReader::isPlayerActive] Received audio player status with" << status->current_buffer_size << "elements, assuming active";
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_queue.empty())
            {
                firstBlockSize = m_queue.front().getSamples();
                yInfo() << "[ReferenceReader::isPlayerActive] First reference block in queue has" << firstBlockSize << "samples";
            }
            else
            {
                firstBlockSize = 0;
                yInfo() << "[ReferenceReader::isPlayerActive] Reference queue is empty";
            }
        }

        int audioPlayerDelaySamples = firstBlockSize - static_cast<int>(status->current_buffer_size) - static_cast<int>(m_slidingWindowIndex);
        if (audioPlayerDelaySamples > 0)
        {
            m_audioPlayerDelayMs = static_cast<float>(audioPlayerDelaySamples) * 1000.0f / 24000.0f; // Assuming 24kHz sample rate for delay calculation
        }

        return true;
    }
    else
    {
        yInfo() << "[ReferenceReader::isPlayerActive] No audio player status received, assuming inactive";
        if (m_slidingWindowIndex > 0) 
        {
            yInfo() << "[ReferenceReader::isPlayerActive] Resetting sliding window index to 0";
            yInfo() << "[ReferenceReader::isPlayerActive] Removing samples from queue:" << m_slidingWindowIndex;
            std::lock_guard<std::mutex> lock(m_mutex);
            m_slidingWindowIndex = 0;
            if (!m_queue.empty())
            {
                m_queue.pop_front();
                m_timestamps.pop_front();
                yInfo() << "[ReferenceReader::isPlayerActive] Removed first reference block from queue. Queue size is now" << m_queue.size();
            }
        }
        return false;
    }
}


float ReferenceReader::getLastEstimatedAudioPlayerDelay()
{
    return m_audioPlayerDelayMs;
}