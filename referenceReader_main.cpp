/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include "referenceReader.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

#include <yarp/os/LogStream.h>
#include <yarp/os/Network.h>
#include <yarp/os/Time.h>

namespace
{
std::atomic<bool> g_shouldExit{false};

void signalHandler(int)
{
    // Convert SIGINT/SIGTERM into a cooperative shutdown request.
    g_shouldExit = true;
}

// Write a 16-bit integer in little-endian order for WAV headers.
void writeLittleEndian16(std::ostream &stream, std::uint16_t value)
{
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
}

// Write a 32-bit integer in little-endian order for WAV headers.
void writeLittleEndian32(std::ostream &stream, std::uint32_t value)
{
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
    stream.put(static_cast<char>((value >> 16) & 0xff));
    stream.put(static_cast<char>((value >> 24) & 0xff));
}

// Format a timestamp that can be embedded into filenames.
std::string makeTimestampString(const std::chrono::system_clock::time_point &timePoint)
{
    const auto time = std::chrono::system_clock::to_time_t(timePoint);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timePoint.time_since_epoch()) % std::chrono::seconds(1);

    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y%m%d_%H%M%S")
           << '_' << std::setw(3) << std::setfill('0') << milliseconds.count();
    return stream.str();
}

bool saveWavFile(const std::filesystem::path &outputPath,
                 const std::vector<short> &samples,
                 int sampleRate,
                 int channelCount)
{
    // Serialize a mono or multi-channel PCM WAV file to disk.
    std::ofstream outputFile(outputPath, std::ios::binary);
    if (!outputFile)
    {
        yError() << "[referenceReader] Unable to open output file:" << outputPath.string();
        return false;
    }

    const std::uint32_t outputSampleRate = static_cast<std::uint32_t>(sampleRate > 0 ? sampleRate : 48000);
    const std::uint16_t channels = static_cast<std::uint16_t>(channelCount > 0 ? channelCount : 1);
    const std::uint16_t bitsPerSample = 16;
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bitsPerSample / 8));
    const std::uint32_t byteRate = outputSampleRate * blockAlign;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t riffChunkSize = 36 + dataSize;

    outputFile.write("RIFF", 4);
    writeLittleEndian32(outputFile, riffChunkSize);
    outputFile.write("WAVE", 4);
    outputFile.write("fmt ", 4);
    writeLittleEndian32(outputFile, 16);
    writeLittleEndian16(outputFile, 1);
    writeLittleEndian16(outputFile, channels);
    writeLittleEndian32(outputFile, outputSampleRate);
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
} // namespace

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // Install signal handlers before any blocking work starts.
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Make sure YARP is reachable before opening the reader port.
    yarp::os::Network yarp;
    if (!yarp.checkNetwork())
    {
        yError() << "[referenceReader] YARP network is not available";
        return EXIT_FAILURE;
    }

    ReferenceReader reader;
    if (!reader.open())
    {
        yError() << "[referenceReader] Failed to open reference reader";
        return EXIT_FAILURE;
    }

    // Run until the user presses ctrl+c, then drain and save everything collected.
    yInfo() << "[referenceReader] Listening on" << reader.portName();
    yInfo() << "[referenceReader] Press ctrl+c to save queued reference audio and exit";

    while (!g_shouldExit.load())
    {
        yarp::os::Time::delay(0.1);
    }

    const auto saveStart = std::chrono::steady_clock::now();

    // Stop incoming reads before draining the queue to disk.
    reader.close();

    // Concatenate every queued reference block into one PCM buffer.
    std::vector<short> queuedSamples;
    int sampleRate = 0;
    std::vector<short> blockSamples;
    int blockSampleRate = 0;
    while (reader.tryPopBlock(blockSamples, blockSampleRate))
    {
        if (sampleRate <= 0 && blockSampleRate > 0)
        {
            sampleRate = blockSampleRate;
        }
        queuedSamples.insert(queuedSamples.end(), blockSamples.begin(), blockSamples.end());
    }

    const auto outputDirectory = std::filesystem::path("./reference-reader-recordings");
    std::error_code errorCode;
    std::filesystem::create_directories(outputDirectory, errorCode);
    if (errorCode)
    {
        yError() << "[referenceReader] Unable to create output directory:" << outputDirectory.string() << errorCode.message();
        return EXIT_FAILURE;
    }

    // Use the current time in the filename so each capture is unique.
    const auto captureTime = std::chrono::system_clock::now();
    const auto outputPath = outputDirectory / ("reference_queue_" + makeTimestampString(captureTime) + ".wav");
    if (!saveWavFile(outputPath, queuedSamples, sampleRate, 1))
    {
        yError() << "[referenceReader] Failed to save queued reference audio";
        return EXIT_FAILURE;
    }

    // Report how long the flush-to-disk step took.
    const auto saveDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - saveStart)
                                    .count();
    yInfo() << "[referenceReader] Saved" << queuedSamples.size() << "samples to" << outputPath.string()
            << "in" << saveDurationMs << "ms";

    return EXIT_SUCCESS;
}