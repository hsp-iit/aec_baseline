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

bool ReferenceReader::tryPopBlock(std::vector<short> &samples, int &sampleRate)
{
    // Drain one queued block in FIFO order.
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
    {
        return false;
    }

    ReferenceBlock block = std::move(m_queue.front());
    m_queue.pop_front();

    samples = std::move(block.samples);
    sampleRate = block.sampleRate;
    return true;
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
    // Block on the YARP port and enqueue every sound block that arrives.
    while (!m_shouldExit)
    {
        yarp::sig::Sound *sound = m_referencePort.read(true);

        yInfo() << "[ReferenceReader::readerThreadFunction] Received reference block with" << (sound ? sound->getSamples() : 0) << "samples at" << (sound ? sound->getFrequency() : 0) << "Hz";

        if (!sound)
        {
            if (m_shouldExit)
            {
                break;
            }
            continue;
        }

        enqueueBlock(*sound);
    }
}

void ReferenceReader::enqueueBlock(const yarp::sig::Sound &sound)
{
    std::vector<short> inputSamples;
    inputSamples.reserve(static_cast<std::size_t>(sound.getSamples()));
    for (int index = 0; index < static_cast<int>(sound.getSamples()); ++index)
    {
        inputSamples.push_back(sound.get(index, 0));
    }

    // Keep the incoming block at its native rate so the component thread can
    // align it to the microphone stream in real time.
    ReferenceBlock block;
    block.sampleRate = sound.getFrequency();
    const int sampleCount = static_cast<int>(inputSamples.size());
    block.samples.reserve(std::max(0, sampleCount));

    for (int index = 0; index < sampleCount; ++index)
    {
        block.samples.push_back(inputSamples[index]);
    }

    yInfo() << "[ReferenceReader::enqueueBlock] Enqueued reference block with" << block.samples.size() << "samples at" << block.sampleRate << "Hz";

    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(std::move(block));

    yInfo() << "[ReferenceReader::enqueueBlock] Added reference block to queue. Queue size is now" << m_queue.size();
}

yarp::sig::Sound ReferenceReader::getReferenceBlock(int index, int size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || size <= 0)
    {
        return yarp::sig::Sound();
    }

    std::vector<short> allSamples;
    for (const ReferenceBlock &block : m_queue)
    {
        allSamples.insert(allSamples.end(), block.samples.begin(), block.samples.end());
    }

    if (index >= static_cast<int>(allSamples.size()))
    {
        return yarp::sig::Sound();
    }

    const int endIndex = std::min(index + size, static_cast<int>(allSamples.size()));
    std::vector<short> refBlock(allSamples.begin() + index, allSamples.begin() + endIndex);

    yarp::sig::Sound sound;
    sound.resize(refBlock.size(), 1);
    for (int i = 0; i < static_cast<int>(refBlock.size()); ++i)
    {
        sound.set(refBlock[i], i, 0);
    }
    sound.setFrequency(m_queue.empty() ? 0 : m_queue.back().sampleRate);
    return sound;
}

yarp::sig::Sound ReferenceReader::getRecordedReferenceBlocks()
{
    // Concatenate all queued reference blocks into one Sound.
    std::vector<ReferenceBlock> blocks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        blocks = std::vector<ReferenceBlock>(m_queue.begin(), m_queue.end());
    }

    int totalSamples = 0;
    int sampleRate = 0;
    for (const ReferenceBlock &block : blocks)
    {
        totalSamples += static_cast<int>(block.samples.size());
        if (sampleRate <= 0 && block.sampleRate > 0)
        {
            sampleRate = block.sampleRate;
        }
    }

    yarp::sig::Sound sound;
    sound.resize(totalSamples, 1);
    int sampleIndex = 0;
    for (const ReferenceBlock &block : blocks)
    {
        for (short sample : block.samples)
        {
            if (sampleIndex < totalSamples)
            {
                sound.set(sampleIndex, 0, sample);
                ++sampleIndex;
            }
            else
            {
                break;
            }
        }
    }
    sound.setFrequency(sampleRate);
    return sound;
}

void ReferenceReader::flushStaledBlocks(int index)
{
    // Flush staled blocks from the queue based on a provided index, which can be used to discard old reference blocks that are no longer relevant for delay estimation.
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_queue.empty() && index > 0)
    {
        m_queue.pop_front();
        --index;
    }
}