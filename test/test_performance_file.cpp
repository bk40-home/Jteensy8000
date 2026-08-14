// =============================================================================
// test_performance_file.cpp — the JTF1 dual-layer container
// =============================================================================
#include "doctest.h"
#include "core/Performance.h"
#include "core/Patch.h"
#include "core/ParameterStore.h"
#include "core/dsp/Curves.h"
#include <vector>
#include <string>

using namespace JT;
using namespace JT::Params;

namespace {
void drainAll(ParameterStore& s)
{
    while (s.takeNextDirty() != ParameterStore::kInvalidIndex) {}
}
} // namespace

TEST_CASE("performance round trip: both layers survive independently")
{
    ParameterStore src;
    drainAll(src);

    // Two clearly different layers.
    REQUIRE(src.setEngineering(ID::FILTER_CUTOFF, 400.0f,  Origin::Ui, 0));
    REQUIRE(src.setEngineering(ID::FILTER_CUTOFF, 6000.0f, Origin::Ui, 1));
    REQUIRE(src.setEngineering(ID::ENV_AMP_ATTACK, 5.0f,   Origin::Ui, 0));
    REQUIRE(src.setEngineering(ID::ENV_AMP_ATTACK, 900.0f, Origin::Ui, 1));

    Performance::Setup setup;
    setup.perfMode     = 2;      // Split
    setup.splitNote    = 55;
    setup.midiChannelA = 3;
    setup.midiChannelB = 9;
    setup.balance      = 40;
    setup.voiceSplit   = 2;

    std::vector<uint8_t> img(Performance::kMaxEncodedSize);
    const size_t len = Performance::save(src, "Split Test", 7, setup,
                                         img.data(), img.size());
    REQUIRE(len > 0);

    ParameterStore dst;
    drainAll(dst);
    const auto r = Performance::load(img.data(), len, dst);
    REQUIRE(r.status == Performance::Status::Ok);

    CHECK(dst.getEngineering(ID::FILTER_CUTOFF, 0) == doctest::Approx(400.0f));
    CHECK(dst.getEngineering(ID::FILTER_CUTOFF, 1) == doctest::Approx(6000.0f));
    CHECK(dst.getEngineering(ID::ENV_AMP_ATTACK, 0) == doctest::Approx(5.0f));
    CHECK(dst.getEngineering(ID::ENV_AMP_ATTACK, 1) == doctest::Approx(900.0f));

    // Setup survives, channels round-tripping as 1..16.
    CHECK(r.info.setup.perfMode     == 2);
    CHECK(r.info.setup.splitNote    == 55);
    CHECK(r.info.setup.midiChannelA == 3);
    CHECK(r.info.setup.midiChannelB == 9);
    CHECK(r.info.setup.balance      == 40);
    CHECK(dst.getEngineering(ID::PERF_SPLIT_NOTE) == doctest::Approx(55.0f));
    CHECK(dst.getEngineering(ID::PERF_BALANCE)    == doctest::Approx(40.0f));
}

TEST_CASE("a layer can be pulled out as a plain patch, byte for byte")
{
    // The reason the container wraps JTP1 rather than inventing a value format:
    // extracting a layer is a pointer, not a re-encode.
    ParameterStore src;
    drainAll(src);
    REQUIRE(src.setEngineering(ID::FILTER_CUTOFF, 1234.0f, Origin::Ui, 1));

    Performance::Setup setup;
    std::vector<uint8_t> img(Performance::kMaxEncodedSize);
    const size_t len = Performance::save(src, "Extract", 0, setup,
                                         img.data(), img.size());
    REQUIRE(len > 0);

    const uint8_t* imgB = nullptr;
    size_t lenB = 0;
    REQUIRE(Performance::layerImage(img.data(), len, 1, imgB, lenB));

    // Load that borrowed image into layer A of a fresh store: this is exactly
    // "load patch into layer", with no special code path.
    ParameterStore dst;
    drainAll(dst);
    const auto pr = Patch::load(imgB, lenB, dst, Origin::PatchLoad, nullptr, 0, 0);
    CHECK(pr.status == Patch::Status::Ok);
    CHECK(dst.getEngineering(ID::FILTER_CUTOFF, 0) == doctest::Approx(1234.0f));
}

TEST_CASE("loading a patch into layer B leaves layer A alone")
{
    ParameterStore src;
    drainAll(src);
    REQUIRE(src.setEngineering(ID::FILTER_CUTOFF, 800.0f, Origin::Ui, 0));

    uint8_t patch[Patch::kMaxEncodedSize];
    const size_t plen = Patch::save(src, "One Layer", 0, patch, sizeof patch, 0);
    REQUIRE(plen > 0);

    ParameterStore dst;
    drainAll(dst);
    REQUIRE(dst.setEngineering(ID::FILTER_CUTOFF, 3000.0f, Origin::Ui, 0));

    const auto r = Patch::load(patch, plen, dst, Origin::PatchLoad, nullptr, 0, 1);
    REQUIRE(r.status == Patch::Status::Ok);

    CHECK(dst.getEngineering(ID::FILTER_CUTOFF, 1) == doctest::Approx(800.0f));
    CHECK(dst.getEngineering(ID::FILTER_CUTOFF, 0) == doctest::Approx(3000.0f));
}

TEST_CASE("a corrupt performance changes nothing at all")
{
    ParameterStore src;
    drainAll(src);
    REQUIRE(src.setEngineering(ID::FILTER_CUTOFF, 500.0f, Origin::Ui, 0));

    Performance::Setup setup;
    std::vector<uint8_t> img(Performance::kMaxEncodedSize);
    const size_t len = Performance::save(src, "Corrupt", 0, setup,
                                         img.data(), img.size());
    REQUIRE(len > 0);
    img[Performance::kHeaderSize + 12] ^= 0xFF;      // flip a bit inside layer A

    ParameterStore dst;
    drainAll(dst);
    const float before = dst.getEngineering(ID::FILTER_CUTOFF, 0);

    const auto r = Performance::load(img.data(), len, dst);
    CHECK(r.status == Performance::Status::BadCrc);
    CHECK(dst.getEngineering(ID::FILTER_CUTOFF, 0) == doctest::Approx(before));
    CHECK(dst.takeNextDirty() == ParameterStore::kInvalidIndex);   // nothing written
}

TEST_CASE("peekInfo reads the browser fields without a store")
{
    ParameterStore src;
    drainAll(src);
    Performance::Setup setup;
    setup.perfMode = 1;
    setup.balance  = 90;

    std::vector<uint8_t> img(Performance::kMaxEncodedSize);
    const size_t len = Performance::save(src, "Browse Me", 4, setup,
                                         img.data(), img.size());
    REQUIRE(len > 0);

    Performance::Info info{};
    REQUIRE(Performance::peekInfo(img.data(), len, info));
    CHECK(std::string(info.name).substr(0, 9) == "Browse Me");
    CHECK(info.category == 4);
    CHECK(info.setup.perfMode == 1);
    CHECK(info.setup.balance  == 90);
}

TEST_CASE("bad magic and truncation are rejected, not guessed at")
{
    ParameterStore src, dst;
    drainAll(src); drainAll(dst);
    Performance::Setup setup;
    std::vector<uint8_t> img(Performance::kMaxEncodedSize);
    const size_t len = Performance::save(src, "X", 0, setup, img.data(), img.size());
    REQUIRE(len > 0);

    img[0] = 'X';
    CHECK(Performance::load(img.data(), len, dst).status == Performance::Status::BadMagic);
    img[0] = 'J';
    CHECK(Performance::load(img.data(), 8, dst).status == Performance::Status::TooShort);
}
