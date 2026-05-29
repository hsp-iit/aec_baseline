/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include "MicrophoneReader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

#include <yarp/os/LogStream.h>

namespace
{
void writeLittleEndian16(std::ostream &stream, std::uint16_t value)
{
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
}

void writeLittleEndian32(std::ostream &stream, std::uint32_t value)
{
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
    stream.put(static_cast<char>((value >> 16) & 0xff));
    stream.put(static_cast<char>((value >> 24) & 0xff));
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
} // namespace

MicrophoneReader::MicrophoneReader()
    : MicrophoneReader("/microphoneReader/audio:i")
{
}

MicrophoneReader::MicrophoneReader(const std::string &portName)
    : m_portName(portName),
      m_running(false),
      m_shouldExit(false),
      m_inputSampleRate(0)
{
}

MicrophoneReader::~MicrophoneReader()
{
    close();
}

bool MicrophoneReader::open()
{
    return open(m_portName);
}

bool MicrophoneReader::open(const std::string &portName)
{
    close();

    m_portName = portName;
    m_shouldExit = false;
    m_running = false;
    m_inputSampleRate = 0;

    if (!m_audioPort.open(m_portName))
    {
        yError() << "[MicrophoneReader::open] Unable to open microphone port:" << m_portName;
        return false;
    }

    m_readerThread = std::thread(&MicrophoneReader::readerThreadFunction, this);
    m_running = true;

    yInfo() << "[MicrophoneReader::open] Microphone port opened:" << m_portName;
    return true;
}

void MicrophoneReader::close()
{
    m_shouldExit = true;
    m_audioPort.close();

    if (m_readerThread.joinable())
    {
        m_readerThread.join();
    }

    m_running = false;
}

bool MicrophoneReader::isRunning() const
{
    return m_running.load();
}

std::size_t MicrophoneReader::bufferedSamples() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffer.size();
}

int MicrophoneReader::inputSampleRate() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inputSampleRate;
}

std::vector<short> MicrophoneReader::recordedSamples(int outputSampleRate) const
{
    std::vector<short> samples;
    int sampleRate = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        samples = m_buffer;
        sampleRate = m_inputSampleRate;
    }

    return resampleLinear(samples, sampleRate, outputSampleRate);
}

bool MicrophoneReader::saveAsWav(const std::filesystem::path &outputPath, int outputSampleRate) const
{
    const std::vector<short> samples = recordedSamples(outputSampleRate);

    std::ofstream outputFile(outputPath, std::ios::binary);
    if (!outputFile)
    {
        yError() << "[MicrophoneReader::saveAsWav] Unable to open output file:" << outputPath.string();
        return false;
    }

    const std::uint32_t sampleRate = static_cast<std::uint32_t>(outputSampleRate > 0 ? outputSampleRate : 16000);
    const std::uint16_t channels = 1;
    const std::uint16_t bitsPerSample = 16;
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bitsPerSample / 8));
    const std::uint32_t byteRate = sampleRate * blockAlign;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t riffChunkSize = 36 + dataSize;

    outputFile.write("RIFF", 4);
    writeLittleEndian32(outputFile, riffChunkSize);
    outputFile.write("WAVE", 4);
    outputFile.write("fmt ", 4);
    writeLittleEndian32(outputFile, 16);
    writeLittleEndian16(outputFile, 1);
    writeLittleEndian16(outputFile, channels);
    writeLittleEndian32(outputFile, sampleRate);
    writeLittleEndian32(outputFile, byteRate);
    writeLittleEndian16(outputFile, blockAlign);
    writeLittleEndian16(outputFile, bitsPerSample);
    outputFile.write("data", 4);
    writeLittleEndian32(outputFile, dataSize);

    for (short sample : samples)
    {
        writeLittleEndian16(outputFile, static_cast<std::uint16_t>(static_cast<std::int16_t>(sample)));
    }

    return static_cast<bool>(outputFile);
}

void MicrophoneReader::readerThreadFunction()
{
    while (!m_shouldExit)
    {
        yarp::sig::Sound *sound = m_audioPort.read(true);
        if (!sound)
        {
            if (m_shouldExit)
            {
                break;
            }
            continue;
        }

        appendBlock(*sound);
    }
}

void MicrophoneReader::appendBlock(const yarp::sig::Sound &sound)
{
    std::vector<short> blockSamples;
    blockSamples.reserve(static_cast<std::size_t>(sound.getSamples()));

    for (int index = 0; index < static_cast<int>(sound.getSamples()); ++index)
    {
        blockSamples.push_back(sound.get(index, 0));
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (sound.getFrequency() > 0)
    {
        m_inputSampleRate = sound.getFrequency();
    }
    m_buffer.insert(m_buffer.end(), blockSamples.begin(), blockSamples.end());

    yInfo() << "[MicrophoneReader::appendBlock] Buffered" << blockSamples.size() << "samples at" << sound.getFrequency() << "Hz. Total buffered samples:" << m_buffer.size();
}