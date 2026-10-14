#include <mgif/scene_editor/detail/BitwiseCast.h>

#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using demo::editor::detail::bitwiseCast;

bool expect(bool condition, const char* message)
{
	if(!condition) std::cerr << "FAILED: " << message << '\n';
	return condition;
}

} // namespace

int main()
{
	static_assert(sizeof(float) == sizeof(std::uint32_t));
	static_assert(sizeof(double) == sizeof(std::uint64_t));
	static_assert(std::numeric_limits<float>::is_iec559);
	static_assert(std::numeric_limits<double>::is_iec559);

	bool passed = true;
	passed &= expect(bitwiseCast<std::int8_t>(std::uint8_t{0x80u})
		== std::numeric_limits<std::int8_t>::min(),
		"signed 8-bit minimum retains its object representation");
	passed &= expect(bitwiseCast<std::int16_t>(std::uint16_t{0x8000u})
		== std::numeric_limits<std::int16_t>::min(),
		"signed 16-bit minimum retains its object representation");
	passed &= expect(bitwiseCast<std::uint32_t>(
		bitwiseCast<float>(std::uint32_t{0xbfc00000u})) == 0xbfc00000u,
		"float decoding round-trips the exact -1.5 bit pattern");
	passed &= expect(bitwiseCast<std::uint64_t>(-0.0) == 0x8000000000000000ull,
		"negative zero remains distinct for command identity and hashing");

	constexpr std::uint64_t nanPayload = 0x7ff8000000000042ull;
	const double nan = bitwiseCast<double>(nanPayload);
	passed &= expect(nan != nan && bitwiseCast<std::uint64_t>(nan) == nanPayload,
		"NaN payload bits survive a round trip unchanged");

	if(passed) std::cout << "Portable bit representation contract passed.\n";
	return passed ? 0 : 1;
}
