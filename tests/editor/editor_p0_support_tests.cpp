#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/EditorFaultInjection.h>
#include "tests/editor/support/EditorTestSupport.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
	bool expect(bool condition, const char* message)
	{
		if(!condition)
		{
			std::cerr << "FAILED: " << message << '\n';
		}
		return condition;
	}
}

int main()
{
	using namespace std::chrono_literals;
	using demo::editor::EditorFaultPoint;

	bool passed = true;

	demo::editor::testing::ManualEditorClock clock;
	passed &= expect(clock.now() == demo::editor::EditorTimePoint{}, "manual clock starts at the requested time");
	passed &= expect(clock.advance(499ms), "manual clock accepts positive forward movement");
	passed &= expect(clock.now().time_since_epoch() == 499ms, "manual clock exposes the 499 ms merge boundary");
	passed &= expect(clock.advance(1ms), "manual clock reaches the exact merge boundary");
	passed &= expect(clock.now().time_since_epoch() == 500ms, "manual clock exposes the 500 ms merge boundary");
	passed &= expect(clock.advance(1ms), "manual clock moves past the merge boundary");
	passed &= expect(clock.now().time_since_epoch() == 501ms, "manual clock exposes the 501 ms merge boundary");
	clock.set(demo::editor::EditorTimePoint{123ms});
	passed &= expect(clock.now().time_since_epoch() == 123ms, "manual clock set replaces the current time");
	passed &= expect(!clock.advance(-1ms), "manual clock rejects backwards movement");
	passed &= expect(clock.now().time_since_epoch() == 123ms, "rejected backwards movement is a no-op");
	clock.set(demo::editor::EditorTimePoint::max());
	passed &= expect(!clock.advance(1ms), "manual clock rejects overflow");
	passed &= expect(clock.now() == demo::editor::EditorTimePoint::max(), "rejected overflow is a no-op");

	demo::editor::NoEditorFaultInjector noFaults;
	for(uint8_t rawPoint = 0; rawPoint < static_cast<uint8_t>(EditorFaultPoint::count); ++rawPoint)
	{
		passed &= expect(!noFaults.shouldFail(static_cast<EditorFaultPoint>(rawPoint)), "production no-fault injector never fails");
	}

	demo::editor::testing::ScriptedEditorFaultInjector faults;
	passed &= expect(!faults.failOnCall(EditorFaultPoint::write, 0), "zero is not a valid one-based call number");
	passed &= expect(faults.failOnCall(EditorFaultPoint::write, 2), "valid fault schedule is accepted");
	passed &= expect(!faults.shouldFail(EditorFaultPoint::write), "scripted fault permits calls before the selected occurrence");
	passed &= expect(faults.shouldFail(EditorFaultPoint::write), "scripted fault fails on the selected occurrence");
	passed &= expect(!faults.shouldFail(EditorFaultPoint::write), "scripted fault is a single deterministic occurrence");
	passed &= expect(faults.callCount(EditorFaultPoint::write) == 3, "scripted fault records call count");
	passed &= expect(faults.disable(EditorFaultPoint::write), "valid fault point can be disabled");
	passed &= expect(!faults.shouldFail(EditorFaultPoint::write), "disabled fault point no longer fails");
	passed &= expect(faults.failOnCall(EditorFaultPoint::preAtomicReplace, 1), "pre-replace fault can be scheduled");
	passed &= expect(faults.failOnCall(EditorFaultPoint::parse, 2), "fault points have independent schedules");
	passed &= expect(!faults.shouldFail(EditorFaultPoint::parse), "interleaved first parse call succeeds");
	passed &= expect(faults.shouldFail(EditorFaultPoint::preAtomicReplace), "interleaved pre-replace call uses its own counter");
	passed &= expect(faults.shouldFail(EditorFaultPoint::parse), "interleaved second parse call fails independently");
	faults.reset();
	passed &= expect(!faults.shouldFail(EditorFaultPoint::preAtomicReplace), "reset disables every fault point");
	const auto invalidPoint = static_cast<EditorFaultPoint>(255);
	passed &= expect(!faults.failOnCall(EditorFaultPoint::count, 1), "count sentinel cannot be scheduled");
	passed &= expect(!faults.failOnCall(invalidPoint, 1), "invalid fault point cannot be scheduled");
	passed &= expect(!faults.shouldFail(invalidPoint), "invalid fault point fails closed without mutation");
	passed &= expect(faults.callCount(EditorFaultPoint::parse) == 0, "invalid probes do not alias the parse counter");

	passed &= expect(std::string_view(demo::editor::editorFaultPointName(EditorFaultPoint::parse)) == "parse",
	                 "parse fault name is stable");
	passed &= expect(std::string_view(demo::editor::editorFaultPointName(EditorFaultPoint::postParseValidation)) == "post-parse-validation",
	                 "post-parse validation fault name is stable");
	passed &= expect(std::string_view(demo::editor::editorFaultPointName(EditorFaultPoint::allocation)) == "allocation",
	                 "allocation fault name is stable");
	passed &= expect(std::string_view(demo::editor::editorFaultPointName(EditorFaultPoint::stagingFileCreate)) == "staging-file-create",
	                 "staging-file create fault name is stable");
	passed &= expect(std::string_view(demo::editor::editorFaultPointName(EditorFaultPoint::write)) == "write",
	                 "write fault name is stable");
	passed &= expect(std::string_view(demo::editor::editorFaultPointName(EditorFaultPoint::flush)) == "flush",
	                 "flush fault name is stable");
	passed &= expect(std::string_view(demo::editor::editorFaultPointName(EditorFaultPoint::preAtomicReplace)) == "pre-atomic-replace",
	                 "pre-atomic-replace fault name is stable");
	passed &= expect(std::string_view(demo::editor::editorFaultPointName(EditorFaultPoint::candidateUpload)) == "candidate-upload",
	                 "candidate-upload fault name is stable");
	passed &= expect(std::string_view(demo::editor::editorFaultPointName(EditorFaultPoint::preCandidatePublish)) == "pre-candidate-publish",
	                 "pre-candidate-publish fault name is stable");

	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
