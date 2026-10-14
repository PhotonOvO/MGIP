#pragma once

#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/EditorSelection.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/SceneCommands.h>
#include <mgif/scene_editor/commands/EditorHistory.h>
#include <mgif/scene_editor/testing/EditorStateInspector.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace demo::editor::testing {

class ManualEditorClock final : public EditorClock {
public:
  explicit ManualEditorClock(EditorTimePoint initial = {}) noexcept
      : m_now(initial) {}

  [[nodiscard]] EditorTimePoint now() const noexcept override {
    ++m_nowCallCount;
    return m_now;
  }

  void set(EditorTimePoint value) noexcept { m_now = value; }
  [[nodiscard]] std::size_t nowCallCount() const noexcept {
    return m_nowCallCount;
  }

  template <typename Rep, typename Period>
  [[nodiscard]] bool
  advance(std::chrono::duration<Rep, Period> duration) noexcept {
    const EditorTimePoint::duration delta =
        std::chrono::duration_cast<EditorTimePoint::duration>(duration);
    if (delta < EditorTimePoint::duration::zero()) {
      return false;
    }
    const EditorTimePoint::duration current = m_now.time_since_epoch();
    if (current > EditorTimePoint::duration::max() - delta) {
      return false;
    }
    m_now += delta;
    return true;
  }

private:
  EditorTimePoint m_now;
  mutable std::size_t m_nowCallCount{0};
};

class ScriptedEditorFaultInjector final : public EditorFaultInjector {
public:
  [[nodiscard]] bool failOnCall(EditorFaultPoint point,
                                std::size_t oneBasedCall) noexcept {
    if (!isValid(point) || oneBasedCall == 0) {
      return false;
    }
    const std::size_t index = toIndex(point);
    m_callCounts[index] = 0;
    m_failOnCalls[index] = oneBasedCall;
    return true;
  }

  [[nodiscard]] bool disable(EditorFaultPoint point) noexcept {
    if (!isValid(point)) {
      return false;
    }
    const std::size_t index = toIndex(point);
    m_callCounts[index] = 0;
    m_failOnCalls[index] = kDisabled;
    return true;
  }

  void reset() noexcept {
    m_callCounts.fill(0);
    m_failOnCalls.fill(kDisabled);
  }

  [[nodiscard]] std::size_t callCount(EditorFaultPoint point) const noexcept {
    return isValid(point) ? m_callCounts[toIndex(point)] : 0;
  }

  [[nodiscard]] bool shouldFail(EditorFaultPoint point) noexcept override {
    if (!isValid(point)) {
      return false;
    }
    const std::size_t index = toIndex(point);
    const std::size_t call = ++m_callCounts[index];
    return call == m_failOnCalls[index];
  }

private:
  static constexpr std::size_t kFaultPointCount =
      static_cast<std::size_t>(EditorFaultPoint::count);
  static constexpr std::size_t kDisabled =
      std::numeric_limits<std::size_t>::max();

  [[nodiscard]] static constexpr bool isValid(EditorFaultPoint point) noexcept {
    return static_cast<std::size_t>(point) < kFaultPointCount;
  }

  [[nodiscard]] static constexpr std::size_t
  toIndex(EditorFaultPoint point) noexcept {
    return static_cast<std::size_t>(point);
  }

  std::array<std::size_t, kFaultPointCount> m_callCounts{};
  std::array<std::size_t, kFaultPointCount> m_failOnCalls = [] {
    std::array<std::size_t, kFaultPointCount> values{};
    values.fill(kDisabled);
    return values;
  }();
};

struct ExactRootStateSnapshot final {
  SceneRootTransform baseline;
  SceneRootTransform current;
  std::uint64_t hierarchyEpoch{0};
  std::uint64_t externalStateSerial{0};
  bool frozen{false};
  std::vector<NodeId> frozenRootIds;

  [[nodiscard]] bool
  operator==(const ExactRootStateSnapshot &other) const noexcept {
    return baseline.translation == other.baseline.translation &&
           baseline.rotation == other.baseline.rotation &&
           baseline.scale == other.baseline.scale &&
           current.translation == other.current.translation &&
           current.rotation == other.current.rotation &&
           current.scale == other.current.scale &&
           hierarchyEpoch == other.hierarchyEpoch &&
           externalStateSerial == other.externalStateSerial &&
           frozen == other.frozen && frozenRootIds == other.frozenRootIds;
  }
};

struct ExactObjectSlotSnapshot final {
  std::uint32_t generation{0};
  bool live{false};
  std::size_t index{0};

  bool operator==(const ExactObjectSlotSnapshot &) const = default;
};

struct ExactDependencySnapshot final {
  GltfDependencyKind kind{GltfDependencyKind::buffer};
  std::size_t objectIndex{0};
  std::string uri;
  std::string resolvedPath;
  std::uint64_t byteSize{0};

  bool operator==(const ExactDependencySnapshot &) const = default;
};

struct ExactDocumentSnapshot final {
  std::string sourcePath;
  GltfDocumentFormat format{GltfDocumentFormat::ascii};
  std::uint64_t revision{0};
  std::uint64_t cleanRevision{0};
  bool dirty{false};
  EditorDocumentAuthority authority{};
  MutationToken token{};
  std::optional<std::size_t> defaultScene;
  SceneId currentScene;
  std::string semanticDigest;
  std::string documentExtensions;
  std::string documentExtras;
  std::string assetExtensions;
  std::string assetExtras;
  std::array<std::vector<EditorObjectId>,
             static_cast<std::size_t>(EditorObjectKind::count)>
      ids;
  std::array<std::vector<ExactObjectSlotSnapshot>,
             static_cast<std::size_t>(EditorObjectKind::count)>
      slots;
  std::array<std::vector<std::string>,
             static_cast<std::size_t>(EditorObjectKind::count)>
      names;
  std::array<std::vector<bool>,
             static_cast<std::size_t>(EditorObjectKind::count)>
      visible;
  std::array<std::vector<bool>,
             static_cast<std::size_t>(EditorObjectKind::count)>
      hasVisibility;
  std::array<std::vector<std::string>,
             static_cast<std::size_t>(EditorObjectKind::count)>
      extensions;
  std::array<std::vector<std::string>,
             static_cast<std::size_t>(EditorObjectKind::count)>
      extras;
  std::vector<std::vector<NodeId>> sceneRoots;
  std::vector<std::vector<NodeId>> nodeChildren;
  std::vector<std::optional<PrimitiveAddress>> primitiveAddresses;
  std::vector<ExactRootStateSnapshot> rootStates;
  std::vector<ExactDependencySnapshot> dependencies;

  bool operator==(const ExactDocumentSnapshot &) const = default;
};

struct ExactHistoryEntrySnapshot final {
  std::string description;
  std::uint64_t snapshotBytes{0};
  RetainedCommandInspection retainedCommand;

  bool operator==(const ExactHistoryEntrySnapshot &) const = default;
};

struct ExactHistorySnapshot final {
  std::vector<ExactHistoryEntrySnapshot> undoOldestToNewest;
  std::vector<ExactHistoryEntrySnapshot> redoOldestToNewest;
  std::optional<EditorTimePoint> lastContinuousPush;
  std::uint64_t retainedSnapshotBytes{0};

  bool operator==(const ExactHistorySnapshot &) const = default;
};

// Projection-heavy tests provide these fresh per-scene digests after a
// successful operation. Core-only history tests leave the vector empty.
struct ExactRuntimeMappingEntry final {
  EditorObjectId documentId{};
  std::uint32_t sceneAssetIndex{std::numeric_limits<std::uint32_t>::max()};
  std::uint64_t runtimeIndex{std::numeric_limits<std::uint64_t>::max()};

  bool operator==(const ExactRuntimeMappingEntry &) const = default;
};

struct ExactProjectionSnapshot final {
  SceneId scene;
  std::string semanticDigest;
  std::string runtimeMappingDigest;
  std::vector<ExactRuntimeMappingEntry> runtimeMapping;
  bool valid{false};

  bool operator==(const ExactProjectionSnapshot &) const = default;
};

struct ExactEditorStateSnapshot final {
  ExactDocumentSnapshot document;
  EditorSelectionSnapshot selection;
  ExactHistorySnapshot history;
  std::vector<ExactProjectionSnapshot> projections;

  bool operator==(const ExactEditorStateSnapshot &) const = default;
};

[[nodiscard]] inline ExactDocumentSnapshot
exactDocumentSnapshot(const GltfDocument &document,
                      const SceneCommands &commands) {
  ExactDocumentSnapshot result;
  result.sourcePath = document.sourcePath().generic_string();
  result.format = document.format();
  result.revision = document.revision();
  result.cleanRevision = document.cleanRevision();
  result.dirty = document.dirty();
  result.authority = document.documentAuthority();
  result.token = commands.token();
  result.defaultScene = document.defaultSceneIndex();
  result.currentScene = document.currentSceneId();
  result.semanticDigest = GltfSemanticCompare::digest(document);
  result.documentExtensions = document.documentExtensionsJson();
  result.documentExtras = document.documentExtrasJson();
  result.assetExtensions = document.assetExtensionsJson();
  result.assetExtras = document.assetExtrasJson();
  for (const auto &dependency : document.dependencies()) {
    result.dependencies.push_back(ExactDependencySnapshot{
        dependency.kind, dependency.objectIndex, dependency.uri,
        dependency.resolvedPath.generic_string(), dependency.byteSize});
  }

  const DocumentInspection inspection = EditorStateInspector::inspect(document);
  for (std::size_t kind = 1u;
       kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind) {
    result.ids[kind] = inspection.idsByIndex[kind];
    result.slots[kind].reserve(inspection.slots[kind].size());
    for (const ObjectSlotInspection &slot : inspection.slots[kind]) {
      result.slots[kind].push_back(
          ExactObjectSlotSnapshot{slot.generation, slot.live, slot.index});
    }
    for (const EditorObjectId id : result.ids[kind]) {
      result.names[kind].emplace_back(document.objectName(id));
      result.visible[kind].push_back(document.objectVisible(id));
      result.hasVisibility[kind].push_back(document.objectHasVisibility(id));
      result.extensions[kind].emplace_back(document.objectExtensionsJson(id));
      result.extras[kind].emplace_back(document.objectExtrasJson(id));
    }
  }

  for (const SceneRootInspection &root : inspection.sceneRoots) {
    result.sceneRoots.push_back(document.sceneRootNodeIds(root.scene));
    result.rootStates.push_back(ExactRootStateSnapshot{
        root.baseline, root.current, root.hierarchyEpoch,
        root.externalStateSerial, root.frozen, root.frozenRootIds});
  }
  for (const EditorObjectId id :
       result.ids[static_cast<std::size_t>(EditorObjectKind::node)]) {
    result.nodeChildren.push_back(document.nodeChildIds(*NodeId::from(id)));
  }
  result.primitiveAddresses.reserve(inspection.primitiveAddresses.size());
  for (const PrimitiveAddress address : inspection.primitiveAddresses)
    result.primitiveAddresses.emplace_back(address);
  return result;
}

[[nodiscard]] inline ExactHistorySnapshot
exactHistorySnapshot(const EditorHistory &history) {
  const HistoryInspection inspection = EditorStateInspector::inspect(history);
  ExactHistorySnapshot result;
  result.undoOldestToNewest.reserve(inspection.undoOldestToNewest.size());
  for (const HistoryEntryInspection &entry : inspection.undoOldestToNewest) {
    result.undoOldestToNewest.push_back(ExactHistoryEntrySnapshot{
        entry.description, entry.snapshotBytes, entry.retainedCommand});
  }
  result.redoOldestToNewest.reserve(inspection.redoOldestToNewest.size());
  for (const HistoryEntryInspection &entry : inspection.redoOldestToNewest) {
    result.redoOldestToNewest.push_back(ExactHistoryEntrySnapshot{
        entry.description, entry.snapshotBytes, entry.retainedCommand});
  }
  result.lastContinuousPush = inspection.lastContinuousPush;
  result.retainedSnapshotBytes = inspection.retainedSnapshotBytes;
  return result;
}

[[nodiscard]] inline ExactEditorStateSnapshot exactEditorStateSnapshot(
    const GltfDocument &document, const SceneCommands &commands,
    const EditorSelectionState &selection, const EditorHistory &history,
    std::vector<ExactProjectionSnapshot> projections = {}) {
  return ExactEditorStateSnapshot{
      exactDocumentSnapshot(document, commands), selection.snapshot(),
      exactHistorySnapshot(history), std::move(projections)};
}

} // namespace demo::editor::testing
