#include "test.h"
#include "updater.h"

using namespace dbp::updater;

TEST(update_version_comparison)
{
    CHECK(is_newer("0.2.0", "0.1.0"));
    CHECK(is_newer("v0.10.0", "0.9.2"));
    CHECK(is_newer("1.0", "0.9.9"));
    CHECK(!is_newer("0.1.0", "0.1.0"));
    CHECK(!is_newer("0.1.0", "0.2.0"));
    CHECK(!is_newer("0.1", "0.1.0"));
    CHECK(!is_newer("", "0.1.0"));
    CHECK(!is_newer("garbage", "0.1.0"));
}

TEST(update_release_needs_a_checksum_and_a_trusted_url)
{
    const std::string base = "https://github.com/GonzaloCN-Dev/db-pro/releases/download/v0.2.0/";
    const std::string hex(64, 'a');
    auto body = [&](const std::string &extra, const std::string &url, bool draft = false) {
        return std::string("{\"tag_name\":\"v0.2.0\",\"body\":\"notes\",\"draft\":") + (draft ? "true" : "false") + ",\"prerelease\":false,\"assets\":[{\"name\":\"DbPro-Setup-0.2.0.exe\",\"size\":10,\"browser_download_url\":\"" + url + "\"" + extra + "}]}";
    };
    auto none = [](const std::string &) { return std::string(); };

    Release ok = parse_release(body(",\"digest\":\"sha256:" + hex + "\"", base + "DbPro-Setup-0.2.0.exe"), none);
    CHECK(ok.ok);
    CHECK(ok.version == "0.2.0");
    CHECK(ok.sha256 == hex);

    CHECK(!parse_release(body("", base + "DbPro-Setup-0.2.0.exe"), none).ok);                                                  // no checksum
    CHECK(!parse_release(body(",\"digest\":\"sha256:" + hex + "\"", "https://evil.example/DbPro-Setup-0.2.0.exe"), none).ok); // foreign host
    CHECK(!parse_release(body(",\"digest\":\"sha256:" + hex + "\"", base + "DbPro-Setup-0.2.0.exe", true), none).ok);          // draft
    CHECK(!parse_release("not json", none).ok);

    Release viafile = parse_release("{\"tag_name\":\"v0.2.0\",\"assets\":[{\"name\":\"DbPro-Setup-0.2.0.exe\",\"browser_download_url\":\"" + base + "DbPro-Setup-0.2.0.exe\"},"
                                    "{\"name\":\"DbPro-Setup-0.2.0.exe.sha256\",\"browser_download_url\":\"" + base + "DbPro-Setup-0.2.0.exe.sha256\"}]}",
                                    [&](const std::string &) { return hex + "  DbPro-Setup-0.2.0.exe\n"; });
    CHECK(viafile.ok);
    CHECK(viafile.sha256 == hex);
}

TEST(update_sha256_of_a_file)
{
    CHECK(sha256_file("Z:\\does\\not\\exist.bin").empty());
    std::string path = "updater_sha_test.bin";
    FILE *f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (!f) return;
    std::fwrite("abc", 1, 3, f);
    std::fclose(f);
    CHECK(sha256_file(path) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::remove(path.c_str());
}