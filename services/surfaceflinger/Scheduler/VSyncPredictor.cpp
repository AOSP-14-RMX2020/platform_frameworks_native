/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra"

#undef LOG_TAG
#define LOG_TAG "VSyncPredictor"

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <algorithm>
#include <chrono>
#include <sstream>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/compiler.h>
#include <cutils/properties.h>
#include <ftl/concat.h>
#include <gui/TraceUtils.h>
#include <utils/Log.h>

#include "RefreshRateSelector.h"
#include "VSyncPredictor.h"

namespace android::scheduler {

using base::StringAppendF;

static auto constexpr kMaxPercent = 100u;

VSyncPredictor::~VSyncPredictor() = default;

VSyncPredictor::VSyncPredictor(PhysicalDisplayId id, nsecs_t idealPeriod, size_t historySize,
                               size_t minimumSamplesForPrediction, uint32_t outlierTolerancePercent)
      : mId(id),
        mTraceOn(property_get_bool("debug.sf.vsp_trace", false)),
        kHistorySize(historySize),
        kMinimumSamplesForPrediction(minimumSamplesForPrediction),
        kOutlierTolerancePercent(std::min(outlierTolerancePercent, kMaxPercent)),
        mIdealPeriod(idealPeriod) {
    resetModel();
}

inline void VSyncPredictor::traceInt64If(const char* name, int64_t value) const {
    if (CC_UNLIKELY(mTraceOn)) {
        traceInt64(name, value);
    }
}

inline void VSyncPredictor::traceInt64(const char* name, int64_t value) const {
    ATRACE_INT64(ftl::Concat(ftl::truncated<14>(name), " ", mId.value).c_str(), value);
}

inline size_t VSyncPredictor::next(size_t i) const {
    return (i + 1) % mTimestamps.size();
}

bool VSyncPredictor::validate(nsecs_t timestamp) const {
    if (mLastTimestampIndex < 0 || mTimestamps.empty()) {
        return true;
    }

    auto const aValidTimestamp = mTimestamps[mLastTimestampIndex];
    auto const percent = (timestamp - aValidTimestamp) % mIdealPeriod * kMaxPercent / mIdealPeriod;
    if (percent >= kOutlierTolerancePercent &&
        percent <= (kMaxPercent - kOutlierTolerancePercent)) {
        return false;
    }

    const auto iter = std::min_element(mTimestamps.begin(), mTimestamps.end(),
                                       [timestamp](nsecs_t a, nsecs_t b) {
                                           return std::abs(timestamp - a) < std::abs(timestamp - b);
                                       });
    const auto distancePercent = std::abs(*iter - timestamp) * kMaxPercent / mIdealPeriod;
    if (distancePercent < kOutlierTolerancePercent) {
        // duplicate timestamp
        return false;
    }
    return true;
}

nsecs_t VSyncPredictor::currentPeriod() const {
    std::lock_guard lock(mMutex);
    return mRateMap.find(mIdealPeriod)->second.slope;
}

bool VSyncPredictor::addVsyncTimestamp(nsecs_t timestamp) {
    std::lock_guard lock(mMutex);

    if (!validate(timestamp)) {
        // VSR could elect to ignore the incongruent timestamp or resetModel(). If ts is ignored,
        // don't insert this ts into mTimestamps ringbuffer. If we are still
        // in the learning phase we should just clear all timestamps and start
        // over.
        if (mTimestamps.size() < kMinimumSamplesForPrediction) {
            // Add the timestamp to mTimestamps before clearing it so we could
            // update mKnownTimestamp based on the new timestamp.
            mTimestamps.push_back(timestamp);
            clearTimestamps();
        } else if (!mTimestamps.empty()) {
            mKnownTimestamp =
                    std::max(timestamp, *std::max_element(mTimestamps.begin(), mTimestamps.end()));
        } else {
            mKnownTimestamp = timestamp;
        }
        return false;
    }

    if (mTimestamps.size() != kHistorySize) {
        mTimestamps.push_back(timestamp);
        mLastTimestampIndex = next(mLastTimestampIndex);
    } else {
        mLastTimestampIndex = next(mLastTimestampIndex);
        mTimestamps[mLastTimestampIndex] = timestamp;
    }

    traceInt64If("VSP-ts", timestamp);

    const size_t numSamples = mTimestamps.size();
    if (numSamples < kMinimumSamplesForPrediction) {
        mRateMap[mIdealPeriod] = {mIdealPeriod, 0};
        return true;
    }

    // This is a 'simple linear regression' calculation of Y over X, with Y being the
    // vsync timestamps, and X being the ordinal of vsync count.
    // The calculated slope is the vsync period.
    // Formula for reference:
    // Sigma_i: means sum over all timestamps.
    // mean(variable): statistical mean of variable.
    // X: snapped ordinal of the timestamp
    // Y: vsync timestamp
    //
    //         Sigma_i( (X_i - mean(X)) * (Y_i - mean(Y) )
    // slope = -------------------------------------------
    //         Sigma_i ( X_i - mean(X) ) ^ 2
    //
    // intercept = mean(Y) - slope * mean(X)
    //
    std::vector<nsecs_t> vsyncTS(numSamples);
    std::vector<nsecs_t> ordinals(numSamples);

    // Normalizing to the oldest timestamp cuts down on error in calculating the intercept.
    const auto oldestTS = *std::min_element(mTimestamps.begin(), mTimestamps.end());
    auto it = mRateMap.find(mIdealPeriod);
    auto const currentPeriod = it->second.slope;

    // The mean of the ordinals must be precise for the intercept calculation, so scale them up for
    // fixed-point arithmetic.
    constexpr int64_t kScalingFactor = 1000;

    nsecs_t meanTS = 0;
    nsecs_t meanOrdinal = 0;

    for (size_t i = 0; i < numSamples; i++) {
        const auto timestamp = mTimestamps[i] - oldestTS;
        vsyncTS[i] = timestamp;
        meanTS += timestamp;

        const auto ordinal = currentPeriod == 0
                ? 0
                : (vsyncTS[i] + currentPeriod / 2) / currentPeriod * kScalingFactor;
        ordinals[i] = ordinal;
        meanOrdinal += ordinal;
    }

    meanTS /= numSamples;
    meanOrdinal /= numSamples;

    for (size_t i = 0; i < numSamples; i++) {
        vsyncTS[i] -= meanTS;
        ordinals[i] -= meanOrdinal;
    }

    nsecs_t top = 0;
    nsecs_t bottom = 0;
    for (size_t i = 0; i < numSamples; i++) {
        top += vsyncTS[i] * ordinals[i];
        bottom += ordinals[i] * ordinals[i];
    }

    if (CC_UNLIKELY(bottom == 0)) {
        it->second = {mIdealPeriod, 0};
        clearTimestamps();
        return false;
    }

    nsecs_t const anticipatedPeriod = top * kScalingFactor / bottom;
    nsecs_t const intercept = meanTS - (anticipatedPeriod * meanOrdinal / kScalingFactor);

    auto const percent = std::abs(anticipatedPeriod - mIdealPeriod) * kMaxPercent / mIdealPeriod;
    if (percent >= kOutlierTolerancePercent) {
        it->second = {mIdealPeriod, 0};
        clearTimestamps();
        return false;
    }

    traceInt64If("VSP-period", anticipatedPeriod);
    traceInt64If("VSP-intercept", intercept);

    it->second = {anticipatedPeriod, intercept};

    ALOGV("model update ts %" PRIu64 ": %" PRId64 " slope: %" PRId64 " intercept: %" PRId64,
          mId.value, timestamp, anticipatedPeriod, intercept);
    return true;
}

auto VSyncPredictor::getVsyncSequenceLocked(nsecs_t timestamp) const -> VsyncSequence {
    const auto vsync = nextAnticipatedVSyncTimeFromLocked(timestamp);
    if (!mLastVsyncSequence) return {vsync, 0};

    const auto [slope, _] = getVSyncPredictionModelLocked();
    const auto [lastVsyncTime, lastVsyncSequence] = *mLastVsyncSequence;
    const auto vsyncSequence = lastVsyncSequence +
            static_cast<int64_t>(std::round((vsync - lastVsyncTime) / static_cast<float>(slope)));
    return {vsync, vsyncSequence};
}

nsecs_t VSyncPredictor::nextAnticipatedVSyncTimeFromLocked(nsecs_t timePoint) const {
    auto const [slope, intercept] = getVSyncPredictionModelLocked();

    if (mTimestamps.empty()) {
        traceInt64("VSP-mode", 1);
        auto const knownTimestamp = mKnownTimestamp ? *mKnownTimestamp : timePoint;
        auto const numPeriodsOut = ((timePoint - knownTimestamp) / mIdealPeriod) + 1;
        return knownTimestamp + numPeriodsOut * mIdealPeriod;
    }

    auto const oldest = *std::min_element(mTimestamps.begin(), mTimestamps.end());

    // See b/145667109, the ordinal calculation must take into account the intercept.
    auto const zeroPoint = oldest + intercept;
    auto const ordinalRequest = (timePoint - zeroPoint + slope) / slope;
    auto const prediction = (ordinalRequest * slope) + intercept + oldest;

    traceInt64("VSP-mode", 0);
    traceInt64If("VSP-timePoint", timePoint);
    traceInt64If("VSP-prediction", prediction);

    auto const printer = [&, slope = slope, intercept = intercept] {
        std::stringstream str;
        str << "prediction made from: " << timePoint << "prediction: " << prediction << " (+"
            << prediction - timePoint << ") slope: " << slope << " intercept: " << intercept
            << "oldestTS: " << oldest << " ordinal: " << ordinalRequest;
        return str.str();
    };

    ALOGV("%s", printer().c_str());
    LOG_ALWAYS_FATAL_IF(prediction < timePoint, "VSyncPredictor: model miscalculation: %s",
                        printer().c_str());

    return prediction;
}

nsecs_t VSyncPredictor::nextAnticipatedVSyncTimeFrom(nsecs_t timePoint) const {
    std::lock_guard lock(mMutex);

    // update the mLastVsyncSequence for reference point
    mLastVsyncSequence = getVsyncSequenceLocked(timePoint);

    const auto renderRatePhase = [&]() REQUIRES(mMutex) -> int {
        if (!mRenderRate) return 0;

        const auto divisor =
                RefreshRateSelector::getFrameRateDivisor(Fps::fromPeriodNsecs(mIdealPeriod),
                                                         *mRenderRate);
        if (divisor <= 1) return 0;

        const int mod = mLastVsyncSequence->seq % divisor;
        if (mod == 0) return 0;

        return divisor - mod;
    }();

    if (renderRatePhase == 0) {
        return mLastVsyncSequence->vsyncTime;
    }

    auto const [slope, intercept] = getVSyncPredictionModelLocked();
    const auto approximateNextVsync = mLastVsyncSequence->vsyncTime + slope * renderRatePhase;
    return nextAnticipatedVSyncTimeFromLocked(approximateNextVsync - slope / 2);
}

/*
 * Returns whether a given vsync timestamp is in phase with a frame rate.
 * If the frame rate is not a divisor of the refresh rate, it is always considered in phase.
 * For example, if the vsync timestamps are (16.6,33.3,50.0,66.6):
 * isVSyncInPhase(16.6, 30) = true
 * isVSyncInPhase(33.3, 30) = false
 * isVSyncInPhase(50.0, 30) = true
 */
bool VSyncPredictor::isVSyncInPhase(nsecs_t timePoint, Fps frameRate) const {
    std::lock_guard lock(mMutex);
    const auto divisor =
            RefreshRateSelector::getFrameRateDivisor(Fps::fromPeriodNsecs(mIdealPeriod), frameRate);
    return isVSyncInPhaseLocked(timePoint, static_cast<unsigned>(divisor));
}

bool VSyncPredictor::isVSyncInPhaseLocked(nsecs_t timePoint, unsigned divisor) const {
    const TimePoint now = TimePoint::now();
    const auto getTimePointIn = [](TimePoint now, nsecs_t timePoint) -> float {
        return ticks<std::milli, float>(TimePoint::fromNs(timePoint) - now);
    };
    ATRACE_FORMAT("%s timePoint in: %.2f divisor: %zu", __func__, getTimePointIn(now, timePoint),
                  divisor);

    if (divisor <= 1 || timePoint == 0) {
        return true;
    }

    const nsecs_t period = mRateMap[mIdealPeriod].slope;
    const nsecs_t justBeforeTimePoint = timePoint - period / 2;
    const auto vsyncSequence = getVsyncSequenceLocked(justBeforeTimePoint);
    ATRACE_FORMAT_INSTANT("vsync in: %.2f sequence: %" PRId64,
                          getTimePointIn(now, vsyncSequence.vsyncTime), vsyncSequence.seq);
    return vsyncSequence.seq % divisor == 0;
}

void VSyncPredictor::setRenderRate(Fps fps) {
    ALOGV("%s %s: %s", __func__, to_string(mId).c_str(), to_string(fps).c_str());
    std::lock_guard lock(mMutex);
    mRenderRate = fps;
}

VSyncPredictor::Model VSyncPredictor::getVSyncPredictionModel() const {
    std::lock_guard lock(mMutex);
    const auto model = VSyncPredictor::getVSyncPredictionModelLocked();
    return {model.slope, model.intercept};
}

VSyncPredictor::Model VSyncPredictor::getVSyncPredictionModelLocked() const {
    return mRateMap.find(mIdealPeriod)->second;
}

void VSyncPredictor::setPeriod(nsecs_t period) {
    ATRACE_FORMAT("%s %s", __func__, to_string(mId).c_str());
    traceInt64("VSP-setPeriod", period);

    std::lock_guard lock(mMutex);
    static constexpr size_t kSizeLimit = 30;
    if (CC_UNLIKELY(mRateMap.size() == kSizeLimit)) {
        mRateMap.erase(mRateMap.begin());
    }

    mIdealPeriod = period;
    if (mRateMap.find(period) == mRateMap.end()) {
        mRateMap[mIdealPeriod] = {period, 0};
    }

    clearTimestamps();
}

void VSyncPredictor::clearTimestamps() {
    if (!mTimestamps.empty()) {
        auto const maxRb = *std::max_element(mTimestamps.begin(), mTimestamps.end());
        if (mKnownTimestamp) {
            mKnownTimestamp = std::max(*mKnownTimestamp, maxRb);
        } else {
            mKnownTimestamp = maxRb;
        }

        mTimestamps.clear();
        mLastTimestampIndex = 0;
    }
}

bool VSyncPredictor::needsMoreSamples() const {
    std::lock_guard lock(mMutex);
    return mTimestamps.size() < kMinimumSamplesForPrediction;
}

void VSyncPredictor::resetModel() {
    std::lock_guard lock(mMutex);
    mRateMap[mIdealPeriod] = {mIdealPeriod, 0};
    clearTimestamps();
}

void VSyncPredictor::dump(std::string& result) const {
    std::lock_guard lock(mMutex);
    StringAppendF(&result, "\tmIdealPeriod=%.2f\n", mIdealPeriod / 1e6f);
    StringAppendF(&result, "\tRefresh Rate Map:\n");
    for (const auto& [idealPeriod, periodInterceptTuple] : mRateMap) {
        StringAppendF(&result,
                      "\t\tFor ideal period %.2fms: period = %.2fms, intercept = %" PRId64 "\n",
                      idealPeriod / 1e6f, periodInterceptTuple.slope / 1e6f,
                      periodInterceptTuple.intercept);
    }
}

} // namespace android::scheduler

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wextra"
