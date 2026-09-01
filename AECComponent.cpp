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
#include <cmath>
#include <sstream>
#include <vector>
#include <algorithm>
#include <yarp/os/Time.h>

namespace
{

    void appendSamples(std::deque<short> &buffer, const std::vector<short> &samples)
    {
        for (short sample : samples)
        {
            buffer.push_back(sample);
        }
    }

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
      m_referenceBufferSeconds(10.0),
      m_audioSaveDirectory("./aec-recordings"),
      m_audioSavePrefix("aec_output"),
      m_audioSaveSequence(0),
      m_microphoneSaveSequence(0),
      m_referenceSaveSequence(0),
      m_aecMobileMode(false),
      m_aecStreamDelayMs(120),
      m_logAecStats(true),
      m_lastAecStatsLog(std::chrono::steady_clock::now())
{
    
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
        if (aecConfig.check("referenceBufferSeconds"))
        {
            m_referenceBufferSeconds = aecConfig.find("referenceBufferSeconds").asFloat64();
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
    if (m_referenceBufferSeconds <= 0.0)
    {
        yWarning() << "[AECComponent::configure] referenceBufferSeconds must be > 0, falling back to 10";
        m_referenceBufferSeconds = 10.0;
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
        yInfo() << "[AECComponent::configure] Recording filtered, original microphone, and reference audio";
        yInfo() << "[AECComponent::configure] Max saved file length:" << m_audioSaveMaxSeconds << "seconds";
    }
    yInfo() << "[AECComponent::configure] Reference buffer limit:" << m_referenceBufferSeconds << "seconds";
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
        config.gain_controller1.enabled = false; // Disable Gain Control 1 (Analog) as it may interfere with AEC
        config.gain_controller1.mode =
            webrtc::AudioProcessing::Config::GainController1::kAdaptiveAnalog;

        // Gain Control 2
        config.gain_controller2.enabled = false; // Disable Gain Control 2 as it may interfere with AEC

        // High Pass Filter
        config.high_pass_filter.enabled = true;

        m_apm->ApplyConfig(config);

        // Create stream configuration
        m_streamConfig = new webrtc::StreamConfig(sample_rate, num_channels);

        yInfo() << "[AECComponent::initializeAEC] AEC initialized successfully with sample rate:" << sample_rate << "Hz and channels:" << num_channels;
        yInfo() << "[AECComponent::initializeAEC] Echo cancellation: ENABLED";
        yInfo() << "[AECComponent::initializeAEC] Gain control 1: DISABLED (Adaptive Analog)";
        yInfo() << "[AECComponent::initializeAEC] Gain control 2: DISABLED";
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

        m_micBuffer.clear();
        m_refBuffer.clear();
        m_outBuffer.clear();
        // wait for microphone input
        yInfo() << "[AECComponent::processingThread] Waiting for microphone audio input...";
        yarp::sig::Sound *microphoneAudio = m_microphoneAudioInputPort.read(true);

        if (!microphoneAudio)
        {
            if (m_shouldExit)
            {
                break;
            }
            continue;
        }

        yInfo() << "[AECComponent::processingThread] Received microphone audio with" << (microphoneAudio ? microphoneAudio->getSamples() : 0) << "samples at" << (microphoneAudio ? microphoneAudio->getFrequency() : 0) << "Hz";

        // split the microphone input into samples with a certain frequency
        std::vector<short> micSamples;
        for (int i = 0; i < static_cast<int>(microphoneAudio->getSamples()); ++i)
        {
            micSamples.push_back(microphoneAudio->get(i, 0));
        }

        yInfo() << "[AECComponent::processingThread] Extracted microphone samples into vector, size:" << micSamples.size();

        // read the microphone input frequency in case there's a misconfiguration
        // correct it by reinitializing the microphone frequency
        int micFrequency = microphoneAudio->getFrequency();
        int micChannels = microphoneAudio->getChannels();
        if (micFrequency > 0 && micFrequency != m_sample_rate || micChannels != m_num_channels)
        {
            yInfo() << "[AECComponent::processingThread] Detected mic frequency" << micFrequency << "Hz and channels" << micChannels << ", reinitializing AEC";
            if (m_streamConfig)
            {
                delete m_streamConfig;
                m_streamConfig = nullptr;
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_micBuffer.clear();
                m_refBuffer.clear();
                m_outBuffer.clear();
                m_audioSaveBuffer.clear();
                m_microphoneSaveBuffer.clear();
                m_referenceSaveBuffer.clear();
            }

            m_sample_rate = micFrequency;
            m_num_channels = micChannels;
            if (!initializeAEC(m_sample_rate, m_num_channels))
            {
                yError() << "[AECComponent::processingThread] Failed to reinitialize AEC with new sample rate and channels";
            }
        }

        m_micBuffer = std::vector<short>(micSamples.begin(), micSamples.end());

        if (m_saveIterativeAudioToDisk)
        {
            m_microphoneSaveBuffer.insert(m_microphoneSaveBuffer.end(), micSamples.begin(), micSamples.end());
            m_micMarkerIndices.insert(m_micMarkerIndices.end(), micSamples.size() - 1, false);
            m_micMarkerIndices.push_back(true); // Mark the end of this microphone block
        }

        // get the reference queue collected until this moment, and its sample rate
        int referenceSampleRate = 0;

        yarp::sig::Sound recordedReference = m_referenceReader.getRecordedReferenceBlocks();

        // if the reference queue is empty, we can skip send the the microphone input to the output port as it is, since we don't have a reference to cancel the echo
        if (recordedReference.getSamples() == 0)
        {
            yInfo() << "[AECComponent::processingThread] No reference blocks available, skipping AEC processing for this microphone block";
            m_outBuffer = m_micBuffer;
            if (m_saveIterativeAudioToDisk)
            {
                m_audioSaveBuffer.insert(m_audioSaveBuffer.end(), m_outBuffer.begin(), m_outBuffer.end());
                m_outMarkerIndices.insert(m_outMarkerIndices.end(), m_outBuffer.size() - 1, false);
                m_outMarkerIndices.push_back(true); // Mark the end of this output block
                // save to disk the reference buffer as no sound with same size of m_outBuffer
                auto nullReference = std::vector<short>(m_outBuffer.size(), 0);
                m_referenceSaveBuffer.insert(m_referenceSaveBuffer.end(), nullReference.begin(), nullReference.end());
                m_refMarkerIndices.insert(m_refMarkerIndices.end(), nullReference.size() - 1, false);
                m_refMarkerIndices.push_back(true); // Mark the end of this reference block
                flushAllAudioSaveBuffers(false);
            }
            sendFilteredAudio(m_outBuffer, m_sample_rate);
            continue;
        }

        yInfo() << "[AECComponent::processingThread] Retrieved recorded reference blocks with" << recordedReference.getSamples() << "samples at" << recordedReference.getFrequency() << "Hz";

        referenceSampleRate = recordedReference.getFrequency();
        int referenceNumChannels = recordedReference.getChannels();

        yInfo() << "[AECComponent::processingThread] Reference blocks have" << referenceNumChannels << "channels";
        if (referenceNumChannels != m_num_channels)
        {
            yWarning() << "[AECComponent::processingThread] Reference blocks have" << referenceNumChannels << "channels, but AEC is configured for" << m_num_channels << "channels. Only the first channel of the reference will be used.";
        }

        if (referenceSampleRate > 0 && referenceSampleRate != m_sample_rate)
        {
            yInfo() << "[AECComponent::processingThread] Resampling reference from" << referenceSampleRate << "Hz to" << m_sample_rate << "Hz";
            yarp::sig::soundfilters::resample(recordedReference, m_sample_rate);
            referenceSampleRate = m_sample_rate;
        }

        for (int i = 0; i < static_cast<int>(recordedReference.getSamples()); ++i)
        {
            m_refBuffer.push_back(recordedReference.get(i, 0));
        }

        yInfo() << "[AECComponent::processingThread] Reference buffer size after resampling:" << m_refBuffer.size();


        auto alignedReference = std::vector<short>(m_refBuffer.begin(), m_refBuffer.end());
        m_referenceSaveBuffer.insert(m_referenceSaveBuffer.end(), alignedReference.begin(), alignedReference.end());
        m_refMarkerIndices.insert(m_refMarkerIndices.end(), alignedReference.size() - 1, false);
        m_refMarkerIndices.push_back(true); // Mark the end of this reference block

        const int frame_samples = std::max(1, static_cast<int>((m_sample_rate * m_block_ms) / 1000));

        m_apm->set_stream_delay_ms(m_referenceReader.getLastEstimatedAudioPlayerDelay());

        yInfo() << "[AECComponent::processingThread] Set AEC stream delay to" << m_referenceReader.getLastEstimatedAudioPlayerDelay() << "ms";

        for (int i = 0; i < static_cast<int>(m_micBuffer.size() / frame_samples); ++i)
        {
            std::vector<short> mic_frame;
            std::vector<short> ref_frame;

            if (static_cast<int>(m_micBuffer.size()) < frame_samples)
                break; // not enough mic samples yet

            mic_frame.insert(mic_frame.begin(), m_micBuffer.begin() + i * frame_samples, m_micBuffer.begin() + (i + 1) * frame_samples);
            ref_frame.insert(ref_frame.begin(), alignedReference.begin() + i * frame_samples, alignedReference.begin() + (i + 1) * frame_samples);

            try
            {
                
                m_apm->ProcessReverseStream(ref_frame.data(), *m_streamConfig, *m_streamConfig, ref_frame.data());
                m_apm->ProcessStream(mic_frame.data(), *m_streamConfig, *m_streamConfig, mic_frame.data());

                // Append processed frame to output buffer; we'll emit in larger chunks
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (int j = 0; j < frame_samples; ++j)
                    {
                        m_outBuffer.push_back(mic_frame[j]);
                        if (m_saveIterativeAudioToDisk)
                        {
                            m_audioSaveBuffer.push_back(mic_frame[j]);
                            m_outMarkerIndices.push_back(false);
                        }
                    }
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
                    yInfo() << "[AECComponent::processingThread] AEC active - "
                            << "mic buffer:" << m_micBuffer.size()
                            << "ref buffer:" << m_refBuffer.size();

                    m_lastAecStatsLog = now;
                }
            }
        }

        m_outMarkerIndices[m_outMarkerIndices.size() - 1] = true; // Mark the end of this output block

        // write to output port the filtered microphone audio
        sendFilteredAudio(m_outBuffer, m_sample_rate);

        // save to disk if enabled
        if (m_saveIterativeAudioToDisk)
        {
            flushAllAudioSaveBuffers(false);
        }



        // log the size of the microphone, reference and output buffers
        yInfo() << "[AECComponent::processingThread] Mic buffer size:" << m_microphoneSaveBuffer.size();
        yInfo() << "[AECComponent::processingThread] Ref buffer size:" << m_referenceSaveBuffer.size();
        yInfo() << "[AECComponent::processingThread] Out buffer size:" << m_audioSaveBuffer.size();

        yInfo() << "[AECComponent::processingThread] Processing thread exiting";
    }
}

void AECComponent::sendFilteredAudio(const std::vector<short> &samples, int sampleRate)
{
    yarp::sig::Sound &out = m_audioOutputPort.prepare();
    out.resize(static_cast<int>(samples.size()), 1);
    out.setFrequency(sampleRate);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        out.set(samples[i], static_cast<int>(i), 0);
    }
    yDebug() << "[AECComponent::sendFilteredAudio] Sending filtered audio block of" << samples.size() << "samples at" << sampleRate << "Hz";
    m_audioOutputPort.write();
}

bool AECComponent::close()
{
    yInfo() << "[AECComponent::close] Closing component";

    m_shouldExit = true;

    m_microphoneAudioInputPort.close();
    m_referenceReader.close();

    if (m_processingThread.joinable())
    {
        m_processingThread.join();
    }
    m_audioOutputPort.close();

    if (m_streamConfig)
    {
        delete m_streamConfig;
        m_streamConfig = nullptr;
    }

    if (m_saveIterativeAudioToDisk)
    {
        flushAllAudioSaveBuffers(true);
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
    return 0.1; // Update period in seconds
}

// bool AECComponent::saveAudioBlockToDisk(const std::vector<short> &samples,
//                                         const std::string &streamName,
//                                         std::size_t sequence)
// {
//     try
//     {
//         std::error_code errorCode;
//         std::filesystem::path outputDirectory(m_audioSaveDirectory);
//         std::filesystem::create_directories(outputDirectory, errorCode);
//         if (errorCode)
//         {
//             yError() << "[AECComponent::saveAudioBlockToDisk] Unable to create output directory:" << m_audioSaveDirectory << errorCode.message();
//             return false;
//         }

//         std::ostringstream fileNameStream;
//         fileNameStream << m_audioSavePrefix << "_" << streamName << "_"
//                        << std::setw(6) << std::setfill('0') << sequence << ".wav";

//         std::filesystem::path outputPath = outputDirectory / fileNameStream.str();
//         std::ofstream outputFile(outputPath, std::ios::binary);
//         if (!outputFile)
//         {
//             yError() << "[AECComponent::saveAudioBlockToDisk] Unable to open output file:" << outputPath.string();
//             return false;
//         }

//         const std::uint32_t sampleRate = static_cast<std::uint32_t>(m_sample_rate);
//         // All saved streams contain the channel-zero samples extracted above.
//         const std::uint16_t channelCount = 1;
//         const std::uint16_t bitsPerSample = 16;
//         const std::uint16_t blockAlign = static_cast<std::uint16_t>(channelCount * (bitsPerSample / 8));
//         const std::uint32_t byteRate = sampleRate * blockAlign;
//         const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
//         const std::uint32_t riffChunkSize = 36 + dataSize;

//         outputFile.write("RIFF", 4);
//         writeLittleEndian32(outputFile, riffChunkSize);
//         outputFile.write("WAVE", 4);
//         outputFile.write("fmt ", 4);
//         writeLittleEndian32(outputFile, 16);
//         writeLittleEndian16(outputFile, 1);
//         writeLittleEndian16(outputFile, channelCount);
//         writeLittleEndian32(outputFile, sampleRate);
//         writeLittleEndian32(outputFile, byteRate);
//         writeLittleEndian16(outputFile, blockAlign);
//         writeLittleEndian16(outputFile, bitsPerSample);
//         outputFile.write("data", 4);
//         writeLittleEndian32(outputFile, dataSize);

//         for (short sample : samples)
//         {
//             writeLittleEndian16(outputFile, static_cast<std::uint16_t>(static_cast<std::int16_t>(sample)));
//         }

//         return static_cast<bool>(outputFile);
//     }
//     catch (const std::exception &exception)
//     {
//         yError() << "[AECComponent::saveAudioBlockToDisk] Exception:" << exception.what();
//         return false;
//     }
// }

bool AECComponent::saveAudioBlockToDisk(const std::vector<short> &samples,
                                            std::vector<bool> &markerIndices,
                                             const std::string &streamName,
                                             std::size_t &sequence,
                                            bool flushRemainder)
{

    std::error_code errorCode;
    std::filesystem::path outputDirectory(m_audioSaveDirectory);
    std::filesystem::create_directories(outputDirectory, errorCode);
    if (errorCode)
    {
        yError() << "[AECComponent::saveAudioBlockToDisk] Unable to create output directory:" << m_audioSaveDirectory << errorCode.message();
        return false;
    }

    // save the audio samples to a WAV file with yarp sig sound and yarp audio to file device
    yarp::sig::Sound sound;
    sound.resize(static_cast<int>(samples.size()), 1);
    sound.setFrequency(m_sample_rate);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        sound.set(samples[i], static_cast<int>(i), 0);
        if (markerIndices[i] == true && !flushRemainder)
        {
            yInfo() << "[AECComponent::saveAudioBlockToDisk] Marker at sample index:" << i << "for stream:" << streamName;
            sound.add_marker("marker_" + std::to_string(i), i);
        }
    }

    std::string fileName = m_audioSaveDirectory + "/" +
    streamName + "_" + std::to_string(sequence) + ".wav";

    yarp::sig::file::write(sound, fileName.c_str());
    return true;
}

void AECComponent::flushAudioSaveBuffer(std::vector<short> &buffer,
                                    std::vector<bool> &markerIndices,
                                        const std::string &streamName,
                                        std::size_t &sequence,
                                        bool flushRemainder)
{
    if (!m_saveIterativeAudioToDisk)
    {
        return;
    }

    const int maxSamplesPerFile = std::max(1, m_sample_rate * m_audioSaveMaxSeconds);

    while (static_cast<int>(buffer.size()) >= maxSamplesPerFile ||
           (flushRemainder && !buffer.empty()))
    {
        const int samplesToWrite = static_cast<int>(std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(maxSamplesPerFile)));
        std::vector<short> samples;
        samples.reserve(samplesToWrite);
        std::vector<bool> subMarkerIndices;
        subMarkerIndices.reserve(samplesToWrite);

        for (int i = 0; i < samplesToWrite; ++i)
        {
            samples.push_back(buffer[i]);
            subMarkerIndices.push_back(markerIndices[i]);
            
        }
        if (!saveAudioBlockToDisk(samples, subMarkerIndices, streamName, sequence, flushRemainder))
        {
            yError() << "[AECComponent::flushAudioSaveBuffer] Failed to save" << streamName << " audio chunk to disk";
            break;
        }
        buffer.erase(buffer.begin(), buffer.begin() + samplesToWrite);
        markerIndices.erase(markerIndices.begin(), markerIndices.begin() + samplesToWrite);
        ++sequence;

        if (!flushRemainder)
        {
            break;
        }
    }
}

void AECComponent::flushAllAudioSaveBuffers(bool flushRemainder)
{
    flushAudioSaveBuffer(m_audioSaveBuffer, m_outMarkerIndices, "filtered", m_audioSaveSequence, flushRemainder);
    flushAudioSaveBuffer(m_microphoneSaveBuffer, m_micMarkerIndices, "microphone", m_microphoneSaveSequence, flushRemainder);
    flushAudioSaveBuffer(m_referenceSaveBuffer, m_refMarkerIndices, "reference", m_referenceSaveSequence, flushRemainder);
}
