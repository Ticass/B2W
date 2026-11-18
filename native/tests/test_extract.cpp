// Extraction caches (extract/cache.cpp) with a scripted unlinker.
#include "extract/cache.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace w2b::extract;
namespace fs = std::filesystem;

namespace
{
    fs::path Fresh(const char* name)
    {
        const auto root = fs::temp_directory_path() / "w2b_extract_tests" / name;
        fs::remove_all(root);
        fs::create_directories(root);
        return root;
    }

    int Runs(const fs::path& zone)
    {
        int runs = 0;
        std::ifstream(zone.wstring() + L".runs") >> runs;
        return runs;
    }
} // namespace

TEST_CASE("an extracted zone is published once and reused from its receipt")
{
    const auto root = Fresh("reuse");
    const auto zone = root / "game" / "zone" / "all" / "common_zm.ff";
    fs::create_directories(zone.parent_path());
    std::ofstream(zone) << "zone bytes";
    ZoneRequest request{zone, FAKE_UNLINKER, root / "cache" / "zones" / "z", SearchPaths({zone.parent_path()}), std::string("material,image"), "IWI"};
    const auto first = ExtractZone(request);
    CHECK(Runs(zone) == 2); // the dump and its asset listing
    CHECK(first.data["files"] == w2b::py::Json::array({"images/bar.iwi", "materials/mc/foo.json"}));
    CHECK(first.data["index"] == w2b::py::Json::parse(R"({"material": ["mc/foo"], "image": ["bar"]})"));
    CHECK(first.data["loaded_index"]["image"] == w2b::py::Json::array({"bar", "referenced"}));
    CHECK(fs::is_regular_file(first.folder / "extraction.json"));
    // the cache key is the receipt's name: the same inputs find the same folder
    CHECK(fs::is_regular_file(request.root / (Key(first.data["inputs"]) + ".json")));

    const auto second = ExtractZone(request);
    CHECK(Runs(zone) == 2);
    CHECK(second.folder == first.folder);

    request.refresh = true;
    const auto refreshed = ExtractZone(request);
    CHECK(Runs(zone) == 4);
    CHECK(refreshed.folder != first.folder); // a new generation; readers of the old keep theirs
}

TEST_CASE("a zone extracted again for more kinds extracts only the new ones")
{
    const auto root = Fresh("incremental");
    const auto zone = root / "game" / "zone" / "english" / "ber1.ff";
    fs::create_directories(zone.parent_path());
    std::ofstream(zone) << "zone bytes";
    ZoneRequest request{zone, FAKE_UNLINKER, root / "dependencies" / "z", SearchPaths({zone.parent_path()}), std::string("material"), "DDS"};
    const auto first = ExtractZone(request);
    CHECK(Runs(zone) == 2); // the dump and its asset listing
    CHECK(first.data["files"] == w2b::py::Json::array({"materials/mc/foo.json"}));

    request.assets = "image,material";
    request.base = first.folder;
    const auto second = ExtractZone(request);
    CHECK(Runs(zone) == 3); // only the image kind is dumped; the listing is reused
    CHECK(second.folder != first.folder);
    CHECK(second.data["files"] == w2b::py::Json::array({"images/bar.iwi", "materials/mc/foo.json"}));
    CHECK(w2b::py::SortKeys(second.data["index"]) == w2b::py::SortKeys(first.data["index"]));
    CHECK(second.data["inputs"]["assets"] == "image,material");
    std::ifstream kinds(zone.wstring() + L".kinds");
    std::string firstKinds, secondKinds;
    kinds >> firstKinds >> secondKinds;
    CHECK(firstKinds == "material");
    CHECK(secondKinds == "image");
    CHECK(fs::is_regular_file(first.folder / "materials/mc/foo.json")); // the earlier generation stays readable
}

TEST_CASE("cache keys hash sorted JSON like all2raw.key")
{
    // all2raw.key({"b": 1, "a": [1, "x"]}) in Python
    CHECK(Key(w2b::py::Json::parse(R"({"b": 1, "a": [1, "x"]})")) == Key(w2b::py::Json::parse(R"({"a": [1, "x"], "b": 1})")));
    CHECK(Key(w2b::py::Json("C:\\maps")).size() == 24);
}

TEST_CASE("asset listings keep definitions, references only on request")
{
    const auto text = "material,mc/foo\nxmodel , body \nimage,,shared\nbad line\n_,x\n";
    const auto own = ListingJson(text);
    CHECK(own == w2b::py::Json::parse(R"({"material": ["mc/foo"], "xmodel": ["body"]})"));
    const auto all = ListingJson(text, true);
    CHECK(all["image"] == w2b::py::Json::array({"shared"}));
}
