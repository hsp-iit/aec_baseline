/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include "delayEstimator.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace
{
int nextPowerOfTwo(int value)
{
    int powerOfTwo = 1;
    while (powerOfTwo < value)
    {
        powerOfTwo <<= 1;
    }
    return powerOfTwo;
}

void fft(std::vector<std::complex<double>> &buffer, bool inverse)
{
    const std::size_t size = buffer.size();

    for (std::size_t index = 1, reversed = 0; index < size; ++index)
    {
        std::size_t bit = size >> 1;
        for (; reversed & bit; bit >>= 1)
        {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (index < reversed)
        {
            std::swap(buffer[index], buffer[reversed]);
        }
    }

    const double pi = std::acos(-1.0);
    for (std::size_t length = 2; length <= size; length <<= 1)
    {
        const double angle = 2.0 * pi / static_cast<double>(length) * (inverse ? 1.0 : -1.0);
        const std::complex<double> wLength(std::cos(angle), std::sin(angle));

        for (std::size_t start = 0; start < size; start += length)
        {
            std::complex<double> w(1.0, 0.0);
            const std::size_t halfLength = length >> 1;

            for (std::size_t index = 0; index < halfLength; ++index)
            {
                const std::complex<double> u = buffer[start + index];
                const std::complex<double> v = buffer[start + index + halfLength] * w;
                buffer[start + index] = u + v;
                buffer[start + index + halfLength] = u - v;
                w *= wLength;
            }
        }
    }

    if (inverse)
    {
        for (std::complex<double> &value : buffer)
        {
            value /= static_cast<double>(size);
        }
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

struct DelayResult
{
    int delaySamples = 0;
    double confidence = 0.0;
};

DelayResult estimateDelayGccPhat(const std::vector<short> &micSignal,
                                 const std::vector<short> &referenceSignal)
{
    DelayResult result;

    if (micSignal.empty() || referenceSignal.empty())
    {
        return result;
    }

    const std::size_t correlationSize = static_cast<std::size_t>(nextPowerOfTwo(static_cast<int>(micSignal.size() + referenceSignal.size())));
    std::vector<std::complex<double>> micSpectrum(correlationSize, std::complex<double>(0.0, 0.0));
    std::vector<std::complex<double>> referenceSpectrum(correlationSize, std::complex<double>(0.0, 0.0));

    const double pi = std::acos(-1.0);
    const double windowDenominator = micSignal.size() > 1 ? static_cast<double>(micSignal.size() - 1) : 1.0;

    for (std::size_t index = 0; index < micSignal.size(); ++index)
    {
        const double window = 0.5 - 0.5 * std::cos((2.0 * pi * static_cast<double>(index)) / windowDenominator);
        micSpectrum[index] = std::complex<double>(window * static_cast<double>(micSignal[index]), 0.0);
    }

    const double referenceWindowDenominator = referenceSignal.size() > 1 ? static_cast<double>(referenceSignal.size() - 1) : 1.0;
    for (std::size_t index = 0; index < referenceSignal.size(); ++index)
    {
        const double window = 0.5 - 0.5 * std::cos((2.0 * pi * static_cast<double>(index)) / referenceWindowDenominator);
        referenceSpectrum[index] = std::complex<double>(window * static_cast<double>(referenceSignal[index]), 0.0);
    }

    fft(micSpectrum, false);
    fft(referenceSpectrum, false);

    for (std::size_t index = 0; index < correlationSize; ++index)
    {
        std::complex<double> crossSpectrum = micSpectrum[index] * std::conj(referenceSpectrum[index]);
        const double magnitude = std::abs(crossSpectrum);
        if (magnitude > 1e-12)
        {
            crossSpectrum /= magnitude;
        }
        else
        {
            crossSpectrum = std::complex<double>(0.0, 0.0);
        }
        micSpectrum[index] = crossSpectrum;
    }

    fft(micSpectrum, true);

    double bestCorrelation = 0.0;
    std::size_t bestIndex = 0;
    double correlationSum = 0.0;

    for (std::size_t index = 0; index < correlationSize; ++index)
    {
        const double value = std::abs(micSpectrum[index].real());
        correlationSum += value;
        if (value > bestCorrelation)
        {
            bestCorrelation = value;
            bestIndex = index;
        }
    }

    if (bestIndex > correlationSize / 2)
    {
        result.delaySamples = -static_cast<int>(correlationSize - bestIndex);
    }
    else
    {
        result.delaySamples = static_cast<int>(bestIndex);
    }

    if (bestCorrelation > 1e-12)
    {
        result.confidence = (correlationSum / static_cast<double>(correlationSize)) / bestCorrelation;
    }

    return result;
}
} // namespace

DelayEstimator::DelayEstimator(double confidenceThreshold)
    : m_estimatedDelaySamples(0),
      m_confidence(0.0),
      m_confidenceThreshold(confidenceThreshold)
{
}

bool DelayEstimator::update(const std::vector<short> &micSignal,
                            int micSampleRate,
                            const std::vector<short> &referenceSignal,
                            int referenceSampleRate)
{
    if (micSignal.empty() || referenceSignal.empty() || micSampleRate <= 0 || referenceSampleRate <= 0)
    {
        return false;
    }

    const std::vector<short> referenceAtMicRate = resampleLinear(referenceSignal, referenceSampleRate, micSampleRate);
    const DelayResult result = estimateDelayGccPhat(micSignal, referenceAtMicRate);
    m_confidence = result.confidence;

    if (m_confidence < m_confidenceThreshold)
    {
        return false;
    }

    m_estimatedDelaySamples = result.delaySamples;
    return true;
}

int DelayEstimator::estimatedDelaySamples() const
{
    return m_estimatedDelaySamples;
}

double DelayEstimator::confidence() const
{
    return m_confidence;
}

double DelayEstimator::confidenceThreshold() const
{
    return m_confidenceThreshold;
}

void DelayEstimator::setConfidenceThreshold(double confidenceThreshold)
{
    m_confidenceThreshold = confidenceThreshold;
}

void DelayEstimator::reset()
{
    m_estimatedDelaySamples = 0;
    m_confidence = 0.0;
}