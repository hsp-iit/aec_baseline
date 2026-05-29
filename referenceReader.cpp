/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include "referenceReader.hpp"

#include <algorithm>

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

std::vector<short> resampleLinear(const std::vector<short> &inputSamples,
                                  int inputSampleRate,
                                  int outputSampleRate)
{
    if (inputSamples.empty() || inputSampleRate <= 0 || outputSampleRate <= 0 || inputSampleRate == outputSampleRate)
    {
        return inputSamples;
    }

    const double ratio = static_cast<double>(outputSampleRate) / static_cast<double>(inputSampleRate);
    const std::size_t outputSize = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(inputSamples.size() * ratio)));
    std::vector<short> outputSamples(outputSize);

    for (std::size_t index = 0; index < outputSize; ++index)
    {
        const double sourcePosition = static_cast<double>(index) / ratio;
        const std::size_t leftIndex = static_cast<std::size_t>(std::floor(sourcePosition));
        const std::size_t rightIndex = std::min(leftIndex + 1, inputSamples.size() - 1);
        const double fraction = sourcePosition - static_cast<double>(leftIndex);

        const double leftSample = static_cast<double>(inputSamples[leftIndex]);
        const double rightSample = static_cast<double>(inputSamples[rightIndex]);
        const double interpolatedSample = leftSample + (rightSample - leftSample) * fraction;
        outputSamples[index] = static_cast<short>(std::lround(interpolatedSample));
    }

    return outputSamples;
}

void ReferenceReader::enqueueBlock(const yarp::sig::Sound &sound)
{

    std::vector<short> resampledSamples;
    std::vector<short> inputSamples;
    inputSamples.reserve(static_cast<std::size_t>(sound.getSamples()));
    for (int index = 0; index < static_cast<int>(sound.getSamples()); ++index)
    {        inputSamples.push_back(sound.get(index, 0));
    }

    int inputSampleRate = sound.getFrequency();
    int outputSampleRate = 16000; // AEC expects 16kHz reference
    if (inputSampleRate != outputSampleRate)
    {
        resampledSamples = resampleLinear(inputSamples, inputSampleRate, outputSampleRate);
    }
    else
    {   resampledSamples = inputSamples;
    }

    // Copy samples out of the YARP Sound and store them in the internal queue.
    ReferenceBlock block;
    block.sampleRate = outputSampleRate;
    const int sampleCount = static_cast<int>(resampledSamples.size());
    block.samples.reserve(std::max(0, sampleCount));

    for (int index = 0; index < sampleCount; ++index)
    {   
        block.samples.push_back(resampledSamples[index]);
    }

    yInfo() << "[ReferenceReader::enqueueBlock] Enqueued reference block with" << block.samples.size() << "samples at" << block.sampleRate << "Hz";

    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(std::move(block));

    yInfo() << "[ReferenceReader::enqueueBlock] Added reference block to queue. Queue size is now" << m_queue.size();
}

yarp::sig::Sound ReferenceReader::getReferenceBlock(int index, int size)
{
    // Get a specific reference block by index and size.
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= static_cast<int>(m_queue.size()))
    {
        return yarp::sig::Sound();
    }

    // concatenate all blocks into a single vector of shorts
    std::vector<short> allSamples;
    for (const ReferenceBlock &block : m_queue)
    {        allSamples.insert(allSamples.end(), block.samples.begin(), block.samples.end());
    }

    // TODO: change vector of reference blocks to single contiguous buffer to avoid this concatenation and allow more efficient retrieval of reference segments by index and size.
    
    std::vector<short>::const_iterator first = allSamples.begin() + index;
    std::vector<short>::const_iterator last = allSamples.begin() + index + std::max(static_cast<int>(allSamples.size()), size);
    std::vector<short> refBlock(first, last);
    
    // const ReferenceBlock &block = allSamples[index];
    yarp::sig::Sound sound;
    sound.resize(refBlock.size(), 1);
    for (int i = 0; i < size && i < static_cast<int>(refBlock.size()); ++i)
    {
        sound.set(i, 0, refBlock[i]);
    }
    sound.setFrequency(m_queue.empty() ? 0 : m_queue.front().sampleRate);
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