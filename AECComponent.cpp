/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include "AECComponent.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <algorithm>
#include <yarp/os/Time.h>

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
}

AECComponent::AECComponent()
    : m_apm(nullptr),
      m_streamConfig(nullptr),
      m_shouldExit(false),
      m_sample_rate(48000),
      m_num_channels(1),
      m_block_ms(10),
      m_saveIterativeAudioToDisk(false),
            m_audioSaveMaxSeconds(30),
      m_audioSaveDirectory("./aec-recordings"),
      m_audioSavePrefix("aec_output"),
            m_audioSaveSequence(0),
            m_aecMobileMode(false),
            m_aecStreamDelayMs(120),
            m_logAecStats(true),
            m_aecFramesWithReference(0),
            m_aecFramesWithoutReference(0),
            m_lastAecStatsLog(std::chrono::steady_clock::now())
{
    // m_referenceReader = ReferenceReader();
    m_delayEstimator = DelayEstimator();
}

AECComponent::~AECComponent()
{
    close();
}

bool AECComponent::configure(yarp::os::ResourceFinder &rf)
{
    yInfo() << "[AECComponent::configure] Configuring component";

    // Parse configuration
    bool okCheck = rf.check("AEC_COMPONENT");
    std::string microphoneInputPortName = "/aecComponent/microphone:i";
    std::string referenceInputPortName = "/aecComponent/reference:i";
    std::string audioOutputPortName = "/aecComponent/audio:o";

    if (okCheck)
    {
        yarp::os::Searchable &aecConfig = rf.findGroup("AEC_COMPONENT");
        
        if (aecConfig.check("microphoneInputPort"))
        {
            microphoneInputPortName = aecConfig.find("microphoneInputPort").asString();
        }
        if (aecConfig.check("referenceInputPort"))
        {
            referenceInputPortName = aecConfig.find("referenceInputPort").asString();
        }
        if (aecConfig.check("audioOutputPort"))
        {
            audioOutputPortName = aecConfig.find("audioOutputPort").asString();
        }
        if (aecConfig.check("sample_rate"))
        {
            m_sample_rate = aecConfig.find("sample_rate").asInt32();
        }
        if (aecConfig.check("num_channels"))
        {
            m_num_channels = aecConfig.find("num_channels").asInt32();
        }
        if (aecConfig.check("block_ms"))
        {
            m_block_ms = aecConfig.find("block_ms").asInt32();
        }
        if (aecConfig.check("saveIterativeAudioToDisk"))
        {
            m_saveIterativeAudioToDisk = aecConfig.find("saveIterativeAudioToDisk").asBool();
        }
        if (aecConfig.check("audioSaveDirectory"))
        {
            m_audioSaveDirectory = aecConfig.find("audioSaveDirectory").asString();
        }
        if (aecConfig.check("audioSavePrefix"))
        {
            m_audioSavePrefix = aecConfig.find("audioSavePrefix").asString();
        }
        if (aecConfig.check("audioSaveMaxSeconds"))
        {
            m_audioSaveMaxSeconds = aecConfig.find("audioSaveMaxSeconds").asInt32();
        }
        if (aecConfig.check("aec_mobile_mode"))
        {
            m_aecMobileMode = aecConfig.find("aec_mobile_mode").asBool();
        }
        if (aecConfig.check("aec_stream_delay_ms"))
        {
            m_aecStreamDelayMs = aecConfig.find("aec_stream_delay_ms").asInt32();
        }
        if (aecConfig.check("logAecStats"))
        {
            m_logAecStats = aecConfig.find("logAecStats").asBool();
        }
    }

    if (m_audioSaveMaxSeconds <= 0)
    {
        yWarning() << "[AECComponent::configure] audioSaveMaxSeconds must be > 0, falling back to 30";
        m_audioSaveMaxSeconds = 30;
    }
    if (m_aecStreamDelayMs < 0)
    {
        yWarning() << "[AECComponent::configure] aec_stream_delay_ms must be >= 0, clamping to 0";
        m_aecStreamDelayMs = 0;
    }

    yInfo() << "[AECComponent::configure] Sample rate:" << m_sample_rate << "Hz";
    yInfo() << "[AECComponent::configure] Channels:" << m_num_channels;
    yInfo() << "[AECComponent::configure] Block size:" << m_block_ms << "ms";
    yInfo() << "[AECComponent::configure] Iterative audio save:" << (m_saveIterativeAudioToDisk ? "ENABLED" : "DISABLED");
    if (m_saveIterativeAudioToDisk)
    {
        yInfo() << "[AECComponent::configure] Audio save directory:" << m_audioSaveDirectory;
        yInfo() << "[AECComponent::configure] Audio save prefix:" << m_audioSavePrefix;
        yInfo() << "[AECComponent::configure] Max saved file length:" << m_audioSaveMaxSeconds << "seconds";
    }
    yInfo() << "[AECComponent::configure] AEC mobile mode:" << (m_aecMobileMode ? "ENABLED" : "DISABLED");
    yInfo() << "[AECComponent::configure] AEC stream delay:" << m_aecStreamDelayMs << "ms";
    yInfo() << "[AECComponent::configure] AEC stats log:" << (m_logAecStats ? "ENABLED" : "DISABLED");

    // Initialize AEC
    if (!initializeAEC(m_sample_rate, m_num_channels))
    {
        yError() << "[AECComponent::configure] Failed to initialize AEC";
        return false;
    }

    // Open input ports - Microphone
    if (!m_microphoneAudioInputPort.open(microphoneInputPortName))
    {
        yError() << "[AECComponent::configure] Unable to open microphone audio input port: " << microphoneInputPortName;
        return false;
    }
    yInfo() << "[AECComponent::configure] Microphone audio input port opened: " << microphoneInputPortName;

    // Open reference reader port via ReferenceReader
    if (!m_referenceReader.open(referenceInputPortName))
    {
        yError() << "[AECComponent::configure] Unable to open reference input port: " << referenceInputPortName;
        return false;
    }
    yInfo() << "[AECComponent::configure] Reference input opened: " << referenceInputPortName;

    // Open output port
    if (!m_audioOutputPort.open(audioOutputPortName))
    {
        yError() << "[AECComponent::configure] Unable to open audio output port: " << audioOutputPortName;
        return false;
    }
    yInfo() << "[AECComponent::configure] Audio output port opened: " << audioOutputPortName;

    // Start processing thread
    m_shouldExit = false;
    m_processingThread = std::thread(&AECComponent::processingThreadFunction, this);

    yInfo() << "[AECComponent::configure] AEC Component configured successfully";
    return true;
}

bool AECComponent::initializeAEC(int sample_rate, int num_channels)
{
    try
    {
        m_apm = webrtc::AudioProcessingBuilder().Create();
        if (!m_apm)
        {
            yError() << "[AECComponent::initializeAEC] Failed to create AudioProcessing instance";
            return false;
        }

        // Configure AEC settings
        webrtc::AudioProcessing::Config config;
        
        // Echo Cancellation
        config.echo_canceller.enabled = true;
        config.echo_canceller.mobile_mode = m_aecMobileMode;

        // Gain Control 1 (Analog)
        config.gain_controller1.enabled = true;
        config.gain_controller1.mode = 
            webrtc::AudioProcessing::Config::GainController1::kAdaptiveAnalog;

        // Gain Control 2
        config.gain_controller2.enabled = true;

        // High Pass Filter
        config.high_pass_filter.enabled = true;

        m_apm->ApplyConfig(config);

        // Create stream configuration
        m_streamConfig = new webrtc::StreamConfig(sample_rate, num_channels);

        yInfo() << "[AECComponent::initializeAEC] AEC initialized successfully";
        yInfo() << "[AECComponent::initializeAEC] Echo cancellation: ENABLED";
        yInfo() << "[AECComponent::initializeAEC] Gain control 1: ENABLED (Adaptive Analog)";
        yInfo() << "[AECComponent::initializeAEC] Gain control 2: ENABLED";
        yInfo() << "[AECComponent::initializeAEC] High pass filter: ENABLED";

        return true;
    }
    catch (const std::exception &e)
    {
        yError() << "[AECComponent::initializeAEC] Exception:" << e.what();
        return false;
    }
}

void AECComponent::onRead(yarp::sig::Sound &msg)
{
    (void)msg;
    // This callback is triggered whenever new audio arrives on the microphone port
    // The actual processing happens in the processing thread
}

void AECComponent::processingThreadFunction()
{
    yInfo() << "[AECComponent::processingThread] Processing thread started";

    while (!m_shouldExit)
    {
        yInfo() << "[AECComponent::processingThread] Waiting for speaker audio input...";
        // Read reference audio (speaker output)
        yarp::sig::Sound referenceAudio = m_referenceReader.getRecordedReferenceBlocks();

        yInfo() << "[AECComponent::processingThread] Retrieved reference audio with" << referenceAudio.getSamples() << "samples at" << referenceAudio.getFrequency() << "Hz";
        yInfo() << "[AECComponent::processingThread] Waiting for microphone audio input...";
        
        // Read microphone audio (blocking): this paces the processing loop and reduces busy-spin jitter.
        yarp::sig::Sound *microphoneAudio = m_microphoneAudioInputPort.read(true);

        yInfo() << "[AECComponent::processingThread] Received microphone audio with" << (microphoneAudio ? microphoneAudio->getSamples() : 0) << "samples at" << (microphoneAudio ? microphoneAudio->getFrequency() : 0) << "Hz";

        std::vector<short> micSamples;
        for (int i = 0; i < static_cast<int>(microphoneAudio->getSamples()); ++i)
        {
            micSamples.push_back(microphoneAudio->get(i, 0));
        }
        yInfo() << "[AECComponent::processingThread] Extracted microphone samples into vector, size:" << micSamples.size();
        std::vector<short> referenceSamples;
        for (int i = 0; i < static_cast<int>(referenceAudio.getSamples()); ++i)
        {
            referenceSamples.push_back(referenceAudio.get(i, 0));
        }
        yInfo() << "[AECComponent::processingThread] Extracted reference samples into vector, size:" << referenceSamples.size();

        m_delayEstimator.update(micSamples, microphoneAudio->getFrequency(), referenceSamples, referenceAudio.getFrequency());

        yInfo() << "[AECComponent::processingThread] Updated delay estimator with new audio data";

        int estimatedDelay = m_delayEstimator.estimatedDelaySamples();

        yInfo() << "[AECComponent::processingThread] Estimated delay in samples:" << estimatedDelay;
        double delayConfidence = m_delayEstimator.confidence();

        yInfo() << "[AECComponent::processingThread] Delay confidence:" << delayConfidence;

        yarp::sig::Sound referenceBlockForMic = m_referenceReader.getReferenceBlock(estimatedDelay, microphoneAudio->getSamples());

        yInfo() << "[AECComponent::processingThread] Retrieved reference block for mic with" << referenceBlockForMic.getSamples() << "samples at" << referenceBlockForMic.getFrequency() << "Hz";

        if (delayConfidence >= m_delayEstimator.confidenceThreshold())
        {
            yInfo() << "[AECComponent::processingThread] Estimated delay:" << estimatedDelay << "samples, confidence:" << delayConfidence;
            // Flush old reference blocks that are outside the estimated delay window to prevent them from being used in future processing iterations.
            m_referenceReader.flushStaledBlocks(estimatedDelay);
        }
        else
        {
            yInfo() << "[AECComponent::processingThread] Delay confidence below threshold:" << delayConfidence;
        }

        // Append incoming samples to internal buffers
        if (microphoneAudio)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            int mic_samples = static_cast<int>(microphoneAudio->getSamples());
            // remember last incoming mic block size so we can emit matching blocks
            m_lastMicBlockSamples = mic_samples;
            // Auto-detect input frequency and reinitialize AEC if it differs
            int mic_freq = microphoneAudio->getFrequency();
            if (mic_freq > 0 && mic_freq != m_sample_rate)
            {
                yInfo() << "[AECComponent::processingThread] Detected mic frequency" << mic_freq << "Hz, reinitializing AEC";
                // reset stream config and reinitialize AEC for new sample rate
                if (m_streamConfig)
                {
                    delete m_streamConfig;
                    m_streamConfig = nullptr;
                }
                m_sample_rate = mic_freq;
                // reinitialize AEC with new sample rate
                if (!initializeAEC(m_sample_rate, m_num_channels))
                {
                    yError() << "[AECComponent::processingThread] Failed to reinitialize AEC with new sample rate";
                }
            }
            for (int i = 0; i < mic_samples; ++i)
            {
                m_micBuffer.push_back(microphoneAudio->get(i, 0));
            }
            yInfo() << "[AECComponent::processingThread] Added" << mic_samples << "mic samples to buffer. Buffer size is now" << m_micBuffer.size();
        }
        if (referenceBlockForMic.getSamples() > 0)
        {
            int ref_samples = static_cast<int>(referenceBlockForMic.getSamples());
            for (int i = 0; i < ref_samples; ++i)
            {
                m_refBuffer.push_back(referenceBlockForMic.get(i, 0));
            }
            yInfo() << "[AECComponent::processingThread] Added" << ref_samples << "ref samples to buffer. Buffer size is now" << m_refBuffer.size();
        }

        // Frame size in samples (e.g., 48 kHz, 10 ms -> 480)
        const int frame_samples = static_cast<int>((m_sample_rate * m_block_ms) / 1000);

        // Process as many full frames as available in mic buffer
        while (true)
        {
            std::vector<short> mic_frame;
            std::vector<short> ref_frame;
            bool hadReferenceData = false;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (static_cast<int>(m_micBuffer.size()) < frame_samples)
                    break; // not enough mic samples yet

                // pop mic frame
                mic_frame.resize(frame_samples);
                for (int i = 0; i < frame_samples; ++i)
                {
                    mic_frame[i] = m_micBuffer.front();
                    m_micBuffer.pop_front();
                }

                // pop ref frame if available
                if (static_cast<int>(m_refBuffer.size()) >= frame_samples)
                {
                    hadReferenceData = true;
                    ref_frame.resize(frame_samples);
                    for (int i = 0; i < frame_samples; ++i)
                    {
                        ref_frame[i] = m_refBuffer.front();
                        m_refBuffer.pop_front();
                    }
                }
                else
                {
                    // Keep AEC state progression consistent even when reference temporarily lags.
                    ref_frame.assign(frame_samples, 0);
                }
            }

            try
            {
                m_apm->set_stream_delay_ms(m_aecStreamDelayMs);
                m_apm->ProcessReverseStream(ref_frame.data(), *m_streamConfig, *m_streamConfig, ref_frame.data());
                m_apm->ProcessStream(mic_frame.data(), *m_streamConfig, *m_streamConfig, mic_frame.data());
                if (hadReferenceData)
                {
                    ++m_aecFramesWithReference;
                }
                else
                {
                    ++m_aecFramesWithoutReference;
                }

                // Append processed frame to output buffer; we'll emit in larger chunks
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (int i = 0; i < frame_samples; ++i)
                        m_outBuffer.push_back(mic_frame[i]);
                }
            }
            catch (const std::exception &e)
            {
                yError() << "[AECComponent::processingThread] Exception during frame processing:" << e.what();
            }

            if (m_logAecStats)
            {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastAecStatsLog).count();
                if (elapsed >= 1)
                {
                    yInfo() << "[AECComponent::processingThread] AEC active - frames with reference:" << m_aecFramesWithReference
                            << "without reference:" << m_aecFramesWithoutReference
                            << "mic buffer:" << m_micBuffer.size()
                            << "ref buffer:" << m_refBuffer.size();
                    m_aecFramesWithReference = 0;
                    m_aecFramesWithoutReference = 0;
                    m_lastAecStatsLog = now;
                }
            }
        }

        // Emit aggregated output blocks matching last incoming mic block size
        if (m_lastMicBlockSamples > 0)
        {
            while (true)
            {
                int emit_samples = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (static_cast<int>(m_outBuffer.size()) >= m_lastMicBlockSamples)
                    {
                        emit_samples = m_lastMicBlockSamples;
                    }
                }

                if (emit_samples == 0)
                    break;

                // pop emit_samples into a temporary buffer and send as one Sound
                std::vector<short> emit_buf(emit_samples);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (int i = 0; i < emit_samples; ++i)
                    {
                        emit_buf[i] = m_outBuffer.front();
                        m_outBuffer.pop_front();
                    }
                }

                yarp::sig::Sound &out = m_audioOutputPort.prepare();
                out.resize(emit_samples, 1);
                out.setFrequency(m_sample_rate);
                for (int i = 0; i < emit_samples; ++i)
                {
                    out.set(emit_buf[i], i, 0);
                }
                yDebug() << "[AECComponent::processingThread] Emitting aggregated block of" << emit_samples << "samples";

                if (m_saveIterativeAudioToDisk)
                {
                    for (short sample : emit_buf)
                    {
                        m_audioSaveBuffer.push_back(sample);
                    }
                    flushAudioSaveBuffer(false);
                }

                m_audioOutputPort.write();
            }
        }
    }

    yInfo() << "[AECComponent::processingThread] Processing thread exiting";
}

bool AECComponent::close()
{
    yInfo() << "[AECComponent::close] Closing component";

    m_shouldExit = true;

    if (m_processingThread.joinable())
    {
        m_processingThread.join();
    }
    // Close the reference reader first to stop its internal thread and release the port
    m_referenceReader.close();

    m_microphoneAudioInputPort.close();
    m_audioOutputPort.close();

    if (m_streamConfig)
    {
        delete m_streamConfig;
        m_streamConfig = nullptr;
    }

    if (m_saveIterativeAudioToDisk)
    {
        flushAudioSaveBuffer(true);
    }

    m_apm = nullptr;

    yInfo() << "[AECComponent::close] Component closed";
    return true;
}

bool AECComponent::updateModule()
{
    return true;
}

double AECComponent::getPeriod()
{
    return 0.1;  // Update period in seconds
}

bool AECComponent::saveAudioBlockToDisk(const std::vector<short> &samples)
{
    try
    {
        std::error_code errorCode;
        std::filesystem::path outputDirectory(m_audioSaveDirectory);
        std::filesystem::create_directories(outputDirectory, errorCode);
        if (errorCode)
        {
            yError() << "[AECComponent::saveAudioBlockToDisk] Unable to create output directory:" << m_audioSaveDirectory << errorCode.message();
            return false;
        }

        std::ostringstream fileNameStream;
        fileNameStream << m_audioSavePrefix << "_" << std::setw(6) << std::setfill('0') << m_audioSaveSequence++ << ".wav";

        std::filesystem::path outputPath = outputDirectory / fileNameStream.str();
        std::ofstream outputFile(outputPath, std::ios::binary);
        if (!outputFile)
        {
            yError() << "[AECComponent::saveAudioBlockToDisk] Unable to open output file:" << outputPath.string();
            return false;
        }

        const std::uint32_t sampleRate = static_cast<std::uint32_t>(m_sample_rate);
        const std::uint16_t channelCount = static_cast<std::uint16_t>(m_num_channels > 0 ? m_num_channels : 1);
        const std::uint16_t bitsPerSample = 16;
        const std::uint16_t blockAlign = static_cast<std::uint16_t>(channelCount * (bitsPerSample / 8));
        const std::uint32_t byteRate = sampleRate * blockAlign;
        const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
        const std::uint32_t riffChunkSize = 36 + dataSize;

        outputFile.write("RIFF", 4);
        writeLittleEndian32(outputFile, riffChunkSize);
        outputFile.write("WAVE", 4);
        outputFile.write("fmt ", 4);
        writeLittleEndian32(outputFile, 16);
        writeLittleEndian16(outputFile, 1);
        writeLittleEndian16(outputFile, channelCount);
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
    catch (const std::exception &exception)
    {
        yError() << "[AECComponent::saveAudioBlockToDisk] Exception:" << exception.what();
        return false;
    }
}

void AECComponent::flushAudioSaveBuffer(bool flushRemainder)
{
    if (!m_saveIterativeAudioToDisk)
    {
        return;
    }

    const int maxSamplesPerFile = std::max(1, m_sample_rate * m_audioSaveMaxSeconds);

    while (static_cast<int>(m_audioSaveBuffer.size()) >= maxSamplesPerFile ||
           (flushRemainder && !m_audioSaveBuffer.empty()))
    {
        const int samplesToWrite = static_cast<int>(std::min<std::size_t>(m_audioSaveBuffer.size(), static_cast<std::size_t>(maxSamplesPerFile)));
        std::vector<short> samples;
        samples.reserve(samplesToWrite);

        for (int i = 0; i < samplesToWrite; ++i)
        {
            samples.push_back(m_audioSaveBuffer.front());
            m_audioSaveBuffer.pop_front();
        }

        if (!saveAudioBlockToDisk(samples))
        {
            yError() << "[AECComponent::flushAudioSaveBuffer] Failed to save audio chunk to disk";
            break;
        }

        if (!flushRemainder)
        {
            break;
        }
    }
}
