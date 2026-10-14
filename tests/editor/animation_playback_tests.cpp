#include <mgif/scene_editor/AnimationPlayback.h>
#include <mgif/scene_editor/GltfDocumentIO.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace demo::editor;

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures"
      / "gltf_editor" / name;
}

std::size_t g_checks = 0;

bool expect(bool condition, std::string_view message) {
  ++g_checks;
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

bool near(double actual, double expected, double tolerance = 1.0e-6) {
  return std::abs(actual - expected) <= tolerance;
}

AnimationAccessorView scalar(std::span<const float> values) {
  return AnimationAccessorView{
      AnimationAccessorShape::scalar,
      AnimationAccessorComponentType::float32,
      values.size(),
      values};
}

AnimationAccessorView vec3(std::span<const float> values) {
  return AnimationAccessorView{
      AnimationAccessorShape::vec3,
      AnimationAccessorComponentType::float32,
      values.size() / 3u,
      values};
}

AnimationAccessorView vec4(std::span<const float> values) {
  return AnimationAccessorView{
      AnimationAccessorShape::vec4,
      AnimationAccessorComponentType::float32,
      values.size() / 4u,
      values};
}

bool testLinearAndStepSampling() {
  const std::array input{0.0F, 2.0F};
  const std::array translationValues{
      0.0F, 0.0F, 0.0F,
      2.0F, 4.0F, 6.0F};
  const std::array scaleValues{
      1.0F, 1.0F, 1.0F,
      3.0F, 5.0F, 7.0F};
  const std::array accessors{
      scalar(input), vec3(translationValues), vec3(scaleValues)};
  const SpanAnimationAccessorAdapter adapter(accessors);
  const std::array samplers{
      AnimationSamplerView{0, 1, AnimationInterpolation::LINEAR},
      AnimationSamplerView{0, 2, AnimationInterpolation::STEP}};
  const std::array channels{
      AnimationChannelView{7, 0, AnimationTargetPath::translation},
      AnimationChannelView{7, 1, AnimationTargetPath::scale}};
  const AnimationClipView clip{samplers, channels, 8};

  const AnimationClipAnalysis analysis = analyzeAnimationClip(clip, adapter);
  bool passed = expect(analysis.succeeded(), "valid LINEAR/STEP clip analyzes")
      && expect(analysis.info.startTime == 0.0 && analysis.info.endTime == 2.0,
                "analysis reports the union time range")
      && expect(analysis.info.samplerCount == 2
                    && analysis.info.channelCount == 2
                    && analysis.info.keyframeCount == 4,
                "analysis reports bounded clip work");

  const AnimationSampleResult middle = sampleAnimationClip(
      clip, adapter, 1.0, AnimationTimeMode::clamp);
  passed &= expect(middle.succeeded() && middle.transforms.size() == 1,
                   "channels for one node publish one atomic transform");
  if (middle.succeeded() && middle.transforms.size() == 1) {
    const AnimationTransformSample& sample = middle.transforms.front();
    passed &= expect(sample.nodeIndex == 7 && sample.translation && sample.scale
                         && !sample.rotation,
                     "sample preserves target node and typed TRS fields");
    if (sample.translation) {
      passed &= expect(near((*sample.translation)[0], 1.0)
                           && near((*sample.translation)[1], 2.0)
                           && near((*sample.translation)[2], 3.0),
                       "LINEAR translation interpolates component-wise");
    }
    if (sample.scale) {
      passed &= expect((*sample.scale == std::array<double, 3>{1.0, 1.0, 1.0}),
                       "STEP holds the preceding key inside an interval");
    }
  }

  const AnimationSampleResult end = sampleAnimationClip(
      clip, adapter, 100.0, AnimationTimeMode::clamp);
  passed &= expect(end.succeeded() && end.normalizedTime == 2.0
                       && end.transforms.size() == 1
                       && end.transforms.front().scale
                       && *end.transforms.front().scale
                           == std::array<double, 3>{3.0, 5.0, 7.0},
                   "clamp and STEP select the final key at the upper bound");
  return passed;
}

bool testQuaternionLinearSampling() {
  const std::array input{0.0F, 2.0F};
  constexpr float rootHalf = 0.70710678118654752440F;
  // The second quaternion uses the negative representation.  Shortest-path
  // slerp must flip it before interpolation.
  const std::array rotations{
      0.0F, 0.0F, 0.0F, 1.0F,
      0.0F, 0.0F, -rootHalf, -rootHalf};
  const std::array accessors{scalar(input), vec4(rotations)};
  const SpanAnimationAccessorAdapter adapter(accessors);
  const std::array samplers{
      AnimationSamplerView{0, 1, AnimationInterpolation::linear}};
  const std::array channels{
      AnimationChannelView{3, 0, AnimationTargetPath::rotation}};

  const AnimationSampleResult result = sampleAnimationClip(
      AnimationClipView{samplers, channels, 4}, adapter, 1.0);
  bool passed = expect(result.succeeded() && result.transforms.size() == 1
                           && result.transforms.front().rotation,
                       "LINEAR rotation produces a typed quaternion");
  if (result.succeeded() && result.transforms.front().rotation) {
    const auto& q = *result.transforms.front().rotation;
    const double length =
        std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    passed &= expect(near(length, 1.0, 1.0e-12),
                     "rotation interpolation normalizes its result")
        && expect(near(q[0], 0.0) && near(q[1], 0.0)
                      && near(q[2], 0.3826834324)
                      && near(q[3], 0.9238795325),
                  "rotation interpolation follows the shortest quaternion arc");
  }
  return passed;
}

bool testTimeNormalizationAndAdvance() {
  bool passed = true;
  passed &= expect(normalizeAnimationTime(-10.0, 1.0, 3.0,
                                           AnimationTimeMode::clamp)
                       == std::optional<double>{1.0}
                       && normalizeAnimationTime(10.0, 1.0, 3.0,
                                                 AnimationTimeMode::clamp)
                           == std::optional<double>{3.0},
                   "clamp normalization preserves inclusive endpoints");
  passed &= expect(normalizeAnimationTime(3.0, 1.0, 3.0,
                                           AnimationTimeMode::loop)
                       == std::optional<double>{1.0}
                       && normalizeAnimationTime(3.5, 1.0, 3.0,
                                                 AnimationTimeMode::loop)
                           == std::optional<double>{1.5}
                       && normalizeAnimationTime(0.5, 1.0, 3.0,
                                                 AnimationTimeMode::loop)
                           == std::optional<double>{2.5},
                   "loop normalization is half-open and handles negative offsets");
  passed &= expect(normalizeAnimationTime(99.0, 2.0, 2.0,
                                           AnimationTimeMode::loop)
                       == std::optional<double>{2.0},
                   "zero-duration clips normalize deterministically");
  passed &= expect(!normalizeAnimationTime(
                         std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0)
                         .has_value()
                       && !normalizeAnimationTime(0.0, 2.0, 1.0).has_value()
                       && !normalizeAnimationTime(
                               0.0, 0.0, 0.0,
                               static_cast<AnimationTimeMode>(99)).has_value(),
                   "non-finite time, ranges, and modes fail closed");

  AnimationClipInfo info{1.0, 3.0, 1, 1, 2};
  AnimationPlaybackState state{1.0, 2.0, true, true};
  const AnimationAdvanceResult advanced =
      advanceAnimationPlayback(state, 1.25, info);
  passed &= expect(advanced.succeeded() && state.time == 1.5,
                   "playback advancement scales and loops time");

  const AnimationPlaybackState before = state;
  state.speed = std::numeric_limits<double>::infinity();
  const AnimationAdvanceResult rejected =
      advanceAnimationPlayback(state, 1.0, info);
  passed &= expect(!rejected.succeeded() && state.time == before.time,
                   "invalid advancement leaves playback time unchanged");
  return passed;
}

AnimationSampleStatus statusFor(
    std::span<const AnimationAccessorView> accessors,
    std::span<const AnimationSamplerView> samplers,
    std::span<const AnimationChannelView> channels,
    std::uint64_t nodeCount = 1,
    const AnimationSamplingLimits& limits = {}) {
  const SpanAnimationAccessorAdapter adapter(accessors);
  return sampleAnimationClip(AnimationClipView{samplers, channels, nodeCount},
                             adapter, 0.0, AnimationTimeMode::clamp, limits)
      .status;
}

bool testExplicitCubicAndInvalidInterpolation() {
  const std::array input{0.0F, 1.0F};
  const std::array values{
      0.0F, 0.0F, 0.0F,
      1.0F, 1.0F, 1.0F};
  const std::array accessors{scalar(input), vec3(values)};
  const std::array channels{
      AnimationChannelView{0, 0, AnimationTargetPath::translation}};
  const std::array cubic{
      AnimationSamplerView{0, 1, AnimationInterpolation::CUBICSPLINE}};
  const std::array invalid{
      AnimationSamplerView{0, 1, AnimationInterpolation::invalid}};

  return expect(statusFor(accessors, cubic, channels)
                    == AnimationSampleStatus::unsupportedCubicSpline,
                "CUBICSPLINE has an explicit unsupported result")
      && expect(statusFor(accessors, invalid, channels)
                    == AnimationSampleStatus::invalidInterpolation,
                "unknown interpolation is rejected separately");
}

bool testAccessorAndChannelRejection() {
  const std::array input{0.0F, 1.0F};
  const std::array negative{-1.0F, 0.0F};
  const std::array descending{1.0F, 0.0F};
  const std::array values{
      0.0F, 0.0F, 0.0F,
      1.0F, 1.0F, 1.0F};
  const std::array shortValues{0.0F, 0.0F, 0.0F};
  const std::array validAccessors{scalar(input), vec3(values)};
  const std::array sampler{
      AnimationSamplerView{0, 1, AnimationInterpolation::LINEAR}};
  const std::array channel{
      AnimationChannelView{0, 0, AnimationTargetPath::translation}};
  bool passed = true;

  const std::array wrongInput{vec3(values), vec3(values)};
  passed &= expect(statusFor(wrongInput, sampler, channel)
                       == AnimationSampleStatus::invalidAccessorType,
                   "animation input must be FLOAT SCALAR");
  const std::array wrongOutput{scalar(input), scalar(input)};
  passed &= expect(statusFor(wrongOutput, sampler, channel)
                       == AnimationSampleStatus::invalidAccessorType,
                   "translation output must be FLOAT VEC3");
  const std::array shortOutput{scalar(input), vec3(shortValues)};
  passed &= expect(statusFor(shortOutput, sampler, channel)
                       == AnimationSampleStatus::invalidAccessorCount,
                   "input/output key counts must match");
  const std::array badTimes{scalar(descending), vec3(values)};
  passed &= expect(statusFor(badTimes, sampler, channel)
                       == AnimationSampleStatus::invalidAccessorPayload,
                   "key times must be strictly increasing");
  const std::array negativeTimes{scalar(negative), vec3(values)};
  passed &= expect(statusFor(negativeTimes, sampler, channel)
                       == AnimationSampleStatus::invalidAccessorPayload,
                   "glTF key times must be non-negative");

  AnimationAccessorView truncated = vec3(values);
  truncated.count = 3;
  const std::array truncatedAccessors{scalar(input), truncated};
  passed &= expect(statusFor(truncatedAccessors, sampler, channel)
                       == AnimationSampleStatus::invalidAccessorPayload,
                   "truncated typed accessor payload is rejected");

  const std::array badSampler{
      AnimationSamplerView{0, 99, AnimationInterpolation::linear}};
  passed &= expect(statusFor(validAccessors, badSampler, channel)
                       == AnimationSampleStatus::invalidAccessorIndex,
                   "out-of-range accessor index is rejected");
  const std::array badChannel{
      AnimationChannelView{0, 0, AnimationTargetPath::invalid}};
  passed &= expect(statusFor(validAccessors, sampler, badChannel)
                       == AnimationSampleStatus::invalidChannel,
                   "unsupported target path is rejected");
  const std::array badNode{
      AnimationChannelView{1, 0, AnimationTargetPath::translation}};
  passed &= expect(statusFor(validAccessors, sampler, badNode)
                       == AnimationSampleStatus::invalidChannel,
                   "out-of-range target node is rejected");
  const std::array duplicates{
      AnimationChannelView{0, 0, AnimationTargetPath::translation},
      AnimationChannelView{0, 0, AnimationTargetPath::translation}};
  passed &= expect(statusFor(validAccessors, sampler, duplicates)
                       == AnimationSampleStatus::duplicateChannel,
                   "duplicate node/path channels are rejected");

  const std::array accessorsWithShortUnused{
      scalar(input), vec3(values), vec3(shortValues)};
  const std::array samplerWithShortUnused{
      AnimationSamplerView{0, 1, AnimationInterpolation::linear},
      AnimationSamplerView{0, 2, AnimationInterpolation::step}};
  passed &= expect(statusFor(accessorsWithShortUnused,
                             samplerWithShortUnused, channel)
                       == AnimationSampleStatus::invalidAccessorCount,
                   "every sampler validates matching input/output counts");
  return passed;
}

class CountingAdapter final : public AnimationAccessorAdapter {
public:
  explicit CountingAdapter(std::span<const AnimationAccessorView> accessors)
      : m_accessors(accessors) {}

  [[nodiscard]] std::size_t accessorCount() const noexcept override {
    return m_accessors.size();
  }

  [[nodiscard]] std::optional<AnimationAccessorView> readAccessor(
      std::size_t index) const noexcept override {
    ++reads;
    if (index >= m_accessors.size())
      return std::nullopt;
    return m_accessors[index];
  }

  mutable std::size_t reads{0};

private:
  std::span<const AnimationAccessorView> m_accessors;
};

bool testLimitsAndAtomicFailure() {
  const std::array input{0.0F, 1.0F};
  const std::array values{
      0.0F, 0.0F, 0.0F,
      1.0F, 1.0F, 1.0F};
  const std::array accessors{scalar(input), vec3(values)};
  const std::array samplers{
      AnimationSamplerView{0, 1, AnimationInterpolation::linear}};
  const std::array channels{
      AnimationChannelView{0, 0, AnimationTargetPath::translation}};
  CountingAdapter adapter(accessors);
  AnimationSamplingLimits limits;
  limits.maxSamplers = 0;
  const AnimationSampleResult limited = sampleAnimationClip(
      AnimationClipView{samplers, channels, 1}, adapter, 0.5,
      AnimationTimeMode::clamp, limits);
  bool passed = expect(limited.status == AnimationSampleStatus::limitExceeded
                           && limited.transforms.empty() && adapter.reads == 0,
                       "limits reject before accessor reads or pose allocation");

  const std::array badChannels{
      AnimationChannelView{0, 0, AnimationTargetPath::translation},
      AnimationChannelView{2, 0, AnimationTargetPath::scale}};
  const AnimationSampleResult atomic = sampleAnimationClip(
      AnimationClipView{samplers, badChannels, 1}, adapter, 0.5);
  passed &= expect(!atomic.succeeded() && atomic.transforms.empty(),
                   "invalid later channel never publishes an earlier sample");

  AnimationSamplingLimits keyLimit;
  keyLimit.maxKeyframes = 1;
  const AnimationSampleResult tooManyKeys = sampleAnimationClip(
      AnimationClipView{samplers, channels, 1}, adapter, 0.5,
      AnimationTimeMode::clamp, keyLimit);
  passed &= expect(tooManyKeys.status == AnimationSampleStatus::limitExceeded
                       && tooManyKeys.transforms.empty(),
                   "aggregate keyframe work is bounded");
  return passed;
}

bool testGltfDocumentAdapter() {
  auto loaded = GltfDocumentIO::loadCandidate(
      fixture("feature_preservation.gltf"));
  bool passed = expect(loaded.succeeded(),
                       "animation fixture loads through document I/O");
  const GltfDocument* document = loaded.candidate();
  if (document == nullptr)
    return false;

  GltfAnimationAdapter adapter(*document, 0);
  passed &= expect(adapter.valid(),
                   "document animation constructs a valid typed adapter");
  passed &= expect(adapter.status() == AnimationSampleStatus::success
                       && adapter.animationIndex() == 0,
                   "adapter reports the selected animation and success");
  const auto input = adapter.accessor(3);
  const auto output = adapter.accessor(4);
  passed &= expect(adapter.accessorCount() == 5 && !adapter.accessor(0)
                       && input && output,
                   "adapter preserves source indices while exposing referenced accessors");
  if (input && output) {
    passed &= expect(input->shape == AnimationAccessorShape::scalar
                         && input->count == 2
                         && output->shape == AnimationAccessorShape::vec3
                         && output->count == 2,
                     "document accessors cross the boundary as typed views");
  }

  const AnimationClipAnalysis analysis = analyzeAnimation(*document, 0);
  passed &= expect(analysis.succeeded() && near(analysis.info.startTime, 0.0)
                       && near(analysis.info.endTime, 1.0)
                       && analysis.info.samplerCount == 1
                       && analysis.info.channelCount == 1,
                   "document animation exposes its bounded clip range");

  const AnimationSampleResult middle = sampleAnimation(
      *document, 0, 0.5, false);
  passed &= expect(middle.succeeded() && middle.normalizedTime == 0.5
                       && middle.transforms.size() == 1,
                   "document animation samples at a clamped midpoint");
  if (middle.succeeded() && middle.transforms.size() == 1) {
    const AnimationTransformSample& transform = middle.transforms.front();
    passed &= expect(transform.nodeIndex == 0 && transform.translation,
                     "document channel targets node zero translation");
    if (transform.translation) {
      passed &= expect(near((*transform.translation)[0], 0.5)
                           && near((*transform.translation)[1], 1.0)
                           && near((*transform.translation)[2], 1.5),
                       "document adapter decodes and linearly interpolates VEC3");
    }
  }

  const AnimationSampleResult looped = sampleAnimation(*document, 0, 1.0);
  passed &= expect(looped.succeeded() && looped.normalizedTime == 0.0
                       && looped.transforms.size() == 1
                       && looped.transforms.front().translation
                       && near((*looped.transforms.front().translation)[0], 0.0)
                       && near((*looped.transforms.front().translation)[1], 0.0)
                       && near((*looped.transforms.front().translation)[2], 0.0),
                   "loop mode maps the exact end point to the start key");

  const GltfAnimationAdapter invalid(*document, 99);
  passed &= expect(!invalid.valid()
                       && invalid.status()
                           == AnimationSampleStatus::invalidAnimationIndex,
                   "invalid document animation indices fail closed");

  AnimationSamplingLimits tinyLimits;
  tinyLimits.maxAccessors = 1;
  const GltfAnimationAdapter limited(*document, 0, tinyLimits);
  passed &= expect(!limited.valid()
                       && limited.status() == AnimationSampleStatus::limitExceeded,
                   "document adapter enforces accessor limits before decoding");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testLinearAndStepSampling();
  passed &= testQuaternionLinearSampling();
  passed &= testTimeNormalizationAndAdvance();
  passed &= testExplicitCubicAndInvalidInterpolation();
  passed &= testAccessorAndChannelRejection();
  passed &= testLimitsAndAtomicFailure();
  passed &= testGltfDocumentAdapter();
  if (passed) {
    std::cout << "Animation playback sampling passed " << g_checks
              << " checks.\n";
  }
  return passed ? 0 : 1;
}
