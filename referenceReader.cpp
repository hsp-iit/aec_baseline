/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include "referenceReader.hpp"

#include <yarp/os/LogStream.h>

ReferenceReader::ReferenceReader()
    : ReferenceReader("/aecComponent/reference:i", false)
{
}

ReferenceReader::ReferenceReader(const std::string &portName, bool logAecStats)
    : m_portName(portName),
      m_running(false),
      m_shouldExit(false),
      m_logAecStats(logAecStats)
{
    m_slidingWindowIndex = 0;
    m_audioPlayerDelayMs = 0.0f;

    yInfo() << "[ReferenceReader::ReferenceReader] Constructed ReferenceReader for port:" << m_portName;
    yInfo() << "[ReferenceReader::ReferenceReader] Logging AEC stats:" << (m_logAecStats ? "ENABLED" : "DISABLED");
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

void ReferenceReader::setLogAecStats(bool logAecStats)
{
    m_logAecStats = logAecStats;
}

void ReferenceReader::readerThreadFunction()
{
    while (!m_shouldExit)
    {
        // Block on the YARP port and enqueue every sound block that arrives.

        yarp::sig::Sound *sound = m_referencePort.read(true);

        if (m_logAecStats)
        {

            yInfo() << "[ReferenceReader::readerThreadFunction] Received reference block with"
                    << (sound ? sound->getSamples() : 0)
                    << "samples at"
                    << (sound ? sound->getFrequency() : 0)
                    << "Hz";
        }

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

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch())
                          .count();

        if (m_logAecStats)
        {

            yInfo() << "[ReferenceReader::readerThreadFunction] Current time:"
                    << now_ms << "ms since epoch";

            yInfo() << "[ReferenceReader::readerThreadFunction] Sound duration:"
                    << soundSeconds << "seconds";

            yInfo() << "[ReferenceReader::readerThreadFunction] Enqueued reference block with" << sound->getSamples() << "samples at" << sound->getFrequency() << "Hz";
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(*sound);
        }

        if (m_logAecStats)
        {
            yInfo() << "[ReferenceReader::readerThreadFunction] Added reference block to queue. Queue size is now" << m_queue.size();
        }
    }
}

yarp::sig::Sound ReferenceReader::getRecordedReferenceBlocks()
{

    if (m_queue.empty())
    {
        if (m_logAecStats)
        {
            yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] Reference queue is empty, returning empty Sound";
        }
        return yarp::sig::Sound();
    }

    if (!isPlayerActive())
    {
        if (m_logAecStats)
        {
            yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] Audio player is not active, returning empty Sound";
        }
        return yarp::sig::Sound();
    }

    if (m_queue.front().getSamples() <= m_slidingWindowIndex)
    {
        m_slidingWindowIndex = 0;

        std::lock_guard<std::mutex> lock(m_mutex);
        // remove the first element of the m_queue
        m_queue.pop_front();
        if (m_logAecStats)
        {
            yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] Removed first reference block from queue due to sliding window index exceeding its size. Queue size is now" << m_queue.size();
        }
    }

    std::deque<yarp::sig::Sound> blocks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        blocks = std::deque<yarp::sig::Sound>(m_queue.begin(), m_queue.end());
    }

    yarp::sig::Sound blockNeighborhood;
    yarp::sig::Sound firstBlock;
    if (!blocks.empty())
    {
        firstBlock = blocks.front();
        int soundFrequency = firstBlock.getFrequency();

        // use consumedSamples to get a neighborhood of size 4000 samples at 16000 Hz from the first block, starting from the sample that has not been consumed yet. If the block has sound frequency different than 16000 Hz, scale the neighborhood size accordingly. If the block has less than 8000 samples remaining, return all remaining samples.
        int neighborhoodSize = static_cast<int>(4000 * soundFrequency / 16000);

        int startSample = m_slidingWindowIndex;

        if (m_logAecStats)
        {
            yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] First block has" << firstBlock.getSamples() << "samples at" << soundFrequency << "Hz, neighborhood size:" << neighborhoodSize << ", start sample:" << startSample;
        }
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

        if (static_cast<int>(blockNeighborhood.getSamples()) < neighborhoodSize)
        {
            int samplesToAppend = neighborhoodSize - static_cast<int>(blockNeighborhood.getSamples());
            if (m_logAecStats)
            {
                yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] Block neighborhood has less than" << neighborhoodSize << "samples, attempting to append next block in queue if it exists";
            }
            if (blocks.size() > 1)
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    // populate nextBlock with the first samplesToAppend samples of the second block in the queue, starting from sample 0
                    yarp::sig::Sound nextBlock = blocks.at(1).subSound(0, samplesToAppend);
                    m_queue.at(1) = blocks.at(1).subSound(samplesToAppend, blocks.at(1).getSamples());
                }
                blockNeighborhood.resize(neighborhoodSize, firstBlock.getChannels());
                for (int i = 0; i < samplesToAppend; ++i)
                {
                    for (int j = 0; j < firstBlock.getChannels(); ++j)
                    {
                        blockNeighborhood.set(blocks.at(1).get(i, j), static_cast<int>(blockNeighborhood.getSamples()) - samplesToAppend + i, j);
                    }
                }
            }
            else // append null samples to blockNeighborhood until it has neighborhoodSize samples
            {
                if (m_logAecStats)
                {
                    yInfo() << "[ReferenceReader::getRecordedReferenceBlocks] No next block in queue, appending null samples to block neighborhood";
                }
                int currentSamples = static_cast<int>(blockNeighborhood.getSamples());
                blockNeighborhood.resize(neighborhoodSize, firstBlock.getChannels());
                for (int i = currentSamples; i < neighborhoodSize; ++i)
                {
                    for (int j = 0; j < firstBlock.getChannels(); ++j)
                    {
                        blockNeighborhood.set(0, i, j);
                    }
                }
            }
        }
    }

    return blocks.empty() ? yarp::sig::Sound() : blockNeighborhood;
}

bool ReferenceReader::isPlayerActive()
{
    yarp::sig::AudioPlayerStatus *status = m_statusPort.read(false);
    if (status && status->current_buffer_size > 0)
    {
        // std::string statusStr = status->get(0).asString();
        int firstBlockSize = 0;
        if (m_logAecStats)
        {
            yInfo() << "[ReferenceReader::isPlayerActive] Received audio player status with" << status->current_buffer_size << "elements, assuming active";
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_queue.empty())
            {
                firstBlockSize = m_queue.front().getSamples();
                if (m_logAecStats)
                {
                    yInfo() << "[ReferenceReader::isPlayerActive] First reference block in queue has" << firstBlockSize << "samples";
                }
            }
            else
            {
                firstBlockSize = 0;
                if (m_logAecStats)
                {
                    yInfo() << "[ReferenceReader::isPlayerActive] Reference queue is empty";
                }
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
        if (m_logAecStats)
        {
            yInfo() << "[ReferenceReader::isPlayerActive] No audio player status received, assuming inactive";
        }
        if (m_slidingWindowIndex > 0)
        {
            if (m_logAecStats)
            {
                yInfo() << "[ReferenceReader::isPlayerActive] Resetting sliding window index to 0";
                yInfo() << "[ReferenceReader::isPlayerActive] Removing samples from queue:" << m_slidingWindowIndex;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_slidingWindowIndex = 0;
            if (!m_queue.empty())
            {
                m_queue.pop_front();
                if (m_logAecStats)
                {
                    yInfo() << "[ReferenceReader::isPlayerActive] Removed first reference block from queue. Queue size is now" << m_queue.size();
                }
            }
        }
        return false;
    }
}

float ReferenceReader::getLastEstimatedAudioPlayerDelay()
{
    return m_audioPlayerDelayMs;
}