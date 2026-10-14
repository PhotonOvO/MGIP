#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfValidator.h>

#include <iostream>
#include <memory>

namespace {

using namespace demo::editor;

bool expect(bool condition, const char *message)
{
	if(!condition) std::cerr << "FAIL: " << message << '\n';
	return condition;
}

} // namespace

int main()
{
	bool passed = true;
	auto empty = GltfDocumentIO::createEmptyCandidate("Untitled Scene");
	passed &= expect(empty.succeeded(), "empty candidate is created");
	passed &= expect(empty.candidate() != nullptr, "empty candidate is retained");
	if(!empty.succeeded() || empty.candidate() == nullptr) return 1;

	GltfDocument *document = empty.candidate();
	passed &= expect(document->objectCount(EditorObjectKind::scene) == 1,
	                 "new candidate contains one scene");
	passed &= expect(document->objectCount(EditorObjectKind::node) == 0,
	                 "new candidate starts without nodes");
	passed &= expect(document->currentSceneIndex() == std::optional<std::size_t>(0),
	                 "new candidate selects its only scene");
	passed &= expect(document->objectName(document->idAt(EditorObjectKind::scene, 0))
	                   == "Untitled Scene",
	                 "new candidate preserves scene name");
	passed &= expect(GltfValidator::validate(*document).valid(),
	                 "new candidate passes document validation");

	return passed ? 0 : 1;
}
