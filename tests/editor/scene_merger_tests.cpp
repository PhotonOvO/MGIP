#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/GltfValidator.h>
#include <mgif/scene_editor/SceneMerger.h>

#include <tiny_gltf.h>

#include <filesystem>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace demo::editor::detail {

class GltfDocumentAccess final
{
public:
  [[nodiscard]] static tinygltf::Model& model(GltfDocument& document) noexcept
  {
    return document.mutableAuthorityModel();
  }

  [[nodiscard]] static const tinygltf::Model& model(const GltfDocument& document) noexcept
  {
    return document.authorityModel();
  }

  [[nodiscard]] static std::unique_ptr<GltfDocument> clone(
    const GltfDocument& document,
    tinygltf::Model model,
    std::filesystem::path sourcePath = {})
  {
    std::vector<GltfDependency> dependencies(
      document.dependencies().begin(), document.dependencies().end());
    if(sourcePath.empty()) sourcePath = document.sourcePath();
    return std::unique_ptr<GltfDocument>(new GltfDocument(
      std::make_unique<tinygltf::Model>(std::move(model)),
      std::move(sourcePath),
      document.format(),
      std::move(dependencies)));
  }
};

} // namespace demo::editor::detail

namespace {

using namespace demo::editor;

std::filesystem::path fixture(const char* name)
{
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

bool expect(bool condition, const char* message)
{
  if(!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}

class TempDirectory final
{
public:
  TempDirectory()
  {
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = std::filesystem::temp_directory_path()
      / ("mgif-scene-merger-" + std::to_string(ticks) + '-'
        + std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(m_path);
  }

  ~TempDirectory()
  {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
  std::filesystem::path m_path;
};

tinygltf::Value object(
  std::initializer_list<std::pair<const std::string, tinygltf::Value>> values)
{
  tinygltf::Value::Object result;
  for(const auto& value : values) result.emplace(value.first, value.second);
  return tinygltf::Value(std::move(result));
}

tinygltf::Value array(std::initializer_list<tinygltf::Value> values)
{
  return tinygltf::Value(tinygltf::Value::Array(values));
}

void declareExtension(tinygltf::Model& model, std::string name)
{
  if(std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(), name)
    == model.extensionsUsed.end()) model.extensionsUsed.push_back(std::move(name));
}

std::unique_ptr<GltfDocument> cloneWithModel(
  const GltfDocument& source,
  tinygltf::Model model,
  const std::filesystem::path& sourcePath = {})
{
  return demo::editor::detail::GltfDocumentAccess::clone(
    source, std::move(model), sourcePath);
}

const tinygltf::Model& model(const GltfDocument& document)
{
  return demo::editor::detail::GltfDocumentAccess::model(document);
}

std::unique_ptr<GltfDocument> addSingleRoot(std::unique_ptr<GltfDocument> document)
{
  tinygltf::Model copy = model(*document);
  copy.nodes.resize(1);
  copy.nodes[0].name = "External Resource Root";
  copy.scenes[0].nodes = {0};
  return cloneWithModel(*document, std::move(copy));
}

bool sameDependencies(
  std::span<const GltfDependency> left,
  const std::vector<GltfDependency>& right)
{
  if(left.size() != right.size()) return false;
  for(std::size_t index = 0; index < left.size(); ++index)
  {
    if(left[index].kind != right[index].kind
      || left[index].objectIndex != right[index].objectIndex
      || left[index].uri != right[index].uri
      || left[index].resolvedPath != right[index].resolvedPath
      || left[index].byteSize != right[index].byteSize) return false;
  }
  return true;
}

struct FailPublish final : EditorFaultInjector
{
  bool shouldFail(EditorFaultPoint point) noexcept override
  {
    return point == EditorFaultPoint::preCandidatePublish;
  }
};

std::unique_ptr<GltfDocument> load(const std::filesystem::path& path)
{
  auto result = GltfDocumentIO::loadCandidate(path);
  return result.succeeded() ? std::move(result).takeCandidate() : nullptr;
}

} // namespace

int main()
{
  bool passed = true;

  auto target = load(fixture("minimal_scene.gltf"));
  auto incoming = load(fixture("feature_preservation.gltf"));
  passed &= expect(target != nullptr && incoming != nullptr, "merge fixtures load");
  if(!target || !incoming) return 1;

  const std::uint64_t beforeRevision = target->revision();
  const SceneDigest beforeDigest = target->documentDigest();
  const std::size_t oldNodes = target->objectCount(EditorObjectKind::node);
  const std::size_t oldMeshes = target->objectCount(EditorObjectKind::mesh);
  const auto merged = SceneMerger::merge(*target, *incoming, "Imported Feature");
  passed &= expect(merged.applied(), "valid merge publishes candidate");
  passed &= expect(merged.wrapperNode.has_value(), "merge returns wrapper node");
  passed &= expect(target->revision() == beforeRevision + 1u, "merge advances revision once");
  passed &= expect(target->objectCount(EditorObjectKind::node) == oldNodes + incoming->objectCount(EditorObjectKind::node) + 1u,
                  "incoming nodes plus wrapper are appended");
  passed &= expect(target->objectCount(EditorObjectKind::mesh) == oldMeshes + incoming->objectCount(EditorObjectKind::mesh),
                  "incoming meshes are appended");
  passed &= expect(target->contains(merged.wrapperNode->untyped()), "wrapper stable ID is live");
  passed &= expect(GltfValidator::validate(*target).valid(), "merged authority validates");
  const std::size_t currentScene = target->currentSceneIndex().value_or(0u);
  const auto roots = target->sceneRootNodeIds(target->currentSceneId());
  passed &= expect(std::find(roots.begin(), roots.end(), *merged.wrapperNode) != roots.end(),
                  "wrapper is attached to current scene roots");

  auto switchedTarget = load(fixture("multi_scene_oracle.gltf"));
  auto switchedIncoming = load(fixture("minimal_scene.gltf"));
  passed &= expect(switchedTarget != nullptr && switchedIncoming != nullptr,
                  "multi-scene fixtures load");
  if(switchedTarget && switchedIncoming && switchedTarget->objectCount(EditorObjectKind::scene) > 1u)
  {
    const SceneId second = SceneId::from(switchedTarget->idAt(EditorObjectKind::scene, 1u)).value();
    passed &= expect(switchedTarget->selectCurrentScene(second), "second scene activates");
    const auto switchedMerge = SceneMerger::merge(*switchedTarget, *switchedIncoming, "Second Scene Import");
    passed &= expect(switchedMerge.applied() && switchedMerge.wrapperNode.has_value(),
                     "merge into non-default current scene publishes");
    const auto secondRoots = switchedTarget->sceneRootNodeIds(second);
    passed &= expect(std::find(secondRoots.begin(), secondRoots.end(), *switchedMerge.wrapperNode) != secondRoots.end(),
                     "wrapper follows current scene instead of default scene");
  }

  auto rollbackTarget = load(fixture("minimal_scene.gltf"));
  auto rollbackIncoming = load(fixture("minimal_scene.gltf"));
  passed &= expect(rollbackTarget != nullptr && rollbackIncoming != nullptr,
                  "rollback fixtures load");
  if(rollbackTarget && rollbackIncoming)
  {
    const auto revision = rollbackTarget->revision();
    const auto digest = rollbackTarget->documentDigest();
    const auto nodes = rollbackTarget->objectCount(EditorObjectKind::node);
    FailPublish faults;
    const auto failed = SceneMerger::merge(*rollbackTarget, *rollbackIncoming, {}, kDefaultEditorLimits, &faults);
    passed &= expect(failed.status == SceneMergeStatus::injectedFailure,
                     "injected publication failure is reported");
    passed &= expect(rollbackTarget->revision() == revision && rollbackTarget->documentDigest() == digest,
                     "failed merge preserves document revision and digest");
    passed &= expect(rollbackTarget->objectCount(EditorObjectKind::node) == nodes,
                     "failed merge preserves object registry");
  }

  // Merge must leave the target persistence envelope untouched while making
  // an imported external image self-contained for a later Save As.
  auto metadataTarget = load(fixture("external_image.gltf"));
  auto metadataIncoming = load(fixture("minimal_scene.gltf"));
  passed &= expect(metadataTarget != nullptr && metadataIncoming != nullptr,
                   "metadata fixtures load");
  if(metadataTarget && metadataIncoming)
  {
    const std::filesystem::path sourcePath = metadataTarget->sourcePath();
    const std::vector<GltfDependency> dependencies(
      metadataTarget->dependencies().begin(), metadataTarget->dependencies().end());
    const auto metadataMerge = SceneMerger::merge(*metadataTarget, *metadataIncoming,
                                                   "Metadata Import");
    passed &= expect(metadataMerge.applied(), "metadata merge publishes");
    passed &= expect(metadataTarget->sourcePath() == sourcePath,
                     "merge preserves target source path");
    passed &= expect(sameDependencies(metadataTarget->dependencies(), dependencies),
                     "merge preserves target dependency metadata");
  }

  auto imageTarget = load(fixture("minimal_scene.gltf"));
  TempDirectory imageSourceDirectory;
  std::filesystem::copy_file(fixture("external_image.gltf"),
    imageSourceDirectory.path() / "external_image.gltf");
  std::filesystem::copy_file(fixture("external_image.payload"),
    imageSourceDirectory.path() / "external_image.payload");
  auto imageIncoming = load(imageSourceDirectory.path() / "external_image.gltf");
  if(imageIncoming)
  {
    imageIncoming = addSingleRoot(std::move(imageIncoming));
  }
  passed &= expect(imageTarget != nullptr && imageIncoming != nullptr,
                   "external image merge fixtures load");
  if(imageTarget && imageIncoming)
  {
    const auto imageMerge = SceneMerger::merge(*imageTarget, *imageIncoming,
                                                "External Image Import");
    passed &= expect(imageMerge.applied(), "external image merge publishes");
    imageIncoming.reset();
    std::filesystem::remove(imageSourceDirectory.path() / "external_image.payload");
    TempDirectory output;
    const auto save = GltfDocumentIO::saveAs(*imageTarget, output.path() / "merged.gltf");
    passed &= expect(save.succeeded(), "merged external image Save As succeeds");
    auto reloaded = load(output.path() / "merged.gltf");
    passed &= expect(reloaded != nullptr, "merged external image Save As reloads");
    if(reloaded)
    {
      passed &= expect(!model(*reloaded).images.empty()
        && !model(*reloaded).images.back().image.empty(),
        "reloaded merged image retains decoded payload");
    }
  }

  // Exercise the typed and opaque KHR_audio records together with an
  // animation pointer. External audio is staged into a candidate bufferView;
  // publication must remain one atomic merge.
  auto audioBase = load(fixture("feature_preservation.gltf"));
  auto audioTargetBase = load(fixture("feature_preservation.gltf"));
  std::unique_ptr<GltfDocument> audioTarget;
  if(audioTargetBase)
  {
    tinygltf::Model targetModel = model(*audioTargetBase);
    targetModel.audioSources.resize(1);
    targetModel.audioSources[0].name = "Target Audio";
    targetModel.audioSources[0].bufferView = 0;
    targetModel.audioSources[0].mimeType = "audio/mpeg";
    targetModel.audioEmitters.resize(1);
    targetModel.audioEmitters[0].name = "Target Emitter";
    targetModel.audioEmitters[0].source = 0;
    targetModel.nodes[0].emitter = 0;
    targetModel.nodes[0].extensions["KHR_audio"] = object({
      {"emitter", tinygltf::Value(0)}});
    targetModel.scenes[0].audioEmitters = {0};
    targetModel.scenes[0].extensions["KHR_audio"] = object({
      {"emitters", array({tinygltf::Value(0)})}});
    targetModel.extensions["KHR_audio"] = object({
      {"emitters", array({object({
        {"name", tinygltf::Value("Target Emitter")},
        {"source", tinygltf::Value(0)}})})},
      {"sources", array({object({
        {"name", tinygltf::Value("Target Audio")},
        {"bufferView", tinygltf::Value(0)},
        {"mimeType", tinygltf::Value("audio/mpeg")}})})}});
    declareExtension(targetModel, "KHR_audio");
    audioTarget = cloneWithModel(*audioTargetBase, std::move(targetModel));
  }
  passed &= expect(audioBase != nullptr && audioTarget != nullptr,
                   "audio merge fixtures load");
  if(audioBase && audioTarget)
  {
    tinygltf::Model audioModel = model(*audioBase);
    audioModel.audioSources.resize(1);
    TempDirectory audioSourceDirectory;
    const std::array<unsigned char, 5> audioBytes{0, 1, 2, 3, 4};
    {
      std::ofstream audioFile(audioSourceDirectory.path() / "tone.mp3", std::ios::binary);
      audioFile.write(reinterpret_cast<const char*>(audioBytes.data()),
                      static_cast<std::streamsize>(audioBytes.size()));
    }
    audioModel.audioSources[0].name = "External Audio";
    audioModel.audioSources[0].uri = "tone.mp3";
    audioModel.audioSources[0].mimeType.clear();
    audioModel.audioEmitters.resize(1);
    audioModel.audioEmitters[0].name = "Emitter";
    audioModel.audioEmitters[0].source = 0;
    audioModel.nodes[0].emitter = 0;
    audioModel.nodes[0].extensions["KHR_audio"] = object({
      {"emitter", tinygltf::Value(0)}, {"sentinel", tinygltf::Value(17)}});
    audioModel.scenes[0].audioEmitters = {0};
    audioModel.scenes[0].extensions["KHR_audio"] = object({
      {"emitters", array({tinygltf::Value(0)})}, {"sentinel", tinygltf::Value(19)}});
    audioModel.extensions["KHR_audio"] = object({
      {"emitters", array({object({
        {"name", tinygltf::Value("Emitter")}, {"source", tinygltf::Value(0)}})})},
      {"sources", array({object({
        {"name", tinygltf::Value("External Audio")},
        {"uri", tinygltf::Value("tone.mp3")}})})},
      {"sentinel", tinygltf::Value(23)}});
    audioModel.extensions["MGIF_pointer_unknown"] = object({
      {"sources", array({object({{"value", tinygltf::Value(0.5)}})})}});
    declareExtension(audioModel, "KHR_audio");
    declareExtension(audioModel, "MGIF_pointer_unknown");

    audioModel.animations[0].channels[0].target_node = -1;
    audioModel.animations[0].channels[0].target_path = "pointer";
    audioModel.animations[0].channels[0].target_extensions[
      "KHR_animation_pointer"] = object({
        {"pointer", tinygltf::Value("/nodes/0/translation")}});
    tinygltf::AnimationChannel audioPointer = audioModel.animations[0].channels[0];
    audioPointer.target_extensions["KHR_animation_pointer"] = object({
      {"pointer", tinygltf::Value("/extensions/KHR_audio/emitters/0/gain")}});
    audioModel.animations[0].channels.push_back(std::move(audioPointer));
    tinygltf::AnimationChannel unknownPointer = audioModel.animations[0].channels[0];
    unknownPointer.target_extensions["KHR_animation_pointer"] = object({
      {"pointer", tinygltf::Value(
        "/extensions/MGIF_pointer_unknown/sources/0/value")}});
    audioModel.animations[0].channels.push_back(std::move(unknownPointer));
    audioModel.textures[0].extensions["MSFT_texture_dds"] = object({
      {"source", tinygltf::Value(0)}});
    audioModel.materials[0].extensions["EXT_materials_bump"] = object({
      {"bumpTexture", object({{"index", tinygltf::Value(0)}})}});
    declareExtension(audioModel, "MSFT_texture_dds");
    declareExtension(audioModel, "EXT_materials_bump");
    declareExtension(audioModel, "KHR_animation_pointer");
    auto audioIncoming = cloneWithModel(
      *audioBase, std::move(audioModel), audioSourceDirectory.path() / "incoming.gltf");
    const auto audioMerge = SceneMerger::merge(*audioTarget, *audioIncoming,
                                                "Audio Import");
    passed &= expect(audioMerge.applied(), "KHR_audio merge publishes");
    if(audioMerge.applied())
    {
      const tinygltf::Model& mergedModel = model(*audioTarget);
      passed &= expect(mergedModel.audioSources.size() == 2u
        && mergedModel.audioSources[1].uri.empty()
        && mergedModel.audioSources[1].bufferView >= 0,
        "external audio is staged into a bufferView");
      passed &= expect(mergedModel.audioEmitters.size() == 2u
        && mergedModel.audioEmitters[1].source == 1,
        "KHR_audio emitter source is remapped");
      passed &= expect(!mergedModel.scenes[0].audioEmitters.empty()
        && mergedModel.scenes[0].audioEmitters.back() == 1,
        "KHR_audio scene emitter is remapped");
      const auto& mergedNode = mergedModel.nodes[3];
      passed &= expect(mergedNode.emitter == 1
        && mergedNode.extensions.at("KHR_audio").Get("emitter").GetNumberAsInt() == 1
        && mergedNode.extensions.at("KHR_audio").Get("sentinel").GetNumberAsInt() == 17,
        "KHR_audio node emitter is remapped");
      const auto& mergedAudio = mergedModel.extensions.at("KHR_audio");
      passed &= expect(mergedAudio.Get("sources").Get(1).Get("bufferView").GetNumberAsInt() >= 0
        && mergedAudio.Get("emitters").Get(1).Get("source").GetNumberAsInt() == 1
        && mergedAudio.Get("sentinel").GetNumberAsInt() == 23,
        "opaque KHR_audio source records are remapped");
      passed &= expect(mergedModel.scenes[0].extensions.at("KHR_audio")
        .Get("sentinel").GetNumberAsInt() == 19,
        "opaque scene-level KHR_audio metadata is preserved");
      passed &= expect(mergedModel.animations[1].channels[0].target_extensions
        .at("KHR_animation_pointer").Get("pointer").Get<std::string>()
          == "/nodes/3/translation",
        "KHR_animation_pointer node index is remapped");
      passed &= expect(mergedModel.animations[1].channels[1].target_extensions
        .at("KHR_animation_pointer").Get("pointer").Get<std::string>()
          == "/extensions/KHR_audio/emitters/1/gain",
        "KHR_animation_pointer extension array index is remapped once");
      passed &= expect(mergedModel.animations[1].channels[2].target_extensions
        .at("KHR_animation_pointer").Get("pointer").Get<std::string>()
          == "/extensions/MGIF_pointer_unknown/sources/0/value",
        "unknown extension pointer array positions remain opaque");
      passed &= expect(mergedModel.textures[1].extensions.at("MSFT_texture_dds")
        .Get("source").GetNumberAsInt() == 1,
        "MSFT texture source is remapped");
      passed &= expect(mergedModel.materials[1].extensions.at("EXT_materials_bump")
        .Get("bumpTexture").Get("index").GetNumberAsInt() == 1,
        "EXT material bump texture is remapped");
      passed &= expect(!mergedModel.nodes[6].extensions.contains("KHR_audio"),
                       "wrapper excludes scene-scoped KHR_audio metadata");

      std::filesystem::remove(audioSourceDirectory.path() / "tone.mp3");
      TempDirectory output;
      const auto save = GltfDocumentIO::saveAs(*audioTarget, output.path() / "audio.gltf");
      passed &= expect(save.succeeded(), "staged audio Save As succeeds");
      auto reloaded = load(output.path() / "audio.gltf");
      passed &= expect(reloaded != nullptr, "staged audio Save As reloads");
      if(reloaded)
      {
        const tinygltf::Model& reloadedModel = model(*reloaded);
        const bool stagedReference = reloadedModel.audioSources.size() == 2u
          && reloadedModel.audioSources[1].uri.empty()
          && reloadedModel.audioSources[1].bufferView >= 0
          && static_cast<std::size_t>(reloadedModel.audioSources[1].bufferView)
            < reloadedModel.bufferViews.size();
        passed &= expect(stagedReference,
                         "reloaded audio retains an embedded bufferView reference");
        bool payloadMatches = false;
        if(stagedReference)
        {
          const auto& view = reloadedModel.bufferViews[
            static_cast<std::size_t>(reloadedModel.audioSources[1].bufferView)];
          if(view.buffer >= 0
            && static_cast<std::size_t>(view.buffer) < reloadedModel.buffers.size())
          {
            const auto& bytes = reloadedModel.buffers[
              static_cast<std::size_t>(view.buffer)].data;
            payloadMatches = view.byteLength == audioBytes.size()
              && view.byteOffset <= bytes.size()
              && audioBytes.size() <= bytes.size() - view.byteOffset
              && std::equal(audioBytes.begin(), audioBytes.end(),
                            bytes.begin() + view.byteOffset);
          }
        }
        passed &= expect(payloadMatches, "reloaded audio retains the staged bytes");
        passed &= expect(reloadedModel.extensions.contains("MGIF_pointer_unknown"),
                         "Save As preserves an unknown top-level extension");
      }
    }
  }

  // Invalid pointer indices and collection limits must fail before candidate
  // publication, preserving both revision and digest.
  auto invalidPointerBase = load(fixture("feature_preservation.gltf"));
  for(const char* pointer : {"/nodes/999/translation", "/nodes/00/translation",
                             "/nodes/999999999999999999999999999999/translation"})
  {
    auto invalidPointerTarget = load(fixture("minimal_scene.gltf"));
    if(!invalidPointerTarget || !invalidPointerBase)
    {
      passed &= expect(false, "invalid pointer fixtures load");
      continue;
    }
    tinygltf::Model invalidModel = model(*invalidPointerBase);
    invalidModel.animations[0].channels[0].target_node = -1;
    invalidModel.animations[0].channels[0].target_path = "pointer";
    invalidModel.animations[0].channels[0].target_extensions[
      "KHR_animation_pointer"] = object({
        {"pointer", tinygltf::Value(pointer)}});
    declareExtension(invalidModel, "KHR_animation_pointer");
    auto invalidIncoming = cloneWithModel(*invalidPointerBase, std::move(invalidModel));
    const std::uint64_t revision = invalidPointerTarget->revision();
    const SceneDigest digest = invalidPointerTarget->documentDigest();
    const auto failed = SceneMerger::merge(*invalidPointerTarget, *invalidIncoming,
                                           "Invalid Pointer");
    passed &= expect(!failed.applied(), "invalid animation pointer is rejected");
    passed &= expect(invalidPointerTarget->revision() == revision
      && invalidPointerTarget->documentDigest() == digest,
      "invalid pointer preserves target atomically");
  }

  auto conflictTargetBase = load(fixture("minimal_scene.gltf"));
  auto conflictIncomingBase = load(fixture("minimal_scene.gltf"));
  if(conflictTargetBase && conflictIncomingBase)
  {
    tinygltf::Model targetModel = model(*conflictTargetBase);
    tinygltf::Model incomingModel = model(*conflictIncomingBase);
    targetModel.extensions["MGIF_unknown_conflict"] = object({
      {"contract", tinygltf::Value(1)}});
    incomingModel.extensions["MGIF_unknown_conflict"] = object({
      {"contract", tinygltf::Value(2)}});
    declareExtension(targetModel, "MGIF_unknown_conflict");
    declareExtension(incomingModel, "MGIF_unknown_conflict");
    auto conflictTarget = cloneWithModel(*conflictTargetBase, std::move(targetModel));
    auto conflictIncoming = cloneWithModel(
      *conflictIncomingBase, std::move(incomingModel));
    const std::uint64_t revision = conflictTarget->revision();
    const SceneDigest digest = conflictTarget->documentDigest();
    const auto failed = SceneMerger::merge(*conflictTarget, *conflictIncoming,
                                           "Unknown Conflict");
    passed &= expect(failed.status == SceneMergeStatus::unsupportedExtensionReference,
                     "different values for one unknown extension key are rejected");
    passed &= expect(conflictTarget->revision() == revision
      && conflictTarget->documentDigest() == digest,
      "unknown extension conflict preserves target atomically");
  }

  auto overflowTarget = load(fixture("minimal_scene.gltf"));
  auto overflowIncoming = load(fixture("minimal_scene.gltf"));
  if(overflowTarget && overflowIncoming)
  {
    EditorLimits tinyLimits = kDefaultEditorLimits;
    tinyLimits.maxNodes = overflowTarget->objectCount(EditorObjectKind::node);
    const std::uint64_t revision = overflowTarget->revision();
    const SceneDigest digest = overflowTarget->documentDigest();
    const auto failed = SceneMerger::merge(*overflowTarget, *overflowIncoming,
                                           "Overflow", tinyLimits);
    passed &= expect(failed.status == SceneMergeStatus::limitExceeded,
                     "node capacity overflow is rejected before publish");
    passed &= expect(overflowTarget->revision() == revision
      && overflowTarget->documentDigest() == digest,
      "node capacity overflow preserves target atomically");
  }

  auto bufferLimitTarget = load(fixture("feature_preservation.gltf"));
  auto bufferLimitIncoming = load(fixture("feature_preservation.gltf"));
  if(bufferLimitTarget && bufferLimitIncoming)
  {
    EditorLimits tinyLimits = kDefaultEditorLimits;
    tinyLimits.maxBuffers = bufferLimitTarget->objectCount(EditorObjectKind::buffer);
    const auto failed = SceneMerger::merge(*bufferLimitTarget, *bufferLimitIncoming,
                                           "Buffer Limit", tinyLimits);
    passed &= expect(failed.status == SceneMergeStatus::limitExceeded,
                     "non-node collection overflow is rejected before publish");
  }

  (void)beforeDigest;
  (void)currentScene;
  return passed ? 0 : 1;
}
