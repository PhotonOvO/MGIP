#include "app/editor/MaterialClipboardCodec.h"

#include <mgif/scene_editor/GltfDocumentIO.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if !defined(MGIF_TEST_SOURCE_DIR)
#define MGIF_TEST_SOURCE_DIR "."
#endif

namespace {

using namespace demo::editor;

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

std::unique_ptr<GltfDocument> loadDocument(const char *name) {
  GltfLoadResult loaded = GltfDocumentIO::loadCandidate(fixture(name));
  return loaded.succeeded() ? std::move(loaded).takeCandidate() : nullptr;
}

MaterialId materialAt(const GltfDocument &document, std::size_t index) {
  return MaterialId::from(
             document.idAt(EditorObjectKind::material, index))
      .value_or(MaterialId{});
}

class FakeClipboard final : public IOsTextClipboard {
public:
  [[nodiscard]] std::optional<std::string> readText() const override {
    return text;
  }

  bool writeText(std::string_view value) override {
    ++writeCount;
    if (rejectWrites) {
      return false;
    }
    text = std::string{value};
    return true;
  }

  std::optional<std::string> text;
  std::size_t writeCount{0};
  bool rejectWrites{false};
};

bool testStrictGrammarAndCanonicalEncoding() {
  bool passed = true;
  passed &= expect(
      MaterialClipboardCodec::parse(std::nullopt).syntax ==
              MaterialClipboardSyntax::absent &&
          MaterialClipboardCodec::parse(std::string_view{}).syntax ==
              MaterialClipboardSyntax::absent,
      "absent and empty clipboard text remain an explicit absent partition");

  const std::array<std::string_view, 10> malformed{
      "material:1",        "gltf_material:",
      "gltf_material:-1",  "gltf_material:+1",
      "gltf_material: 1",  "gltf_material:1 ",
      "gltf_material:abc", "gltf_material:1tail",
      "gltf_material:1.5", " gltf_material:1"};
  for (const std::string_view text : malformed) {
    const ParsedMaterialClipboard parsed = MaterialClipboardCodec::parse(text);
    passed &= expect(parsed.syntax == MaterialClipboardSyntax::malformed &&
                         !parsed.documentIndex,
                     "malformed clipboard grammar fails closed");
  }

  const std::string non_ascii_digit = "gltf_material:\xD9\xA1";
  const std::string overflow = "gltf_material:18446744073709551616";
  passed &= expect(
      MaterialClipboardCodec::parse(std::string_view{non_ascii_digit}).syntax ==
              MaterialClipboardSyntax::malformed &&
          MaterialClipboardCodec::parse(std::string_view{overflow}).syntax ==
              MaterialClipboardSyntax::malformed,
      "non-ASCII digits and checked uint64 overflow are rejected");

  const ParsedMaterialClipboard leading_zero =
      MaterialClipboardCodec::parse(std::string_view{"gltf_material:00042"});
  const ParsedMaterialClipboard maximum = MaterialClipboardCodec::parse(
      std::string_view{"gltf_material:18446744073709551615"});
  passed &= expect(
      leading_zero.syntax == MaterialClipboardSyntax::parsed &&
          leading_zero.documentIndex == 42u &&
          maximum.documentIndex == std::numeric_limits<std::uint64_t>::max() &&
          MaterialClipboardCodec::encode(0u) == "gltf_material:0" &&
          MaterialClipboardCodec::encode(42u) == "gltf_material:42" &&
          MaterialClipboardCodec::encode(
              std::numeric_limits<std::uint64_t>::max()) ==
              "gltf_material:18446744073709551615",
      "input accepts leading zeroes while encoder always emits canonical ASCII");
  return passed;
}

bool testAvailabilityAndCopy() {
  auto two_materials = loadDocument("r09_shared_mesh.gltf");
  if (!expect(two_materials != nullptr,
              "material availability fixture loads")) {
    return false;
  }
  const MaterialId material0 = materialAt(*two_materials, 0u);
  const MaterialId material1 = materialAt(*two_materials, 1u);
  if (!expect(material0.valid() && material1.valid(),
              "fixture supplies two stable material IDs")) {
    return false;
  }

  bool passed = true;
  const MaterialPasteAvailability malformed =
      MaterialClipboardCodec::resolveText(
          *two_materials, std::string_view{"gltf_material:1tail"});
  const MaterialPasteAvailability valid =
      MaterialClipboardCodec::resolveText(
          *two_materials, std::string_view{"gltf_material:1"});
  const MaterialPasteAvailability out_of_range =
      MaterialClipboardCodec::resolveText(
          *two_materials, std::string_view{"gltf_material:999"});
  passed &= expect(!malformed.menuEnabled && !malformed.applicable(),
                   "malformed clipboard disables Paste");
  passed &= expect(
      valid.menuEnabled && valid.indexInActiveDocument && valid.applicable() &&
          valid.activeDocumentMaterial == material1 &&
          valid.wouldChange(material0) && !valid.wouldChange(material1),
      "in-range Paste resolves active-document ID and same assignment is no-op");
  passed &= expect(
      out_of_range.menuEnabled && !out_of_range.indexInActiveDocument &&
          !out_of_range.activeDocumentMaterial && !out_of_range.applicable(),
      "canonical out-of-range text stays enabled but cannot apply");

  FakeClipboard clipboard;
  passed &= expect(
      MaterialClipboardCodec::copy(*two_materials, material1, clipboard) &&
          clipboard.text == "gltf_material:1" &&
          MaterialClipboardCodec::copy(*two_materials, material1, clipboard) &&
          clipboard.writeCount == 2u,
      "Copy resolves current document index each time and emits exact text");

  auto foreign = loadDocument("r09_shared_mesh.gltf");
  if (!expect(foreign != nullptr, "foreign clipboard fixture loads")) {
    return false;
  }
  passed &= expect(
      !MaterialClipboardCodec::copy(*foreign, material1, clipboard),
      "copy rejects a stable material ID from another authority");
  clipboard.rejectWrites = true;
  passed &= expect(
      !MaterialClipboardCodec::copy(*two_materials, material0, clipboard),
      "OS clipboard write failure is reported without document mutation");
  return passed;
}

bool testClickTimeCrossDocumentRevalidation() {
  auto document_a = loadDocument("r09_shared_mesh.gltf");
  auto document_b = loadDocument("r09_shared_mesh.gltf");
  auto document_c = loadDocument("feature_preservation.gltf");
  if (!expect(document_a != nullptr && document_b != nullptr &&
                  document_c != nullptr,
              "cross-document clipboard fixtures load")) {
    return false;
  }

  FakeClipboard clipboard;
  clipboard.text = "gltf_material:1";
  const MaterialPasteAvailability drawn_in_a =
      MaterialClipboardCodec::resolveOnPasteClick(*document_a, clipboard);
  const MaterialPasteAvailability clicked_in_b =
      MaterialClipboardCodec::resolveOnPasteClick(*document_b, clipboard);
  const MaterialPasteAvailability clicked_in_c =
      MaterialClipboardCodec::resolveOnPasteClick(*document_c, clipboard);
  bool passed = true;
  passed &= expect(
      drawn_in_a.applicable() && clicked_in_b.applicable() &&
          drawn_in_a.activeDocumentMaterial->authority() ==
              document_a->documentAuthority() &&
          clicked_in_b.activeDocumentMaterial->authority() ==
              document_b->documentAuthority() &&
          drawn_in_a.activeDocumentMaterial !=
              clicked_in_b.activeDocumentMaterial,
      "same index is resolved again to the document active at click time");
  passed &= expect(
      clicked_in_c.menuEnabled && !clicked_in_c.indexInActiveDocument &&
          !clicked_in_c.activeDocumentMaterial,
      "document switch can make the same canonical index invalid without stale ID");

  clipboard.text = "gltf_material:0";
  const MaterialPasteAvailability retry_c =
      MaterialClipboardCodec::resolveOnPasteClick(*document_c, clipboard);
  passed &= expect(
      retry_c.applicable() && retry_c.activeDocumentMaterial->authority() ==
                                document_c->documentAuthority(),
      "retry reads current clipboard and current document rather than cached state");
  clipboard.text.reset();
  passed &= expect(
      !MaterialClipboardCodec::resolveOnPasteClick(*document_a, clipboard)
           .menuEnabled,
      "missing OS clipboard text remains a disabled non-history no-op");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testStrictGrammarAndCanonicalEncoding();
  passed &= testAvailabilityAndCopy();
  passed &= testClickTimeCrossDocumentRevalidation();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
