/******************************************************************************
 *                                                                            *
 * Copyright (C) 2024 Acoustic Echo Cancellation Component                    *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#ifndef DELAY_ESTIMATOR_HPP
#define DELAY_ESTIMATOR_HPP

#include <vector>

class DelayEstimator
{
public:
    explicit DelayEstimator(double confidenceThreshold = 0.1);

    // Update the internal delay estimate using GCC-PHAT.
    // The reference signal is resampled to the microphone sample rate first.
    bool update(const std::vector<short> &micSignal,
                int micSampleRate,
                const std::vector<short> &referenceSignal,
                int referenceSampleRate);

    int estimatedDelaySamples() const;
    double confidence() const;
    double confidenceThreshold() const;
    void setConfidenceThreshold(double confidenceThreshold);
    void reset();

private:
    int m_estimatedDelaySamples;
    double m_confidence;
    double m_confidenceThreshold;
};

#endif // DELAY_ESTIMATOR_HPP