/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#ifndef MICROPHONE_READER__HPP
#define MICROPHONE_READER__HPP

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <yarp/os/BufferedPort.h>
#include <yarp/sig/Sound.h>

class MicrophoneReader
{
public:
    MicrophoneReader();
    explicit MicrophoneReader(const std::string &portName);
    ~MicrophoneReader();

    bool open();
    bool open(const std::string &portName);
    void close();

    bool isRunning() const;
    std::size_t bufferedSamples() const;
    int inputSampleRate() const;

    std::vector<short> recordedSamples(int outputSampleRate = 16000) const;
    bool saveAsWav(const std::filesystem::path &outputPath, int outputSampleRate = 16000) const;

private:
    void readerThreadFunction();
    void appendBlock(const yarp::sig::Sound &sound);

    std::string m_portName;
    yarp::os::BufferedPort<yarp::sig::Sound> m_audioPort;
    std::thread m_readerThread;
    mutable std::mutex m_mutex;
    std::vector<short> m_buffer;
    std::atomic<bool> m_running;
    std::atomic<bool> m_shouldExit;
    int m_inputSampleRate;
};

#endif // MICROPHONE_READER__HPP