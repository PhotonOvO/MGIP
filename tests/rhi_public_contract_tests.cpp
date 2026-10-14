#include "../common/HandlePool.h"
#include "../rhi/RHIArgumentTable.h"
#include "../rhi/RHIBackend.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHICapabilities.h"
#include "../rhi/RHIDebugCounters.h"
#include "../rhi/RHIDebugValidation.h"
#include "../rhi/RHIResidency.h"
#include "../rhi/RHIResult.h"
#include "../rhi/RHIPipeline.h"
#include "../rhi/RHIResourceLifetime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Keep this executable as a contract test in Release/NDEBUG builds too.
#ifdef NDEBUG
#undef assert
#define assert(condition)                                                                            \
  do                                                                                                 \
  {                                                                                                  \
    if(!(condition))                                                                                 \
    {                                                                                                \
      throw std::runtime_error("contract assertion failed: " #condition);                           \
    }                                                                                                \
  } while(false)
#endif

namespace {

using TestHandle = demo::rhi::BufferHandle;

[[nodiscard]] uint16_t handleOwner(TestHandle handle)
{
  return static_cast<uint16_t>(handle.generation >> 16u);
}

[[nodiscard]] uint16_t handleSlotGeneration(TestHandle handle)
{
  return static_cast<uint16_t>(handle.generation);
}

struct TransactionalValue
{
  static inline bool     throwOnValueConstruction{false};
  static inline bool     throwOnTransferConstruction{false};
  static inline uint32_t assignmentAttempts{0};

  int value{0};

  TransactionalValue() = delete;

  explicit TransactionalValue(int input)
  {
    if(throwOnValueConstruction)
    {
      throw std::runtime_error("injected value construction failure");
    }
    value = input;
  }

  TransactionalValue(const TransactionalValue& other)
  {
    if(throwOnTransferConstruction)
    {
      throw std::runtime_error("injected transfer construction failure");
    }
    value = other.value;
  }

  TransactionalValue(TransactionalValue&& other) noexcept(false)
  {
    if(throwOnTransferConstruction)
    {
      throw std::runtime_error("injected transfer construction failure");
    }
    value       = other.value;
    other.value = -1;
  }

  TransactionalValue& operator=(const TransactionalValue&)
  {
    ++assignmentAttempts;
    throw std::runtime_error("injected copy assignment failure");
  }

  TransactionalValue& operator=(TransactionalValue&&)
  {
    ++assignmentAttempts;
    throw std::runtime_error("injected move assignment failure");
  }

  static void resetFaults()
  {
    throwOnValueConstruction    = false;
    throwOnTransferConstruction = false;
    assignmentAttempts          = 0;
  }
};

template <typename Fn>
[[nodiscard]] bool throwsRuntimeError(Fn&& fn)
{
  try
  {
    std::forward<Fn>(fn)();
  }
  catch(const std::runtime_error&)
  {
    return true;
  }
  return false;
}

void testHandlePoolGenerationSafety()
{
  demo::HandlePool<demo::rhi::BufferHandle, uint32_t> pool;
  const demo::rhi::BufferHandle first = pool.emplace(17u);
  assert(first.isValid());
  assert(pool.liveCount() == 1);
  assert(pool.tryGet(first) != nullptr && *pool.tryGet(first) == 17u);

  assert(pool.destroy(first));
  assert(!pool.destroy(first));
  assert(pool.tryGet(first) == nullptr);

  const demo::rhi::BufferHandle reused = pool.emplace(23u);
  assert(reused.index == first.index);
  assert(reused.generation != first.generation);
  assert(pool.tryGet(first) == nullptr);
  assert(pool.tryGet(reused) != nullptr && *pool.tryGet(reused) == 23u);

  assert(demo::rhi::nextHandleGeneration(
           (std::numeric_limits<uint32_t>::max)()) == 1u);
  demo::HandlePool<demo::rhi::BufferHandle, uint32_t> otherPool;
  const demo::rhi::BufferHandle foreign = otherPool.emplace(99u);
  assert(foreign.generation != reused.generation);
  assert(pool.tryGet(foreign) == nullptr);
  assert(otherPool.tryGet(reused) == nullptr);
}

void testHandlePoolPublicEncoding()
{
  static_assert(sizeof(decltype(TestHandle{}.index)) == sizeof(uint32_t));
  static_assert(sizeof(decltype(TestHandle{}.generation)) == sizeof(uint32_t));
  static_assert(sizeof(TestHandle) == 2 * sizeof(uint32_t));
  static_assert(std::is_trivially_copyable_v<TestHandle>);
  static_assert(std::is_standard_layout_v<TestHandle>);
  static_assert(demo::detail::encodeHandleGeneration(1, 1) == 0x00010001u);
  static_assert(demo::detail::encodeHandleGeneration(
                  (std::numeric_limits<uint16_t>::max)(),
                  (std::numeric_limits<uint16_t>::max)()) ==
                (std::numeric_limits<uint32_t>::max)());

  demo::HandlePool<TestHandle, uint32_t> pool;
  const TestHandle handle = pool.emplace(1u);
  assert(handle.index == 1u);
  assert(handleOwner(handle) != 0);
  assert(handleSlotGeneration(handle) == 1u);
  assert(pool.tryGet(TestHandle{}) == nullptr);
  assert(pool.tryGet(TestHandle{(std::numeric_limits<uint32_t>::max)(), handle.generation}) == nullptr);
}

void testHandlePoolOwnerIdentityNeverReused()
{
  uint16_t firstOwner = 0;
  {
    demo::HandlePool<TestHandle, uint32_t> firstPool;
    firstOwner = handleOwner(firstPool.emplace(1u));
  }

  demo::HandlePool<TestHandle, uint32_t> secondPool;
  const uint16_t secondOwner = handleOwner(secondPool.emplace(2u));
  assert(firstOwner != 0);
  assert(secondOwner > firstOwner);
}

void testHandlePoolAliasingAcrossGrowth()
{
  using Pool = demo::HandlePool<TestHandle, std::string>;
  const std::string expected(4096, 'x');

  Pool lvaluePool;
  const TestHandle source = lvaluePool.emplace(expected);
  for(uint32_t insertion = 0; insertion < 128; ++insertion)
  {
    const std::string* const alias = lvaluePool.tryGet(source);
    assert(alias != nullptr && *alias == expected);
    const TestHandle copied = lvaluePool.emplace(*alias);
    assert(lvaluePool.tryGet(copied) != nullptr);
    assert(*lvaluePool.tryGet(copied) == expected);
  }
  assert(lvaluePool.liveCount() == 129u);
  assert(*lvaluePool.tryGet(source) == expected);

  Pool       rvaluePool;
  TestHandle moving = rvaluePool.emplace(expected);
  for(uint32_t insertion = 0; insertion < 128; ++insertion)
  {
    std::string* const alias = rvaluePool.tryGet(moving);
    assert(alias != nullptr && *alias == expected);
    const TestHandle moved = rvaluePool.emplace(std::move(*alias));
    assert(rvaluePool.tryGet(moved) != nullptr);
    assert(*rvaluePool.tryGet(moved) == expected);
    moving = moved;
  }
  assert(rvaluePool.liveCount() == 129u);
}

void testHandlePoolTransactionalFailures()
{
  using Pool = demo::HandlePool<TestHandle, TransactionalValue>;
  TransactionalValue::resetFaults();

  Pool freshValueFailure;
  TransactionalValue::throwOnValueConstruction = true;
  for(uint32_t attempt = 0; attempt < 3; ++attempt)
  {
    assert(throwsRuntimeError([&] { (void)freshValueFailure.emplace(10); }));
    assert(freshValueFailure.liveCount() == 0);
  }
  TransactionalValue::throwOnValueConstruction = false;
  const TestHandle firstAfterValueFailures = freshValueFailure.emplace(11);
  assert(firstAfterValueFailures.index == 1u);
  assert(freshValueFailure.tryGet(firstAfterValueFailures)->value == 11);

  Pool freshTransferFailure;
  TransactionalValue::throwOnTransferConstruction = true;
  for(uint32_t attempt = 0; attempt < 3; ++attempt)
  {
    assert(throwsRuntimeError([&] { (void)freshTransferFailure.emplace(20); }));
    assert(freshTransferFailure.liveCount() == 0);
  }
  TransactionalValue::throwOnTransferConstruction = false;
  const TestHandle firstAfterTransferFailures = freshTransferFailure.emplace(21);
  assert(firstAfterTransferFailures.index == 1u);
  assert(freshTransferFailure.tryGet(firstAfterTransferFailures)->value == 21);

  Pool appendedFailure;
  const TestHandle appendedSurvivor = appendedFailure.emplace(30);
  TransactionalValue::throwOnTransferConstruction = true;
  assert(throwsRuntimeError([&] { (void)appendedFailure.emplace(31); }));
  assert(appendedFailure.liveCount() == 1u);
  assert(appendedFailure.tryGet(appendedSurvivor) != nullptr);
  assert(appendedFailure.tryGet(appendedSurvivor)->value == 30);
  TransactionalValue::throwOnTransferConstruction = false;
  const TestHandle appendedAfterFailure = appendedFailure.emplace(31);
  assert(appendedAfterFailure.index == 2u);

  Pool reusedFailure;
  const TestHandle freeTail = reusedFailure.emplace(40);
  const TestHandle survivor = reusedFailure.emplace(41);
  const TestHandle freeHead = reusedFailure.emplace(42);
  assert(reusedFailure.destroy(freeTail));
  assert(reusedFailure.destroy(freeHead));
  assert(reusedFailure.liveCount() == 1u);

  TransactionalValue::throwOnValueConstruction = true;
  for(uint32_t attempt = 0; attempt < 3; ++attempt)
  {
    assert(throwsRuntimeError([&] { (void)reusedFailure.emplace(43); }));
    assert(reusedFailure.liveCount() == 1u);
    assert(reusedFailure.tryGet(survivor) != nullptr);
  }
  TransactionalValue::throwOnValueConstruction = false;

  TransactionalValue::throwOnTransferConstruction = true;
  for(uint32_t attempt = 0; attempt < 3; ++attempt)
  {
    assert(throwsRuntimeError([&] { (void)reusedFailure.emplace(44); }));
    assert(reusedFailure.liveCount() == 1u);
    assert(reusedFailure.tryGet(survivor)->value == 41);
  }
  TransactionalValue::throwOnTransferConstruction = false;

  const TestHandle recoveredHead = reusedFailure.emplace(45);
  const TestHandle recoveredTail = reusedFailure.emplace(46);
  assert(recoveredHead.index == freeHead.index);
  assert(recoveredTail.index == freeTail.index);
  assert(recoveredHead.generation != freeHead.generation);
  assert(recoveredTail.generation != freeTail.generation);
  assert(reusedFailure.tryGet(freeHead) == nullptr);
  assert(reusedFailure.tryGet(freeTail) == nullptr);
  assert(reusedFailure.tryGet(recoveredHead)->value == 45);
  assert(reusedFailure.tryGet(recoveredTail)->value == 46);

  assert(TransactionalValue::assignmentAttempts == 0u);
  assert(reusedFailure.destroy(recoveredHead));
  const TestHandle assignmentProof = reusedFailure.emplace(47);
  assert(assignmentProof.index == recoveredHead.index);
  assert(reusedFailure.tryGet(assignmentProof)->value == 47);
  assert(TransactionalValue::assignmentAttempts == 0u);
  TransactionalValue::resetFaults();
}

void testHandlePoolOwnerExhaustion()
{
  demo::detail::HandlePoolOwnerAllocator owners{
    demo::detail::kMaxHandlePoolOwner - 1};
  assert(owners.acquire() == demo::detail::kMaxHandlePoolOwner - 1);
  assert(owners.acquire() == demo::detail::kMaxHandlePoolOwner);
  assert(demo::detail::encodeHandleGeneration(
           static_cast<uint16_t>(demo::detail::kMaxHandlePoolOwner),
           (std::numeric_limits<uint16_t>::max)()) ==
         (std::numeric_limits<uint32_t>::max)());

  bool exhausted = false;
  try
  {
    (void)owners.acquire();
  }
  catch(const std::overflow_error&)
  {
    exhausted = true;
  }
  assert(exhausted);

  exhausted = false;
  try
  {
    (void)owners.acquire();
  }
  catch(const std::overflow_error&)
  {
    exhausted = true;
  }
  assert(exhausted);

  demo::detail::HandlePoolOwnerAllocator invalidOwner{0};
  exhausted = false;
  try
  {
    (void)invalidOwner.acquire();
  }
  catch(const std::overflow_error&)
  {
    exhausted = true;
  }
  assert(exhausted);
}

void testHandlePoolConcurrentOwnerExhaustion()
{
  constexpr uint32_t firstOwner = demo::detail::kMaxHandlePoolOwner - 31u;
  constexpr size_t   threadCount = 96;
  demo::detail::HandlePoolOwnerAllocator owners{firstOwner};
  std::atomic<bool> start{false};
  std::array<uint32_t, threadCount> results{};
  std::vector<std::thread> threads;
  threads.reserve(threadCount);

  for(size_t index = 0; index < threadCount; ++index)
  {
    threads.emplace_back([&, index] {
      while(!start.load(std::memory_order_acquire))
      {
        std::this_thread::yield();
      }
      try
      {
        results[index] = owners.acquire();
      }
      catch(const std::overflow_error&)
      {
        results[index] = 0;
      }
    });
  }

  start.store(true, std::memory_order_release);
  for(std::thread& thread : threads)
  {
    thread.join();
  }

  std::vector<uint32_t> successes;
  for(const uint32_t owner : results)
  {
    if(owner != 0)
    {
      successes.push_back(owner);
    }
  }
  std::sort(successes.begin(), successes.end());
  assert(successes.size() == 32u);
  for(size_t index = 0; index < successes.size(); ++index)
  {
    assert(successes[index] == firstOwner + index);
  }

  bool exhausted = false;
  try
  {
    (void)owners.acquire();
  }
  catch(const std::overflow_error&)
  {
    exhausted = true;
  }
  assert(exhausted);
}

void testHandlePoolSlotGenerationExhaustion()
{
  using Pool = demo::HandlePool<demo::rhi::BufferHandle, uint32_t, 2>;
  Pool pool;

  const demo::rhi::BufferHandle first = pool.emplace(1u);
  assert(pool.destroy(first));
  const demo::rhi::BufferHandle lastGeneration = pool.emplace(2u);
  assert(lastGeneration.index == first.index);
  assert(lastGeneration.generation != first.generation);
  assert(pool.destroy(lastGeneration));

  const demo::rhi::BufferHandle afterRetirement = pool.emplace(3u);
  assert(afterRetirement.index != first.index);
  assert(pool.tryGet(first) == nullptr);
  assert(pool.tryGet(lastGeneration) == nullptr);
  assert(pool.tryGet(afterRetirement) != nullptr);
  assert(*pool.tryGet(afterRetirement) == 3u);
}

void testHandlePoolRetirementAndFreeListOrder()
{
  using RetiringPool = demo::HandlePool<TestHandle, uint32_t, 1>;
  RetiringPool pool;

  const TestHandle first = pool.emplace(1u);
  assert(pool.destroy(first));
  const TestHandle second = pool.emplace(2u);
  assert(second.index != first.index);
  assert(pool.destroy(second));
  const TestHandle third = pool.emplace(3u);
  assert(third.index != first.index && third.index != second.index);
  assert(pool.destroy(third));

  using ReusablePool = demo::HandlePool<TestHandle, uint32_t, 2>;
  ReusablePool reusable;
  const TestHandle freeTail = reusable.emplace(4u);
  const TestHandle freeHead = reusable.emplace(5u);
  assert(reusable.destroy(freeTail));
  assert(reusable.destroy(freeHead));
  const TestHandle reusedHead = reusable.emplace(6u);
  const TestHandle reusedTail = reusable.emplace(7u);
  assert(reusedHead.index == freeHead.index);
  assert(reusedTail.index == freeTail.index);
  assert(reusedHead.generation != freeHead.generation);
  assert(reusedTail.generation != freeTail.generation);
  assert(pool.tryGet(first) == nullptr);
  assert(pool.tryGet(second) == nullptr);
  assert(pool.tryGet(third) == nullptr);
  assert(reusable.liveCount() == 2u);
}

void testHandlePoolMoveOwnership()
{
  using Pool = demo::HandlePool<demo::rhi::BufferHandle, uint32_t>;
  static_assert(std::is_nothrow_move_constructible_v<Pool>);
  static_assert(std::is_nothrow_move_assignable_v<Pool>);

  Pool source;
  const demo::rhi::BufferHandle freed = source.emplace(40u);
  const demo::rhi::BufferHandle transferred = source.emplace(41u);
  assert(source.destroy(freed));
  Pool moved{std::move(source)};
  assert(source.liveCount() == 0);
  assert(source.tryGet(transferred) == nullptr);
  assert(moved.tryGet(transferred) != nullptr);
  assert(*moved.tryGet(transferred) == 41u);
  uint32_t movedFromCallbackCount = 0;
  source.forEachActive([&](auto, auto&) { ++movedFromCallbackCount; });
  assert(movedFromCallbackCount == 0u);
  assert(!source.destroy(transferred));
  const demo::rhi::BufferHandle reusedAfterMove = moved.emplace(42u);
  assert(reusedAfterMove.index == freed.index);
  assert(reusedAfterMove.generation != freed.generation);
  assert(moved.destroy(reusedAfterMove));

  bool movedFromRejected = false;
  try
  {
    (void)source.emplace(42u);
  }
  catch(const std::logic_error&)
  {
    movedFromRejected = true;
  }
  assert(movedFromRejected);

  Pool destination;
  const demo::rhi::BufferHandle replaced = destination.emplace(51u);
  destination = std::move(moved);
  assert(moved.liveCount() == 0);
  assert(moved.tryGet(transferred) == nullptr);
  assert(destination.tryGet(replaced) == nullptr);
  assert(destination.tryGet(transferred) != nullptr);
  assert(*destination.tryGet(transferred) == 41u);
  assert(destination.tryGet(reusedAfterMove) == nullptr);
  const demo::rhi::BufferHandle reusedAfterAssignment = destination.emplace(52u);
  assert(reusedAfterAssignment.index == reusedAfterMove.index);
  assert(reusedAfterAssignment.generation != reusedAfterMove.generation);

  Pool movedFromAssignmentTarget;
  const demo::rhi::BufferHandle retained = movedFromAssignmentTarget.emplace(61u);
  movedFromAssignmentTarget = std::move(source);
  assert(movedFromAssignmentTarget.tryGet(retained) != nullptr);
  assert(*movedFromAssignmentTarget.tryGet(retained) == 61u);
  const demo::rhi::BufferHandle afterMovedFromAssignment =
    movedFromAssignmentTarget.emplace(62u);
  assert(afterMovedFromAssignment.index != retained.index);

  const auto moveAssign = [](Pool& target, Pool& origin) {
    target = std::move(origin);
  };
  moveAssign(destination, destination);
  assert(destination.tryGet(transferred) != nullptr);
  assert(destination.tryGet(reusedAfterAssignment) != nullptr);
}

void testPipelineBindingSchema()
{
  using namespace demo::rhi;
  const std::array layouts{
    ArgumentLayoutHandle{1, 1},
    ArgumentLayoutHandle{2, 1},
  };
  const std::array roots{
    RootBindingDesc{
      .slot = 5,
      .kind = RootBindingKind::constants,
      .visibility = ShaderStage::vertex,
      .size = 16,
      .alignment = 16,
    },
    RootBindingDesc{
      .slot = 9,
      .kind = RootBindingKind::gpuPointer,
      .visibility = ShaderStage::compute,
      .size = kGpuPointerRootBindingSize,
      .alignment = 8,
    },
  };
  const PipelineBindingSchemaStorage storage{layouts, roots};
  const PipelineBindingSchemaDesc schema = storage.view();
  assert(validatePipelineBindingSchema(schema).valid());
  assert(schema.argumentSlots.size() == 2);
  assert(schema.argumentSlots[0].slot == 0);
  assert(schema.argumentSlots[1].slot == 1);
  assert(schema.rootBindings[0].slot == 5);
  assert(schema.rootBindings[1].slot == 9);

  std::array duplicateRoots{roots[0], roots[0]};
  const PipelineBindingSchemaDesc duplicateSchema{
    .rootBindings = duplicateRoots,
  };
  assert(validatePipelineBindingSchema(duplicateSchema).error ==
         PipelineBindingSchemaValidationError::duplicateRootSlot);

  RootBindingDesc invalidAlignment = roots[0];
  invalidAlignment.alignment = 3;
  const PipelineBindingSchemaDesc invalidAlignmentSchema{
    .rootBindings = std::span{&invalidAlignment, 1},
  };
  assert(validatePipelineBindingSchema(invalidAlignmentSchema).error ==
         PipelineBindingSchemaValidationError::invalidRootBindingAlignment);
}

void testQueueOperationCapabilities()
{
  using namespace demo::rhi;
  static_assert(supportsQueueOperation(QueueClass::graphics, QueueOperation::render));
  static_assert(supportsQueueOperation(QueueClass::graphics, QueueOperation::compute));
  static_assert(supportsQueueOperation(QueueClass::graphics, QueueOperation::transfer));
  static_assert(!supportsQueueOperation(QueueClass::compute, QueueOperation::render));
  static_assert(supportsQueueOperation(QueueClass::compute, QueueOperation::compute));
  static_assert(supportsQueueOperation(QueueClass::compute, QueueOperation::transfer));
  static_assert(!supportsQueueOperation(QueueClass::transfer, QueueOperation::render));
  static_assert(!supportsQueueOperation(QueueClass::transfer, QueueOperation::compute));
  static_assert(supportsQueueOperation(QueueClass::transfer, QueueOperation::transfer));
}

void testCommandBufferStateContract()
{
  using demo::rhi::CommandBufferState;
  using demo::rhi::canTransitionCommandBufferState;

  assert(canTransitionCommandBufferState(
    CommandBufferState::idle, CommandBufferState::recording));
  assert(canTransitionCommandBufferState(
    CommandBufferState::recording, CommandBufferState::executable));
  assert(canTransitionCommandBufferState(
    CommandBufferState::executable, CommandBufferState::submitted));
  assert(canTransitionCommandBufferState(
    CommandBufferState::submitted, CommandBufferState::reusable));
  assert(canTransitionCommandBufferState(
    CommandBufferState::reusable, CommandBufferState::recording));

  assert(!canTransitionCommandBufferState(
    CommandBufferState::idle, CommandBufferState::submitted));
  assert(!canTransitionCommandBufferState(
    CommandBufferState::recording, CommandBufferState::submitted));
  assert(!canTransitionCommandBufferState(
    CommandBufferState::submitted, CommandBufferState::recording));
}

void testArgumentTableUpdateContract()
{
  using demo::rhi::ArgumentTableUseState;
  using demo::rhi::canUpdateArgumentTable;

  assert(canUpdateArgumentTable(ArgumentTableUseState::idle, false));
  assert(canUpdateArgumentTable(
    ArgumentTableUseState::executableUnsubmitted, false));
  assert(!canUpdateArgumentTable(
    ArgumentTableUseState::pendingGpuWork, false));
  assert(canUpdateArgumentTable(
    ArgumentTableUseState::pendingGpuWork, true));
  assert(!canUpdateArgumentTable(
    ArgumentTableUseState::idle, true, true));
}

void testDeferredRetirement()
{
  using namespace demo::rhi;
  InlineDeferredDestructionQueue queue;
  const ResourceHandle first{ResourceKind::Buffer, 1, 1};
  const ResourceHandle second{ResourceKind::Texture, 2, 1};
  const QueueIdentity graphics{1, 1};
  const QueueIdentity compute{2, 1};

  SubmissionTokenSet firstDependencies{};
  assert(firstDependencies.record(SubmissionToken{graphics, 4}));
  assert(firstDependencies.record(SubmissionToken{graphics, 3}));
  SubmissionTokenSet secondDependencies{};
  assert(secondDependencies.record(SubmissionToken{graphics, 7}));
  assert(secondDependencies.record(SubmissionToken{compute, 2}));
  queue.enqueue({first, firstDependencies});
  queue.enqueue({second, secondDependencies});

  const std::array incomplete{
    SubmissionToken{graphics, 3},
    SubmissionToken{compute, 2},
  };
  assert(queue.process(incomplete) == 0);

  const std::array graphicsComplete{
    SubmissionToken{graphics, 4},
    SubmissionToken{compute, 2},
  };
  assert(queue.process(graphicsComplete) == 1);
  assert(queue.drainedResources().front() == first);
  assert(queue.pendingRetirements().size() == 1);

  const std::array computeIncomplete{
    SubmissionToken{graphics, 7},
    SubmissionToken{compute, 1},
  };
  assert(queue.process(computeIncomplete) == 0);

  const std::array allComplete{
    SubmissionToken{graphics, 7},
    SubmissionToken{compute, 2},
  };
  assert(queue.process(allComplete) == 1);
  assert(queue.drainedResources().front() == second);
  assert(queue.empty());

  std::array<SubmissionToken, SubmissionTokenSet::capacity + 1> uniqueTokens{
    SubmissionToken{QueueIdentity{10, 1}, 1},
    SubmissionToken{QueueIdentity{11, 1}, 1},
    SubmissionToken{QueueIdentity{12, 1}, 1},
    SubmissionToken{QueueIdentity{13, 1}, 1},
  };
  SubmissionTokenSet bounded{};
  assert(bounded.record(uniqueTokens[0]));
  assert(bounded.record(uniqueTokens[1]));
  assert(bounded.record(uniqueTokens[2]));
  assert(!bounded.record(uniqueTokens[3]));
}

void testResultAndResidencyContract()
{
  using namespace demo::rhi;
  constexpr auto unsupported = Result<ResidencySetHandle>::fail(
    RHIErrorCode::unsupported, "not supported");
  static_assert(!unsupported.succeeded());
  static_assert(unsupported.error.code == RHIErrorCode::unsupported);

  CapabilityReport report{};
  assert(!supportsTier(report, CapabilityTier::Residency));
  report.explicitResidency = ExplicitResidencyLevel::validatedNoOp;
  assert(supportsTier(report, CapabilityTier::Residency));
  CapabilityRequirements requirements{};
  requirements.requireCoreGraphics = false;
  requirements.requireCoreCompute = false;
  requirements.requireCoreBindless = false;
  requirements.requireResidency = true;
  assert(evaluateCapabilityRequirements(report, requirements) == RHICapabilityError::None);

  constexpr BufferHandle buffer{7, 3};
  constexpr ResidencyResource resource = residencyResource(buffer);
  static_assert(resource.kind == ResidencyResourceKind::buffer);
  static_assert(resource.index == buffer.index && resource.generation == buffer.generation);

  constexpr BufferMapDesc map{
    .offset = 16,
    .size = 32,
    .mode = BufferMapMode::readWrite,
  };
  static_assert(map.offset == 16 && map.size == 32);
}

void testBarrierStateValidation()
{
  using namespace demo::rhi;
  DebugResourceStateTracker tracker;

  const BufferBarrier computeWrite{
    .buffer = BufferHandle{1, 1},
    .before = ResourceState::General,
    .after = ResourceState::ShaderWrite,
  };
  assert(tracker.transition(computeWrite).valid());
  const BufferBarrier indirectRead{
    .buffer = computeWrite.buffer,
    .before = ResourceState::ShaderWrite,
    .after = ResourceState::IndirectArgument,
  };
  assert(tracker.transition(indirectRead).valid());

  const TextureBarrier transferWrite{
    .texture = TextureHandle{2, 1},
    .before = ResourceState::Undefined,
    .after = ResourceState::TransferDst,
  };
  assert(tracker.transition(transferWrite).valid());
  TextureBarrier sampled = transferWrite;
  sampled.before = ResourceState::TransferDst;
  sampled.after = ResourceState::ShaderRead;
  assert(tracker.transition(sampled).valid());

  const TextureBarrier renderTarget{
    .texture = TextureHandle{3, 1},
    .before = ResourceState::Undefined,
    .after = ResourceState::ColorAttachment,
  };
  assert(tracker.transition(renderTarget).valid());
  TextureBarrier renderTargetSampled = renderTarget;
  renderTargetSampled.before = ResourceState::ColorAttachment;
  renderTargetSampled.after = ResourceState::ShaderRead;
  assert(tracker.transition(renderTargetSampled).valid());

  const TextureBarrier presentSource{
    .texture = TextureHandle{4, 1},
    .before = ResourceState::ColorAttachment,
    .after = ResourceState::Present,
  };
  assert(tracker.transition(presentSource).valid());
  TextureBarrier mismatch = presentSource;
  mismatch.before = ResourceState::TransferDst;
  mismatch.after = ResourceState::Present;
  assert(tracker.transition(mismatch).error == ResourceStateValidationError::stateMismatch);

  constexpr AliasingResource empty{};
  constexpr AliasingResource buffer =
    AliasingResource::fromBuffer(BufferHandle{9, 2});
  constexpr AliasingResource texture =
    AliasingResource::fromTexture(TextureHandle{10, 3});
  static_assert(empty.isValid());
  static_assert(buffer.isValid() && buffer.kind == AliasingResourceKind::buffer);
  static_assert(texture.isValid() && texture.kind == AliasingResourceKind::texture);
  constexpr AliasingBarrier aliasing{.before = buffer, .after = texture};
  static_assert(aliasing.before.buffer.index == 9);
  static_assert(aliasing.after.texture.index == 10);
}

void testHotPathCounters()
{
  using namespace demo::rhi;
  resetHotPathCounters(BackendType::vulkan);
  incrementHotPathCounter(BackendType::vulkan, HotPathCounter::commandBufferBegins, 2);
  incrementHotPathCounter(BackendType::vulkan, HotPathCounter::queueSubmits);
  RHIHotPathCounters counters = snapshotHotPathCounters(BackendType::vulkan);
  assert(counters.commandBufferBegins == 2);
  assert(counters.queueSubmits == 1);
  assert(counters.stableRecordingBudgetMet());
  incrementHotPathCounter(
    BackendType::vulkan, HotPathCounter::commandRecordingHeapAllocations);
  counters = snapshotHotPathCounters(BackendType::vulkan);
  assert(!counters.stableRecordingBudgetMet());
  resetHotPathCounters(BackendType::vulkan);
}
void testBackendIdentityShape()
{
  using namespace demo::rhi;
  constexpr BackendInfo info{
    .type = BackendType::d3d12,
    .apiName = "Direct3D 12",
    .version = BackendVersion{.major = 12, .minor = 2, .nativeValue = 0xc200},
  };
  static_assert(info.type == BackendType::d3d12);
  static_assert(info.version.major == 12);
  static_assert(info.version.nativeValue == 0xc200);
}

void testEditorTokenFormatContract()
{
  using demo::rhi::TextureFormat;
  static_assert(static_cast<uint8_t>(TextureFormat::r32Uint) ==
                static_cast<uint8_t>(TextureFormat::bc7SrgbBlock) + 1u);
  static_assert(TextureFormat::r32Uint != TextureFormat::r32Sfloat);
  constexpr demo::rhi::BufferDesc raw{
    .size = 64u,
    .usage = demo::rhi::BufferUsageFlags::storage,
  };
  constexpr demo::rhi::BufferDesc structured{
    .size = 64u,
    .usage = demo::rhi::BufferUsageFlags::storage,
    .structuredElementStride = 16u,
  };
  static_assert(raw.structuredElementStride == 0u);
  static_assert(structured.structuredElementStride == 16u &&
                structured.size % structured.structuredElementStride == 0u);
}

}  // namespace

int main()
{
  testHandlePoolPublicEncoding();
  testHandlePoolGenerationSafety();
  testHandlePoolOwnerIdentityNeverReused();
  testHandlePoolAliasingAcrossGrowth();
  testHandlePoolTransactionalFailures();
  testHandlePoolOwnerExhaustion();
  testHandlePoolConcurrentOwnerExhaustion();
  testHandlePoolSlotGenerationExhaustion();
  testHandlePoolRetirementAndFreeListOrder();
  testHandlePoolMoveOwnership();
  testPipelineBindingSchema();
  testQueueOperationCapabilities();
  testCommandBufferStateContract();
  testArgumentTableUpdateContract();
  testDeferredRetirement();
  testResultAndResidencyContract();
  testBarrierStateValidation();
  testHotPathCounters();
  testBackendIdentityShape();
  testEditorTokenFormatContract();
  return 0;
}
