#include <mgif/scene_editor/EditorSession.h>
#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/SceneEditor.h>
#include <mgif/scene_editor/commands/HistoryLifecycle.h>
#include <mgif/scene_editor/testing/EditorStateInspector.h>

#include "support/EditorTestSupport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace demo::editor;
using namespace demo::editor::testing;
using namespace std::chrono_literals;

bool expect(bool condition, std::string_view message) {
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

std::unique_ptr<GltfDocument>
loadDocument(const char *name = "minimal_scene.gltf") {
  auto loaded = GltfDocumentIO::loadCandidate(fixture(name));
  if (!loaded.succeeded())
    return nullptr;
  return std::move(loaded).takeCandidate();
}

class HistoryRig final {
public:
  explicit HistoryRig(std::unique_ptr<GltfDocument> source,
                      EditorLimits limits = kDefaultEditorLimits)
      : document(std::move(source)), commands(*document, limits),
        context{*document, commands, selection},
        history(context, clock, limits) {}

  std::unique_ptr<GltfDocument> document;
  SceneCommands commands;
  EditorSelectionState selection;
  EditorCommandContext context;
  ManualEditorClock clock;
  EditorHistory history;
};

std::unique_ptr<HistoryRig>
makeRig(EditorLimits limits = kDefaultEditorLimits) {
  auto document = loadDocument();
  if (!document)
    return nullptr;
  return std::make_unique<HistoryRig>(std::move(document), limits);
}

void appendU64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u)
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendByte(std::vector<std::uint8_t> &bytes, std::uint8_t value) {
  bytes.push_back(value);
}

void appendBool(std::vector<std::uint8_t> &bytes, bool value) {
  appendByte(bytes, value ? 1u : 0u);
}

void appendU16(std::vector<std::uint8_t> &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void appendString(std::vector<std::uint8_t> &bytes, std::string_view value) {
  appendU64(bytes, static_cast<std::uint64_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

class FakeCommand final : public ContinuousEditorCommand {
public:
  FakeCommand(EditorCommandContext &context, std::string description,
              std::uint64_t bytes,
              EditorCommandKind kind = EditorCommandKind::transform,
              std::optional<EditorObjectId> target = std::nullopt,
              HistoryPolicy policy = HistoryPolicy::undoable,
              bool alreadyExecuted = false)
      : m_description(std::move(description)), m_bytes(bytes), m_kind(kind),
        m_target(target), m_policy(policy), m_token(context.mutations.token()),
        m_authority(context.document.documentAuthority()) {
    if (alreadyExecuted) {
      m_lifecycle = CommandLifecycleState::executed;
      m_stateRevision = 1;
    }
  }

  FakeCommand &failExecute(bool value = true) noexcept {
    m_failExecute = value;
    return *this;
  }

  FakeCommand &unchangedExecute(bool value = true) noexcept {
    m_unchangedExecute = value;
    return *this;
  }

  FakeCommand &failUndo(bool value = true) noexcept {
    m_failUndo = value;
    return *this;
  }

  FakeCommand &failRedo(bool value = true) noexcept {
    m_failRedo = value;
    return *this;
  }

  FakeCommand &envelope(bool value = true) noexcept {
    m_envelope = value;
    return *this;
  }

  FakeCommand &validation(ExecutedValidation value) noexcept {
    m_validation = value;
    return *this;
  }

  FakeCommand &mergeBehavior(
      bool offer, bool absorb,
      std::optional<std::uint64_t> mergedBytes = std::nullopt) noexcept {
    m_offerMerge = offer;
    m_absorb = absorb;
    m_mergedBytes = mergedBytes;
    return *this;
  }

  FakeCommand &finalValue(std::uint64_t value) noexcept {
    m_finalValue = value;
    return *this;
  }

  FakeCommand &corruptInspection(bool value = true) noexcept {
    m_corruptInspection = value;
    return *this;
  }

  FakeCommand &callbackCount(std::size_t &value) noexcept {
    m_callbackCount = &value;
    return *this;
  }

  FakeCommand &throwExecuteBadAlloc(bool value = true) noexcept {
    m_throwExecuteBadAlloc = value;
    return *this;
  }

  FakeCommand &throwUndoInternal(bool value = true) noexcept {
    m_throwUndoInternal = value;
    return *this;
  }

  FakeCommand &throwRedoBadAlloc(bool value = true) noexcept {
    m_throwRedoBadAlloc = value;
    return *this;
  }

  [[nodiscard]] CommandResult execute(EditorCommandContext &context) override {
    countCallback();
    if (m_throwExecuteBadAlloc)
      throw std::bad_alloc{};
    if (m_failExecute || m_lifecycle != CommandLifecycleState::prepared)
      return rejected(context, MutationErrorCode::internalError);
    if (m_unchangedExecute) {
      CommandResult result = rejected(context, MutationErrorCode::none);
      result.status = CommandStatus::unchanged;
      return result;
    }
    m_lifecycle = CommandLifecycleState::executed;
    ++m_stateRevision;
    return accepted(context);
  }

  [[nodiscard]] CommandResult undo(EditorCommandContext &context) override {
    countCallback();
    if (m_throwUndoInternal)
      throw std::runtime_error("injected Undo exception");
    if (m_failUndo || m_lifecycle != CommandLifecycleState::executed)
      return rejected(context, MutationErrorCode::staleTransaction);
    m_lifecycle = CommandLifecycleState::undone;
    ++m_stateRevision;
    return accepted(context);
  }

  [[nodiscard]] CommandResult redo(EditorCommandContext &context) override {
    countCallback();
    if (m_throwRedoBadAlloc)
      throw std::bad_alloc{};
    if (m_failRedo || m_lifecycle != CommandLifecycleState::undone)
      return rejected(context, MutationErrorCode::staleTransaction);
    m_lifecycle = CommandLifecycleState::executed;
    ++m_stateRevision;
    return accepted(context);
  }

  [[nodiscard]] HistoryPolicy historyPolicy() const noexcept override {
    return m_policy;
  }

  [[nodiscard]] std::uint64_t snapshotBytes() const noexcept override {
    return m_bytes;
  }

  [[nodiscard]] std::string_view description() const noexcept override {
    return m_description;
  }

  [[nodiscard]] std::optional<CommandMergeKey>
  mergeKey() const noexcept override {
    if (!m_target)
      return std::nullopt;
    return CommandMergeKey{m_kind, *m_target};
  }

  [[nodiscard]] RetainedCommandInspection
  inspectRetainedState() const override {
    RetainedCommandInspection result;
    result.kind = m_kind;
    result.lifecycle = m_lifecycle;
    result.stateRevision = m_stateRevision;
    result.retainedSnapshotBytes = m_bytes;
    result.stableTarget = m_target;
    result.authority = m_authority;
    result.preparedToken = m_token;
    result.beforePayload.typeTag = "fake-u64";
    appendU64(result.beforePayload.canonicalBytes, m_initialValue);
    appendString(result.beforePayload.canonicalBytes, m_description);
    appendU64(result.beforePayload.canonicalBytes,
              static_cast<std::uint64_t>(m_description.capacity()));
    appendByte(result.beforePayload.canonicalBytes,
               static_cast<std::uint8_t>(m_policy));
    appendBool(result.beforePayload.canonicalBytes, m_validation.has_value());
    if (m_validation) {
      appendByte(result.beforePayload.canonicalBytes,
                 static_cast<std::uint8_t>(m_validation->status));
      appendU16(result.beforePayload.canonicalBytes,
                static_cast<std::uint16_t>(m_validation->error));
    }
    appendBool(result.beforePayload.canonicalBytes, m_mergedBytes.has_value());
    if (m_mergedBytes)
      appendU64(result.beforePayload.canonicalBytes, *m_mergedBytes);
    appendBool(result.beforePayload.canonicalBytes, m_failExecute);
    appendBool(result.beforePayload.canonicalBytes, m_unchangedExecute);
    appendBool(result.beforePayload.canonicalBytes, m_failUndo);
    appendBool(result.beforePayload.canonicalBytes, m_failRedo);
    appendBool(result.beforePayload.canonicalBytes, m_envelope);
    appendBool(result.beforePayload.canonicalBytes, m_offerMerge);
    appendBool(result.beforePayload.canonicalBytes, m_absorb);
    appendBool(result.beforePayload.canonicalBytes, m_throwExecuteBadAlloc);
    appendBool(result.beforePayload.canonicalBytes, m_throwUndoInternal);
    appendBool(result.beforePayload.canonicalBytes, m_throwRedoBadAlloc);
    result.finalPayload.typeTag = "fake-u64";
    appendU64(result.finalPayload.canonicalBytes, m_finalValue);
    result.executeSelection = EditorSelectionSnapshot::empty();
    result.undoSelection = EditorSelectionSnapshot::empty();
    result.redoSelection = EditorSelectionSnapshot::empty();
    detail::finalizeRetainedCommandInspection(result);
    if (m_corruptInspection)
      result.canonicalSha256[0] ^= 0xffu;
    return result;
  }

  [[nodiscard]] ExecutedValidation
  validateExecuted(const EditorCommandContext &) const noexcept override {
    if (m_validation)
      return *m_validation;
    return m_lifecycle == CommandLifecycleState::executed
               ? ExecutedValidation{ExecutedValidationStatus::matches,
                                    MutationErrorCode::none}
               : ExecutedValidation{};
  }

  [[nodiscard]] std::optional<std::uint64_t>
  mergedSnapshotBytes(const EditorCommand &newer) const noexcept override {
    const auto *typed = dynamic_cast<const FakeCommand *>(&newer);
    if (!m_offerMerge || typed == nullptr)
      return std::nullopt;
    if (m_mergedBytes)
      return m_mergedBytes;
    std::uint64_t result = 0;
    return editorCheckedAdd(m_bytes, typed->m_bytes, result)
               ? std::optional<std::uint64_t>{result}
               : std::nullopt;
  }

  [[nodiscard]] bool tryAbsorbNewer(EditorCommand &newer) noexcept override {
    auto *typed = dynamic_cast<FakeCommand *>(&newer);
    if (!m_absorb || typed == nullptr || !typed->m_absorb || !m_offerMerge)
      return false;
    const auto bytes = mergedSnapshotBytes(newer);
    if (!bytes)
      return false;
    m_finalValue = typed->m_finalValue;
    m_bytes = *bytes;
    ++m_stateRevision;
    return true;
  }

  [[nodiscard]] ContinuousValueKind valueKind() const noexcept override {
    return ContinuousValueKind::nodeTransform;
  }

  [[nodiscard]] SnapshotByteResult prospectiveSnapshotBytes(
      const ContinuousCommandValue &value) const noexcept override {
    return std::holds_alternative<NodeTransform>(value)
               ? SnapshotByteResult{MutationErrorCode::none, m_bytes}
               : SnapshotByteResult{MutationErrorCode::invalidSnapshot, 0};
  }

  [[nodiscard]] CommandResult
  applyPreview(EditorCommandContext &context,
               const ContinuousCommandValue &) override {
    return rejected(context, MutationErrorCode::invalidSnapshot);
  }

  [[nodiscard]] bool initialEqualsFinal() const noexcept override {
    return m_initialValue == m_finalValue;
  }

private:
  [[nodiscard]] CommandResult accepted(EditorCommandContext &context) const {
    CommandResult result;
    result.status = m_envelope ? CommandStatus::historyEnvelopeNoop
                               : CommandStatus::applied;
    result.error = MutationErrorCode::none;
    result.historyPolicy = m_policy;
    result.beforeRevision = context.document.revision();
    result.afterRevision = context.document.revision();
    return result;
  }

  [[nodiscard]] CommandResult rejected(EditorCommandContext &context,
                                       MutationErrorCode error) const {
    CommandResult result;
    result.status = CommandStatus::rejected;
    result.error = error;
    result.historyPolicy = m_policy;
    result.beforeRevision = context.document.revision();
    result.afterRevision = context.document.revision();
    return result;
  }

  void countCallback() noexcept {
    if (m_callbackCount != nullptr)
      ++*m_callbackCount;
  }

  std::string m_description;
  std::uint64_t m_bytes{0};
  EditorCommandKind m_kind{EditorCommandKind::transform};
  std::optional<EditorObjectId> m_target;
  HistoryPolicy m_policy{HistoryPolicy::undoable};
  MutationToken m_token{};
  EditorDocumentAuthority m_authority{};
  CommandLifecycleState m_lifecycle{CommandLifecycleState::prepared};
  std::uint64_t m_stateRevision{0};
  std::uint64_t m_initialValue{0};
  std::uint64_t m_finalValue{0};
  std::optional<ExecutedValidation> m_validation;
  std::optional<std::uint64_t> m_mergedBytes;
  std::size_t *m_callbackCount{nullptr};
  bool m_failExecute{false};
  bool m_unchangedExecute{false};
  bool m_failUndo{false};
  bool m_failRedo{false};
  bool m_envelope{false};
  bool m_offerMerge{true};
  bool m_absorb{true};
  bool m_corruptInspection{false};
  bool m_throwExecuteBadAlloc{false};
  bool m_throwUndoInternal{false};
  bool m_throwRedoBadAlloc{false};
};

std::unique_ptr<FakeCommand>
fake(HistoryRig &rig, std::string description, std::uint64_t bytes = 1,
     bool executed = false, std::optional<EditorObjectId> target = std::nullopt,
     EditorCommandKind kind = EditorCommandKind::transform,
     HistoryPolicy policy = HistoryPolicy::undoable) {
  return std::make_unique<FakeCommand>(rig.context, std::move(description),
                                       bytes, kind, target, policy, executed);
}

bool testCoreExecute() {
  auto rig = makeRig();
  if (!expect(rig != nullptr, "core fixture loads"))
    return false;
  bool passed = true;
  const auto initial = exactEditorStateSnapshot(*rig->document, rig->commands,
                                                rig->selection, rig->history);
  passed &=
      expect(rig->history.execute(nullptr).status == HistoryStatus::noCommand &&
                 rig->history.commitExecuted(nullptr).status ==
                     HistoryStatus::noCommand,
             "null execute/commit are no-command results");
  passed &=
      expect(exactEditorStateSnapshot(*rig->document, rig->commands,
                                      rig->selection, rig->history) == initial,
             "null execute/commit preserve complete state");

  std::size_t callbacks = 0;
  auto nonHistory =
      fake(*rig, "non-history", 1, false, std::nullopt,
           EditorCommandKind::transform, HistoryPolicy::nonHistory);
  nonHistory->callbackCount(callbacks);
  passed &= expect(rig->history.execute(std::move(nonHistory)).status ==
                           HistoryStatus::rejected &&
                       callbacks == 0 && rig->history.undoCount() == 0,
                   "policy rejection occurs before callback");
  const ExactHistorySnapshot beforeNonHistoryCommit =
      exactHistorySnapshot(rig->history);
  auto nonHistoryCommit =
      fake(*rig, "non-history-commit", 1, true, std::nullopt,
           EditorCommandKind::transform, HistoryPolicy::nonHistory);
  passed &= expect(
      rig->history.commitExecuted(std::move(nonHistoryCommit)).status ==
              HistoryStatus::rejected &&
          exactHistorySnapshot(rig->history) == beforeNonHistoryCommit,
      "commitExecuted policy rejection precedes validation and clock writes");

  auto unchanged = fake(*rig, "unchanged", 1);
  unchanged->unchangedExecute();
  const auto beforeUnchanged = exactEditorStateSnapshot(
      *rig->document, rig->commands, rig->selection, rig->history);
  passed &= expect(rig->history.execute(std::move(unchanged)).status ==
                           HistoryStatus::rejected &&
                       exactEditorStateSnapshot(*rig->document, rig->commands,
                                                rig->selection, rig->history) ==
                           beforeUnchanged,
                   "ordinary unchanged execute preserves the complete state");

  passed &=
      expect(rig->history.execute(fake(*rig, "one", 3)).status ==
                     HistoryStatus::pushed &&
                 rig->history.undoDescription() == "one" &&
                 rig->history.retainedSnapshotBytes() == 3,
             "one discrete command pushes with exact description and bytes");
  passed &= expect(rig->history.execute(fake(*rig, "two", 5)).status ==
                           HistoryStatus::pushed &&
                       rig->history.undoDescription() == "two" &&
                       rig->history.undoCount() == 2,
                   "many discrete commands preserve LIFO top");
  const std::uint64_t bytesBeforeMove = rig->history.retainedSnapshotBytes();
  passed &= expect(rig->history.undo().status == HistoryStatus::moved &&
                       rig->history.undoDescription() == "one" &&
                       rig->history.redoDescription() == "two" &&
                       rig->history.retainedSnapshotBytes() == bytesBeforeMove,
                   "Undo moves the exact top command without changing bytes");
  passed &=
      expect(rig->history.redo().status == HistoryStatus::moved &&
                 rig->history.undoDescription() == "two" &&
                 rig->history.retainedSnapshotBytes() == bytesBeforeMove,
             "Redo moves the exact source top back without changing bytes");

  const ExactHistorySnapshot beforeThrownExecute =
      exactHistorySnapshot(rig->history);
  auto thrownExecute = fake(*rig, "execute-throws", 4);
  thrownExecute->throwExecuteBadAlloc();
  passed &= expect(
      rig->history.execute(std::move(thrownExecute)).command.error ==
              MutationErrorCode::allocationFailed &&
          exactHistorySnapshot(rig->history) == beforeThrownExecute,
      "bad_alloc from execute is typed and preserves both stacks exactly");

  auto failUndo = fake(*rig, "undo-fails", 7);
  failUndo->failUndo();
  passed &= expect(rig->history.execute(std::move(failUndo)).succeeded(),
                   "failing-Undo command first executes");
  const ExactHistorySnapshot beforeFailedUndo =
      exactHistorySnapshot(rig->history);
  passed &=
      expect(rig->history.undo().status == HistoryStatus::rejected &&
                 exactHistorySnapshot(rig->history) == beforeFailedUndo,
             "failed Undo preserves source stack and retained command bytes");

  auto redoRig = makeRig();
  if (!expect(redoRig != nullptr, "redo-failure fixture loads"))
    return false;
  auto failRedo = fake(*redoRig, "redo-fails", 9);
  failRedo->failRedo();
  passed &= expect(redoRig->history.execute(std::move(failRedo)).succeeded() &&
                       redoRig->history.undo().succeeded(),
                   "failing-Redo command reaches redo stack");
  const ExactHistorySnapshot beforeFailedRedo =
      exactHistorySnapshot(redoRig->history);
  passed &=
      expect(redoRig->history.redo().status == HistoryStatus::rejected &&
                 exactHistorySnapshot(redoRig->history) == beforeFailedRedo,
             "failed Redo preserves source stack and retained command bytes");

  auto thrownUndoRig = makeRig();
  if (!expect(thrownUndoRig != nullptr, "throwing-Undo fixture loads"))
    return false;
  auto thrownUndo = fake(*thrownUndoRig, "undo-throws", 6);
  thrownUndo->throwUndoInternal();
  passed &=
      expect(thrownUndoRig->history.execute(std::move(thrownUndo)).succeeded(),
             "throwing-Undo command first executes");
  const ExactHistorySnapshot beforeThrownUndo =
      exactHistorySnapshot(thrownUndoRig->history);
  passed &= expect(
      thrownUndoRig->history.undo().command.error ==
              MutationErrorCode::internalError &&
          exactHistorySnapshot(thrownUndoRig->history) == beforeThrownUndo,
      "exception from Undo is typed and preserves source ownership");

  auto thrownRedoRig = makeRig();
  if (!expect(thrownRedoRig != nullptr, "throwing-Redo fixture loads"))
    return false;
  auto thrownRedo = fake(*thrownRedoRig, "redo-throws", 6);
  thrownRedo->throwRedoBadAlloc();
  passed &= expect(
      thrownRedoRig->history.execute(std::move(thrownRedo)).succeeded() &&
          thrownRedoRig->history.undo().succeeded(),
      "throwing-Redo command reaches the redo stack");
  const ExactHistorySnapshot beforeThrownRedo =
      exactHistorySnapshot(thrownRedoRig->history);
  passed &= expect(
      thrownRedoRig->history.redo().command.error ==
              MutationErrorCode::allocationFailed &&
          exactHistorySnapshot(thrownRedoRig->history) == beforeThrownRedo,
      "bad_alloc from Redo is typed and preserves source ownership");

  rig->history.clear();
  passed &= expect(rig->history.undo().status == HistoryStatus::empty &&
                       rig->history.redo().status == HistoryStatus::empty &&
                       !rig->history.canUndo() && !rig->history.canRedo() &&
                       rig->clock.nowCallCount() == 0,
                   "discrete/null/rejected/Undo/Redo paths never sample the "
                   "continuous clock");

  ManualEditorClock sessionClock;
  const auto noDocument = EditorSessionBundle::prepare(
      std::unique_ptr<GltfDocument>{}, sessionClock);
  passed &= expect(!noDocument.succeeded() &&
                       noDocument.status == SessionPrepareStatus::noDocument,
                   "null candidate preparation reports noDocument");
  auto firstDocument = loadDocument("minimal_scene.gltf");
  auto firstPrepared =
      EditorSessionBundle::prepare(std::move(firstDocument), sessionClock);
  ActiveEditorSession active;
  passed &=
      expect(firstPrepared.succeeded() &&
                 active.replace(std::move(firstPrepared.session)),
             "real document/editor session bundle prepares and publishes");
  EditorSessionBundle *const first = active.get();
  const EditorDocumentAuthority firstAuthority =
      first->document().documentAuthority();
  passed &= expect(!active.replace(nullptr) && active.get() == first,
                   "null replacement preserves the active bundle exactly");

  SceneEditor &firstEditor = first->editor();
  const EditorMutationResult firstRoot = firstEditor.addRootNode(
      first->document().currentSceneId(), "Retirement State");
  EditorCommandContext firstContext{first->document(), firstEditor.commands(),
                                    firstEditor.selection()};
  passed &=
      expect(firstRoot.mutation.applied() &&
                 firstEditor.history()
                     .execute(std::make_unique<FakeCommand>(
                         firstContext, "retirement-history", 2))
                     .succeeded() &&
                 !firstEditor.selection().snapshot().emptyValue(),
             "old live bundle holds selection and history before replacement");
  const ExactEditorStateSnapshot oldBundleBeforePreparationFailure =
      exactEditorStateSnapshot(first->document(), firstEditor.commands(),
                               firstEditor.selection(), firstEditor.history());

  ScriptedEditorFaultInjector faults;
  (void)faults.failOnCall(EditorFaultPoint::allocation, 1);
  auto failedPrepared =
      EditorSessionBundle::prepare(loadDocument("feature_preservation.gltf"),
                                   sessionClock, kDefaultEditorLimits, &faults);
  passed &= expect(
      !failedPrepared.succeeded() &&
          failedPrepared.status == SessionPrepareStatus::allocationFailed &&
          active.get() == first &&
          exactEditorStateSnapshot(first->document(), firstEditor.commands(),
                                   firstEditor.selection(),
                                   firstEditor.history()) ==
              oldBundleBeforePreparationFailure,
      "facade allocation failure preserves the complete live old bundle");

  ScriptedEditorFaultInjector secondFault;
  (void)secondFault.failOnCall(EditorFaultPoint::allocation, 2);
  auto secondAllocationFailure = EditorSessionBundle::prepare(
      loadDocument("minimal_scene.gltf"), sessionClock, kDefaultEditorLimits,
      &secondFault);
  passed &=
      expect(!secondAllocationFailure.succeeded() &&
                 secondAllocationFailure.status ==
                     SessionPrepareStatus::allocationFailed &&
                 active.get() == first &&
                 secondFault.callCount(EditorFaultPoint::allocation) == 2 &&
                 exactEditorStateSnapshot(
                     first->document(), firstEditor.commands(),
                     firstEditor.selection(), firstEditor.history()) ==
                     oldBundleBeforePreparationFailure,
             "bundle allocation failure retires no active state");

  auto secondPrepared = EditorSessionBundle::prepare(
      loadDocument("feature_preservation.gltf"), sessionClock);
  const EditorDocumentAuthority secondAuthority =
      secondPrepared.succeeded()
          ? secondPrepared.session->document().documentAuthority()
          : EditorDocumentAuthority{};
  passed &= expect(
      secondPrepared.succeeded() &&
          active.replace(std::move(secondPrepared.session)) &&
          active.get() != nullptr &&
          active.get()->document().documentAuthority() == secondAuthority &&
          secondAuthority != firstAuthority,
      "replacement publishes new bundle before retiring the live old bundle");

  SceneEditor &editor = active.get()->editor();
  const SceneId scene = active.get()->document().currentSceneId();
  EditorCommandContext facadeContext{active.get()->document(),
                                     editor.commands(), editor.selection()};
  const EditorObjectId existingRoot =
      active.get()->document().idAt(EditorObjectKind::node, 0);
  auto retained = std::make_unique<FakeCommand>(
      facadeContext, "save-preserved", 3, EditorCommandKind::transform,
      existingRoot, HistoryPolicy::undoable, true);
  retained->finalValue(42).mergeBehavior(true, true, 3);
  passed &=
      expect(editor.history().commitExecuted(std::move(retained)).succeeded(),
             "session facade seeds retained history and merge time");
  const auto beforeRootAddHistory = exactHistorySnapshot(editor.history());
  const auto rootAdd = editor.addRootNode(scene, "Root Through Facade");
  const EditorSelectionSnapshot rootAddSelection =
      editor.selection().snapshot();
  const auto *rootSelection =
      std::get_if<NodeSelection>(&rootAddSelection.value);
  passed &=
      expect(rootAdd.mutation.applied() &&
                 rootAdd.mutation.historyPolicy == HistoryPolicy::nonHistory &&
                 rootAdd.selectionStatus == SelectionRestoreStatus::restored &&
                 rootSelection != nullptr && rootSelection->scene == scene &&
                 !rootSelection->parent &&
                 active.get()->document().hasNodeOccurrence(
                     scene, rootSelection->node, std::nullopt) &&
                 exactHistorySnapshot(editor.history()) == beforeRootAddHistory,
             "root Add is non-history, selects the created root occurrence, "
             "and preserves retained history/time");

  const ExactEditorStateSnapshot beforeRejectedRoot =
      exactEditorStateSnapshot(active.get()->document(), editor.commands(),
                               editor.selection(), editor.history());
  const EditorMutationResult rejectedRoot =
      editor.addRootNode(SceneId{}, "Must Not Publish");
  passed &= expect(
      !rejectedRoot.mutation.applied() &&
          exactEditorStateSnapshot(active.get()->document(), editor.commands(),
                                   editor.selection(),
                                   editor.history()) == beforeRejectedRoot,
      "rejected root Add preserves prior selection, history, time, and "
      "document");

  std::error_code filesystemError;
  const std::filesystem::path temporaryRoot =
      std::filesystem::temp_directory_path(filesystemError);
  std::filesystem::path saveDirectory;
  if (!filesystemError && temporaryRoot.is_absolute()) {
    saveDirectory =
        temporaryRoot /
        ("mgif-p4-03-save-as-" +
         std::to_string(active.get()->document().documentAuthority().value));
    std::filesystem::create_directories(saveDirectory, filesystemError);
  }
  passed &=
      expect(!filesystemError && !saveDirectory.empty(),
             "Save As test directory is available outside the source tree");
  if (!filesystemError && !saveDirectory.empty()) {
    const std::filesystem::path failedTarget = saveDirectory / "failed.gltf";
    ScriptedEditorFaultInjector saveFault;
    (void)saveFault.failOnCall(EditorFaultPoint::allocation, 1);
    GltfSaveOptions failedOptions;
    failedOptions.faultInjector = &saveFault;
    const ExactEditorStateSnapshot beforeFailedSave =
        exactEditorStateSnapshot(active.get()->document(), editor.commands(),
                                 editor.selection(), editor.history());
    const GltfSaveResult failedSave = GltfDocumentIO::saveAs(
        active.get()->document(), failedTarget, failedOptions);
    passed &= expect(
        !failedSave.succeeded() &&
            failedSave.error() == GltfSaveError::injectedFailure &&
            exactEditorStateSnapshot(active.get()->document(),
                                     editor.commands(), editor.selection(),
                                     editor.history()) == beforeFailedSave,
        "failed Save As preserves the complete dirty session snapshot");

    const std::filesystem::path successfulTarget =
        saveDirectory / "successful.gltf";
    const ExactEditorStateSnapshot beforeSuccessfulSave =
        exactEditorStateSnapshot(active.get()->document(), editor.commands(),
                                 editor.selection(), editor.history());
    const GltfSaveResult successfulSave =
        GltfDocumentIO::saveAs(active.get()->document(), successfulTarget);
    ExactEditorStateSnapshot afterSuccessfulSave =
        exactEditorStateSnapshot(active.get()->document(), editor.commands(),
                                 editor.selection(), editor.history());
    const std::filesystem::path canonicalTarget =
        std::filesystem::weakly_canonical(successfulTarget, filesystemError);
    const bool saveMetadataIsExact =
        !filesystemError && successfulSave.succeeded() &&
        afterSuccessfulSave.document.sourcePath ==
            canonicalTarget.generic_string() &&
        afterSuccessfulSave.document.format == GltfDocumentFormat::ascii &&
        afterSuccessfulSave.document.revision ==
            beforeSuccessfulSave.document.revision &&
        afterSuccessfulSave.document.cleanRevision ==
            beforeSuccessfulSave.document.revision &&
        !afterSuccessfulSave.document.dirty;
    afterSuccessfulSave.document.sourcePath =
        beforeSuccessfulSave.document.sourcePath;
    afterSuccessfulSave.document.format = beforeSuccessfulSave.document.format;
    afterSuccessfulSave.document.dependencies =
        beforeSuccessfulSave.document.dependencies;
    afterSuccessfulSave.document.cleanRevision =
        beforeSuccessfulSave.document.cleanRevision;
    afterSuccessfulSave.document.dirty = beforeSuccessfulSave.document.dirty;
    passed &= expect(saveMetadataIsExact &&
                         afterSuccessfulSave == beforeSuccessfulSave,
                     "successful Save As updates only "
                     "path/dependency/format/clean metadata and "
                     "preserves revision, selection, history, and merge time");
  }
  if (!saveDirectory.empty()) {
    filesystemError.clear();
    (void)std::filesystem::remove_all(saveDirectory, filesystemError);
  }
  active.unload();
  passed &=
      expect(active.get() == nullptr,
             "unload atomically publishes no bundle and retires the old one");
  return passed;
}

bool testCapacityMerge() {
  auto rig = makeRig();
  if (!expect(rig != nullptr, "capacity fixture loads"))
    return false;
  bool passed = true;
  for (std::size_t index = 1; index <= 101; ++index) {
    passed &= expect(
        rig->history.execute(fake(*rig, std::to_string(index), 1)).status ==
            HistoryStatus::pushed,
        "capacity command pushes");
    if (index == 99 || index == 100)
      passed &= expect(rig->history.undoCount() == index,
                       "99/100 count boundary is exact");
  }
  const HistoryInspection capacity =
      EditorStateInspector::inspect(rig->history);
  passed &=
      expect(rig->history.undoCount() == 100 &&
                 capacity.undoOldestToNewest.size() == 100 &&
                 capacity.undoOldestToNewest.front().description == "2" &&
                 capacity.undoOldestToNewest.back().description == "101" &&
                 capacity.retainedSnapshotBytes == 100,
             "entry 101 evicts only the oldest and retains 2-101");
  std::size_t undoMoves = 0;
  while (rig->history.undo().status == HistoryStatus::moved)
    ++undoMoves;
  passed &= expect(undoMoves == 100 && rig->history.redoCount() == 100,
                   "capacity history permits exactly 100 successful Undos");
  const HistoryInspection redone = EditorStateInspector::inspect(rig->history);
  passed &= expect(redone.redoOldestToNewest.front().description == "2" &&
                       redone.redoOldestToNewest.back().description == "101",
                   "redo inspection remains original oldest-to-newest order");
  std::size_t redoMoves = 0;
  while (rig->history.redo().status == HistoryStatus::moved)
    ++redoMoves;
  const HistoryInspection afterAllRedos =
      EditorStateInspector::inspect(rig->history);
  passed &= expect(
      redoMoves == 100 && rig->history.undoCount() == 100 &&
          afterAllRedos.undoOldestToNewest.front().description == "2" &&
          afterAllRedos.undoOldestToNewest.back().description == "101",
      "capacity history permits exactly 100 Redos in the original order");

  EditorLimits small = kDefaultEditorLimits;
  small.maxHistoryEntrySnapshotBytes = 8;
  small.maxHistoryAggregateSnapshotBytes = 16;
  auto budget = makeRig(small);
  if (!expect(budget != nullptr, "budget fixture loads"))
    return false;
  passed &=
      expect(budget->history.execute(fake(*budget, "below", 7)).succeeded() &&
                 budget->history.execute(fake(*budget, "at", 8)).succeeded(),
             "below and exactly-at entry budget are admitted");
  const auto exactBudgetState = exactEditorStateSnapshot(
      *budget->document, budget->commands, budget->selection, budget->history);
  std::size_t overBudgetCallbacks = 0;
  auto entryOver = fake(*budget, "entry-over", 9);
  entryOver->callbackCount(overBudgetCallbacks);
  passed &= expect(
      budget->history.execute(std::move(entryOver)).command.error ==
              MutationErrorCode::snapshotBudgetExceeded &&
          overBudgetCallbacks == 0 &&
          exactEditorStateSnapshot(*budget->document, budget->commands,
                                   budget->selection,
                                   budget->history) == exactBudgetState,
      "one-byte-over entry rejection precedes callbacks and is complete-state "
      "atomic");
  passed &= expect(budget->history.execute(fake(*budget, "aggregate-over", 2))
                           .command.error ==
                       MutationErrorCode::snapshotBudgetExceeded,
                   "one-byte-over aggregate rejection is exact");
  passed &= expect(
      budget->history
              .execute(fake(*budget, "overflow",
                            std::numeric_limits<std::uint64_t>::max()))
              .command.error == MutationErrorCode::snapshotBudgetExceeded,
      "checked overflow-sized entry rejects without wrap");

  EditorLimits overflowLimits = kDefaultEditorLimits;
  overflowLimits.maxHistoryEntrySnapshotBytes =
      std::numeric_limits<std::uint64_t>::max();
  overflowLimits.maxHistoryAggregateSnapshotBytes =
      std::numeric_limits<std::uint64_t>::max();
  auto overflowBudget = makeRig(overflowLimits);
  if (!expect(overflowBudget != nullptr, "checked-overflow fixture loads"))
    return false;
  passed &= expect(
      overflowBudget->history.execute(fake(*overflowBudget, "seed", 1))
              .succeeded() &&
          !overflowBudget->history
               .admitProspectivePush(std::numeric_limits<std::uint64_t>::max())
               .admitted,
      "aggregate addition overflow rejects even with maximum configured "
      "limits");

  auto defaultBudget = makeRig();
  if (!expect(defaultBudget != nullptr, "default budget fixture loads"))
    return false;
  constexpr std::uint64_t entryBudget = 8ull * 1024ull * 1024ull;
  constexpr std::uint64_t aggregateBudget = 64ull * 1024ull * 1024ull;
  passed &= expect(
      defaultBudget->history.admitProspectivePush(entryBudget).admitted &&
          !defaultBudget->history.admitProspectivePush(entryBudget + 1u)
               .admitted,
      "default 8 MiB entry boundary is exact");
  for (std::size_t index = 0; index < 8; ++index) {
    passed &= expect(
        defaultBudget->history
            .execute(fake(*defaultBudget, "eight-mib-" + std::to_string(index),
                          entryBudget))
            .succeeded(),
        "default aggregate boundary entry pushes");
  }
  const auto atAggregate =
      EditorStateInspector::inspect(defaultBudget->history);
  passed &= expect(
      atAggregate.retainedSnapshotBytes == aggregateBudget &&
          defaultBudget->history
                  .execute(fake(*defaultBudget, "aggregate-one-byte-over", 1))
                  .command.error == MutationErrorCode::snapshotBudgetExceeded &&
          EditorStateInspector::inspect(defaultBudget->history)
                  .retainedSnapshotBytes == aggregateBudget,
      "default 64 MiB aggregate and one-byte-over boundary are exact");

  EditorLimits evictionLimits = kDefaultEditorLimits;
  evictionLimits.maxHistoryEntrySnapshotBytes = 8;
  evictionLimits.maxHistoryAggregateSnapshotBytes = 107;
  auto evictionBudget = makeRig(evictionLimits);
  if (!expect(evictionBudget != nullptr, "count-eviction budget fixture loads"))
    return false;
  passed &= expect(
      evictionBudget->history.execute(fake(*evictionBudget, "evicted-heavy", 8))
          .succeeded(),
      "count-eviction fixture pushes its heavy oldest entry");
  for (std::size_t index = 1; index <= 99; ++index) {
    passed &= expect(
        evictionBudget->history
            .execute(fake(*evictionBudget, "small-" + std::to_string(index), 1))
            .succeeded(),
        "count-eviction fixture fills the fixed stack");
  }
  const HistoryAdmission replacementAdmission =
      evictionBudget->history.admitProspectivePush(8);
  passed &= expect(
      replacementAdmission.admitted &&
          replacementAdmission.prospectiveBytes == 107 &&
          evictionBudget->history
              .execute(fake(*evictionBudget, "replacement-heavy", 8))
              .succeeded(),
      "aggregate admission subtracts the exact count-evicted entry first");
  const HistoryInspection afterBudgetEviction =
      EditorStateInspector::inspect(evictionBudget->history);
  passed &= expect(
      afterBudgetEviction.undoOldestToNewest.size() == 100 &&
          afterBudgetEviction.undoOldestToNewest.front().description ==
              "small-1" &&
          afterBudgetEviction.undoOldestToNewest.back().description ==
              "replacement-heavy" &&
          afterBudgetEviction.retainedSnapshotBytes == 107,
      "count eviction and aggregate accounting retain the exact 100 entries");

  auto branchBudget = makeRig(small);
  if (!expect(branchBudget != nullptr, "branch budget fixture loads"))
    return false;
  passed &= expect(
      branchBudget->history.execute(fake(*branchBudget, "a", 8)).succeeded() &&
          branchBudget->history.execute(fake(*branchBudget, "b", 8))
              .succeeded() &&
          branchBudget->history.undo().succeeded(),
      "budget branch seeds redo");
  const auto branchBeforeReject = exactHistorySnapshot(branchBudget->history);
  passed &= expect(
      branchBudget->history.execute(fake(*branchBudget, "too-large-branch", 9))
                  .status == HistoryStatus::rejected &&
          exactHistorySnapshot(branchBudget->history) == branchBeforeReject,
      "failed branch admission does not clear redo");
  passed &= expect(
      branchBudget->history.execute(fake(*branchBudget, "fitting-branch", 8))
              .succeeded() &&
          !branchBudget->history.canRedo(),
      "successful branch excludes and then clears redo");

  auto mergeRig = makeRig();
  if (!expect(mergeRig != nullptr, "merge fixture loads"))
    return false;
  const EditorObjectId target =
      mergeRig->document->idAt(EditorObjectKind::node, 0);
  mergeRig->clock.set(EditorTimePoint{});
  auto first = fake(*mergeRig, "first", 4, true, target);
  first->finalValue(1).mergeBehavior(true, true, 4);
  passed &=
      expect(mergeRig->history.commitExecuted(std::move(first)).status ==
                     HistoryStatus::pushed &&
                 mergeRig->clock.nowCallCount() == 1,
             "epoch-zero first commit samples once and pushes rather than "
             "merges");
  mergeRig->clock.set(EditorTimePoint{} + 499ms);
  auto at499 = fake(*mergeRig, "at-499", 4, true, target);
  at499->finalValue(2).mergeBehavior(true, true, 4);
  passed &= expect(mergeRig->history.commitExecuted(std::move(at499)).status ==
                           HistoryStatus::merged &&
                       mergeRig->history.undoCount() == 1,
                   "499ms merges with strict less-than window");
  std::vector<std::uint8_t> newestFinalBytes;
  appendU64(newestFinalBytes, 2);
  const HistoryInspection mergedAt499 =
      EditorStateInspector::inspect(mergeRig->history);
  passed &= expect(
      mergedAt499.undoOldestToNewest[0].description == "first" &&
          mergedAt499.undoOldestToNewest[0]
                  .retainedCommand.finalPayload.canonicalBytes ==
              newestFinalBytes &&
          mergedAt499.undoOldestToNewest[0].retainedCommand.stateRevision == 2,
      "true merge retains the oldest command and adopts the newest final "
      "value");
  mergeRig->clock.set(EditorTimePoint{} + 999ms);
  auto at500 = fake(*mergeRig, "at-500", 4, true, target);
  at500->finalValue(3).mergeBehavior(true, true, 4);
  passed &= expect(mergeRig->history.commitExecuted(std::move(at500)).status ==
                           HistoryStatus::pushed &&
                       mergeRig->history.undoCount() == 2,
                   "500ms splits");
  mergeRig->clock.set(EditorTimePoint{} + 1500ms);
  auto at501 = fake(*mergeRig, "at-501", 4, true, target);
  at501->finalValue(4).mergeBehavior(true, true, 4);
  passed &= expect(mergeRig->history.commitExecuted(std::move(at501)).status ==
                           HistoryStatus::pushed &&
                       mergeRig->history.undoCount() == 3,
                   "501ms splits");

  auto incompatible = makeRig();
  if (!expect(incompatible != nullptr, "incompatible merge fixture loads"))
    return false;
  const EditorObjectId incompatibleTarget =
      incompatible->document->idAt(EditorObjectKind::node, 0);
  EditorObjectId otherTarget = incompatibleTarget;
  ++otherTarget.slot;
  auto incompatibleFirst =
      fake(*incompatible, "same-kind-first", 2, true, incompatibleTarget);
  incompatibleFirst->mergeBehavior(true, true, 2);
  passed &=
      expect(incompatible->history.commitExecuted(std::move(incompatibleFirst))
                 .succeeded(),
             "incompatible merge first command pushes");
  incompatible->clock.set(EditorTimePoint{} + 1ms);
  auto differentTarget =
      fake(*incompatible, "different-target", 2, true, otherTarget);
  differentTarget->mergeBehavior(true, true, 2);
  passed &= expect(
      incompatible->history.commitExecuted(std::move(differentTarget)).status ==
              HistoryStatus::pushed &&
          incompatible->history.undoCount() == 2,
      "same type with a different stable target splits");
  incompatible->clock.set(EditorTimePoint{} + 2ms);
  auto differentKind = fake(*incompatible, "different-kind", 2, true,
                            otherTarget, EditorCommandKind::editLight);
  differentKind->mergeBehavior(true, true, 2);
  passed &= expect(
      incompatible->history.commitExecuted(std::move(differentKind)).status ==
              HistoryStatus::pushed &&
          incompatible->history.undoCount() == 3,
      "different concrete kind with the same target splits");

  auto afterUndoRig = makeRig();
  if (!expect(afterUndoRig != nullptr, "after-Undo merge fixture loads"))
    return false;
  const EditorObjectId exposedTarget =
      afterUndoRig->document->idAt(EditorObjectKind::node, 0);
  EditorObjectId branchTarget = exposedTarget;
  ++branchTarget.slot;
  auto exposed = fake(*afterUndoRig, "exposed", 2, true, exposedTarget);
  exposed->mergeBehavior(true, true, 2).finalValue(1);
  passed &= expect(
      afterUndoRig->history.commitExecuted(std::move(exposed)).succeeded(),
      "after-Undo merge exposed command pushes");
  afterUndoRig->clock.set(EditorTimePoint{} + 1ms);
  auto branchTop = fake(*afterUndoRig, "branch-top", 2, true, branchTarget);
  branchTop->mergeBehavior(true, true, 2);
  passed &= expect(
      afterUndoRig->history.commitExecuted(std::move(branchTop)).succeeded() &&
          afterUndoRig->history.undo().succeeded() &&
          afterUndoRig->history.canRedo(),
      "Undo exposes an older compatible continuous command");
  afterUndoRig->clock.set(EditorTimePoint{} + 2ms);
  auto mergeExposed =
      fake(*afterUndoRig, "merge-exposed", 2, true, exposedTarget);
  mergeExposed->mergeBehavior(true, true, 2).finalValue(2);
  passed &= expect(
      afterUndoRig->history.commitExecuted(std::move(mergeExposed)).status ==
              HistoryStatus::merged &&
          afterUndoRig->history.undoCount() == 1 &&
          !afterUndoRig->history.canRedo(),
      "post-Undo continuous command may merge with exposed top and clear redo");

  auto discreteTopRig = makeRig();
  if (!expect(discreteTopRig != nullptr,
              "compatible discrete top fixture loads"))
    return false;
  const EditorObjectId discreteTarget =
      discreteTopRig->document->idAt(EditorObjectKind::node, 0);
  EditorObjectId timeSeedTarget = discreteTarget;
  ++timeSeedTarget.slot;
  auto timeSeed = fake(*discreteTopRig, "time-seed", 2, true, timeSeedTarget);
  timeSeed->mergeBehavior(true, true, 2);
  passed &= expect(
      discreteTopRig->history.commitExecuted(std::move(timeSeed)).succeeded(),
      "compatible discrete top test seeds time");
  auto discreteTop =
      fake(*discreteTopRig, "discrete-top", 2, false, discreteTarget);
  discreteTop->mergeBehavior(true, true, 2).finalValue(3);
  passed &= expect(
      discreteTopRig->history.execute(std::move(discreteTop)).succeeded(),
      "compatible discrete command becomes top without updating time");
  discreteTopRig->clock.set(EditorTimePoint{} + 1ms);
  auto continuousNewer =
      fake(*discreteTopRig, "continuous-newer", 2, true, discreteTarget);
  continuousNewer->mergeBehavior(true, true, 2).finalValue(4);
  passed &=
      expect(discreteTopRig->history.commitExecuted(std::move(continuousNewer))
                         .status == HistoryStatus::merged &&
                 discreteTopRig->history.undoCount() == 2,
             "continuous commit may merge with a compatible discrete top");

  auto falseRig = makeRig();
  if (!expect(falseRig != nullptr, "false-absorb fixture loads"))
    return false;
  const EditorObjectId falseTarget =
      falseRig->document->idAt(EditorObjectKind::node, 0);
  auto older = fake(*falseRig, "older", 4, true, falseTarget);
  older->finalValue(10).mergeBehavior(true, true, 4);
  const RetainedCommandInspection olderBefore = older->inspectRetainedState();
  passed &=
      expect(falseRig->history.commitExecuted(std::move(older)).succeeded(),
             "false-absorb older command pushes");
  falseRig->clock.set(EditorTimePoint{} + 1ms);
  auto newer = fake(*falseRig, "newer", 5, true, falseTarget);
  newer->finalValue(20).mergeBehavior(true, false, 5);
  const RetainedCommandInspection newerBefore = newer->inspectRetainedState();
  passed &=
      expect(falseRig->history.commitExecuted(std::move(newer)).status ==
                 HistoryStatus::pushed,
             "false absorption splits and pushes the intact newer command");
  const HistoryInspection falseAfter =
      EditorStateInspector::inspect(falseRig->history);
  passed &= expect(
      falseAfter.undoOldestToNewest.size() == 2 &&
          falseAfter.undoOldestToNewest[0].retainedCommand == olderBefore &&
          falseAfter.undoOldestToNewest[1].retainedCommand == newerBefore &&
          olderBefore.beforePayload.canonicalBytes !=
              newerBefore.beforePayload.canonicalBytes &&
          olderBefore.canonicalSha256 != newerBefore.canonicalSha256,
      "false absorption is a strict two-object no-write including exact "
      "description and compatibility bytes");

  auto afterRedoRig = makeRig();
  if (!expect(afterRedoRig != nullptr, "after-Redo merge fixture loads"))
    return false;
  const EditorObjectId afterRedoTarget =
      afterRedoRig->document->idAt(EditorObjectKind::node, 0);
  auto beforeRedo =
      fake(*afterRedoRig, "before-redo", 2, true, afterRedoTarget);
  beforeRedo->mergeBehavior(true, true, 2).finalValue(1);
  passed &= expect(
      afterRedoRig->history.commitExecuted(std::move(beforeRedo)).succeeded() &&
          afterRedoRig->history.undo().succeeded() &&
          afterRedoRig->history.redo().succeeded(),
      "after-Redo merge fixture restores its compatible top");
  afterRedoRig->clock.set(EditorTimePoint{} + 1ms);
  auto mergeAfterRedo =
      fake(*afterRedoRig, "merge-after-redo", 2, true, afterRedoTarget);
  mergeAfterRedo->mergeBehavior(true, true, 2).finalValue(2);
  passed &= expect(
      afterRedoRig->history.commitExecuted(std::move(mergeAfterRedo)).status ==
              HistoryStatus::merged &&
          afterRedoRig->history.undoCount() == 1,
      "continuous commit may merge after Undo/Redo within the prior window");

  EditorLimits mergeLimits = kDefaultEditorLimits;
  mergeLimits.maxHistoryEntrySnapshotBytes = 10;
  mergeLimits.maxHistoryAggregateSnapshotBytes = 20;
  auto fallback = makeRig(mergeLimits);
  if (!expect(fallback != nullptr, "merge fallback fixture loads"))
    return false;
  const EditorObjectId fallbackTarget =
      fallback->document->idAt(EditorObjectKind::node, 0);
  auto fallbackOlder = fake(*fallback, "fallback-old", 6, true, fallbackTarget);
  fallbackOlder->mergeBehavior(true, true, 12);
  passed &= expect(
      fallback->history.commitExecuted(std::move(fallbackOlder)).succeeded(),
      "merge fallback older pushes");
  fallback->clock.set(EditorTimePoint{} + 1ms);
  auto fallbackNewer = fake(*fallback, "fallback-new", 6, true, fallbackTarget);
  fallbackNewer->mergeBehavior(true, true, 12);
  passed &= expect(
      fallback->history.commitExecuted(std::move(fallbackNewer)).status ==
              HistoryStatus::pushed &&
          fallback->history.undoCount() == 2,
      "oversize merged representation falls back to admitted split push");

  auto validationRig = makeRig();
  if (!expect(validationRig != nullptr, "validation fixture loads"))
    return false;
  auto stale = fake(*validationRig, "stale", 1, true, target);
  stale->validation(ExecutedValidation{ExecutedValidationStatus::valueMismatch,
                                       MutationErrorCode::staleTransaction});
  const std::size_t clockCallsBeforeValidation =
      validationRig->clock.nowCallCount();
  const auto beforeValidation = exactEditorStateSnapshot(
      *validationRig->document, validationRig->commands,
      validationRig->selection, validationRig->history);
  passed &= expect(
      validationRig->history.commitExecuted(std::move(stale)).status ==
              HistoryStatus::rejected &&
          exactEditorStateSnapshot(
              *validationRig->document, validationRig->commands,
              validationRig->selection,
              validationRig->history) == beforeValidation &&
          validationRig->clock.nowCallCount() == clockCallsBeforeValidation,
      "executed-state validation rejection precedes clock and stack writes");
  return passed;
}

bool testBranchClear() {
  auto rig = makeRig();
  if (!expect(rig != nullptr, "branch fixture loads"))
    return false;
  bool passed = true;
  passed &=
      expect(rig->history.execute(fake(*rig, "a", 2)).succeeded() &&
                 rig->history.execute(fake(*rig, "b", 3)).succeeded() &&
                 rig->history.undo().succeeded() && rig->history.canRedo(),
             "branch test seeds redo");
  const ExactHistorySnapshot branchBeforeFailure =
      exactHistorySnapshot(rig->history);
  auto failed = fake(*rig, "failed", 1);
  failed->failExecute();
  passed &=
      expect(rig->history.execute(std::move(failed)).status ==
                     HistoryStatus::rejected &&
                 exactHistorySnapshot(rig->history) == branchBeforeFailure,
             "rejected branch preserves redo and all retained state");
  passed &= expect(
      rig->history.execute(fake(*rig, "c", 4)).succeeded() &&
          !rig->history.canRedo() && rig->history.undoDescription() == "c",
      "successful semantic branch clears redo after command success");

  auto timeRig = makeRig();
  if (!expect(timeRig != nullptr, "clear-time fixture loads"))
    return false;
  const EditorObjectId target =
      timeRig->document->idAt(EditorObjectKind::node, 0);
  auto continuous = fake(*timeRig, "continuous", 2, true, target);
  continuous->mergeBehavior(true, true, 2);
  passed &=
      expect(timeRig->history.commitExecuted(std::move(continuous)).succeeded(),
             "continuous command seeds merge timestamp");
  const auto timestamp =
      EditorStateInspector::inspect(timeRig->history).lastContinuousPush;
  passed &=
      expect(timestamp.has_value(), "successful continuous push records time");
  timeRig->history.clear();
  passed &= expect(
      EditorStateInspector::inspect(timeRig->history).lastContinuousPush ==
          timestamp,
      "clear deliberately preserves last continuous time");
  timeRig->clock.set(EditorTimePoint{} + 1ms);
  auto afterClear = fake(*timeRig, "after-clear", 2, true, target);
  afterClear->mergeBehavior(true, true, 2);
  passed &=
      expect(timeRig->history.commitExecuted(std::move(afterClear)).status ==
                     HistoryStatus::pushed &&
                 timeRig->history.undoCount() == 1,
             "empty undo prevents the first post-clear merge");

  const auto timeBeforeDiscrete =
      EditorStateInspector::inspect(timeRig->history).lastContinuousPush;
  passed &= expect(
      timeRig->history.execute(fake(*timeRig, "discrete", 1)).succeeded() &&
          EditorStateInspector::inspect(timeRig->history).lastContinuousPush ==
              timeBeforeDiscrete,
      "discrete execute never updates continuous time");

  const auto lifecycleBefore = exactHistorySnapshot(timeRig->history);
  applyHistoryLifecycle(timeRig->history, HistoryLifecycleEvent::merge,
                        LifecycleCommitState::failed);
  applyHistoryLifecycle(timeRig->history, HistoryLifecycleEvent::compact,
                        LifecycleCommitState::noOp);
  applyHistoryLifecycle(timeRig->history, HistoryLifecycleEvent::saveAs,
                        LifecycleCommitState::changed);
  passed &=
      expect(exactHistorySnapshot(timeRig->history) == lifecycleBefore,
             "failed/no-op Merge/Compact and every Save As preserve history");
  applyHistoryLifecycle(timeRig->history, HistoryLifecycleEvent::merge,
                        LifecycleCommitState::changed);
  passed &= expect(
      !timeRig->history.canUndo() && !timeRig->history.canRedo() &&
          EditorStateInspector::inspect(timeRig->history).lastContinuousPush ==
              timeBeforeDiscrete,
      "changed Merge clears after publication and preserves merge time");
  passed &= expect(
      timeRig->history.execute(fake(*timeRig, "compact-seed", 1)).succeeded(),
      "Compact lifecycle seed pushes");
  applyHistoryLifecycle(timeRig->history, HistoryLifecycleEvent::compact,
                        LifecycleCommitState::changed);
  passed &= expect(!timeRig->history.canUndo(),
                   "changed Compact uses the explicit target clear override");

  auto inspectRig = makeRig();
  if (!expect(inspectRig != nullptr, "inspection fixture loads"))
    return false;
  passed &= expect(
      inspectRig->history.execute(fake(*inspectRig, "inspect", 11)).succeeded(),
      "inspection command pushes");
  const HistoryInspection beforeUndo =
      EditorStateInspector::inspect(inspectRig->history);
  passed &= expect(
      beforeUndo.undoOldestToNewest.size() == 1 &&
          beforeUndo.undoOldestToNewest[0].retainedCommand.lifecycle ==
              CommandLifecycleState::executed &&
          beforeUndo.undoOldestToNewest[0].retainedCommand.stateRevision == 1 &&
          detail::retainedCommandInspectionHashMatches(
              beforeUndo.undoOldestToNewest[0].retainedCommand),
      "history inspector exposes typed lifecycle, revision, bytes, and valid "
      "SHA");
  passed &= expect(inspectRig->history.undo().succeeded(),
                   "inspection command undoes");
  const HistoryInspection afterUndo =
      EditorStateInspector::inspect(inspectRig->history);
  passed &= expect(
      afterUndo.redoOldestToNewest[0].retainedCommand.lifecycle ==
              CommandLifecycleState::undone &&
          afterUndo.redoOldestToNewest[0].retainedCommand.stateRevision == 2 &&
          afterUndo.redoOldestToNewest[0].retainedCommand.canonicalSha256 !=
              beforeUndo.undoOldestToNewest[0].retainedCommand.canonicalSha256,
      "successful internal lifecycle write advances state revision and "
      "fingerprint");

  auto exactBytesRig = makeRig();
  if (!expect(exactBytesRig != nullptr, "exact inspection byte fixture loads"))
    return false;
  constexpr char descriptionBytes[] = "fake\0description";
  const std::string exactDescription{descriptionBytes,
                                     sizeof(descriptionBytes) - 1u};
  if (!expect(exactBytesRig->history
                  .execute(fake(*exactBytesRig, exactDescription, 3))
                  .succeeded(),
              "description with an embedded zero byte pushes"))
    return false;
  const HistoryInspection exactBytesInspection =
      EditorStateInspector::inspect(exactBytesRig->history);
  std::vector<std::uint8_t> expectedPayloadPrefix;
  appendU64(expectedPayloadPrefix, 0);
  appendString(expectedPayloadPrefix, exactDescription);
  const auto &actualPayload = exactBytesInspection.undoOldestToNewest[0]
                                  .retainedCommand.beforePayload.canonicalBytes;
  passed &= expect(
      exactBytesInspection.undoOldestToNewest[0].description ==
              exactDescription &&
          actualPayload.size() >= expectedPayloadPrefix.size() &&
          std::equal(expectedPayloadPrefix.begin(), expectedPayloadPrefix.end(),
                     actualPayload.begin()),
      "history and canonical inspection preserve exact description bytes");

  auto corruptRig = makeRig();
  if (!expect(corruptRig != nullptr, "corrupt inspection fixture loads"))
    return false;
  auto corrupt = fake(*corruptRig, "corrupt", 1);
  corrupt->corruptInspection();
  passed &=
      expect(corruptRig->history.execute(std::move(corrupt)).succeeded(),
             "corrupt-inspection command still exercises inspector invariant");
  bool mismatchRejected = false;
  try {
    (void)EditorStateInspector::inspect(corruptRig->history);
  } catch (const std::logic_error &) {
    mismatchRejected = true;
  }
  passed &= expect(mismatchRejected,
                   "history inspector rejects a canonical SHA mismatch");

  auto envelopeRig = makeRig();
  if (!expect(envelopeRig != nullptr, "envelope fixture loads"))
    return false;
  auto envelope = fake(*envelopeRig, "envelope", 1);
  envelope->envelope();
  passed &=
      expect(envelopeRig->history.execute(std::move(envelope)).status ==
                     HistoryStatus::pushed &&
                 envelopeRig->history.undo().status == HistoryStatus::moved &&
                 envelopeRig->history.redo().status == HistoryStatus::moved,
             "authorized zero-revision envelope moves through both stacks");
  return passed;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: editor_history_core_tests "
                 "<core|capacity-merge|branch-clear>\n";
    return EXIT_FAILURE;
  }
  const std::string_view suite = argv[1];
  bool passed = false;
  if (suite == "core")
    passed = testCoreExecute();
  else if (suite == "capacity-merge")
    passed = testCapacityMerge();
  else if (suite == "branch-clear")
    passed = testBranchClear();
  else {
    std::cerr << "unknown suite: " << suite << '\n';
    return EXIT_FAILURE;
  }
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
