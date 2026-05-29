/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include "MicrophoneReader.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include <yarp/os/LogStream.h>
#include <yarp/os/Network.h>
#include <yarp/os/Time.h>

namespace
{
std::atomic<bool> g_shouldExit{false};

void signalHandler(int)
{
    g_shouldExit = true;
}

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
} // namespace

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    yarp::os::Network yarp;
    if (!yarp.checkNetwork())
    {
        yError() << "[microphoneRecorder] YARP network is not available";
        return EXIT_FAILURE;
    }

    MicrophoneReader reader;
    if (!reader.open())
    {
        yError() << "[microphoneRecorder] Failed to open microphone reader";
        return EXIT_FAILURE;
    }

    yInfo() << "[microphoneRecorder] Listening on" << "/audioRecorderWrapper_nws_ros2/audio:o";
    yInfo() << "[microphoneRecorder] Press ctrl+c to save the recorded buffer and exit";

    while (!g_shouldExit.load())
    {
        yarp::os::Time::delay(0.1);
    }

    reader.close();

    const std::filesystem::path outputDirectory("./microphone-recordings");
    std::error_code errorCode;
    std::filesystem::create_directories(outputDirectory, errorCode);
    if (errorCode)
    {
        yError() << "[microphoneRecorder] Unable to create output directory:" << outputDirectory.string() << errorCode.message();
        return EXIT_FAILURE;
    }

    const auto outputPath = outputDirectory / ("microphone_capture_" + makeTimestampString(std::chrono::system_clock::now()) + ".wav");
    if (!reader.saveAsWav(outputPath, 16000))
    {
        yError() << "[microphoneRecorder] Failed to save recorded buffer";
        return EXIT_FAILURE;
    }

    yInfo() << "[microphoneRecorder] Saved" << reader.bufferedSamples() << "samples to" << outputPath.string() << "at 16kHz";
    return EXIT_SUCCESS;
}