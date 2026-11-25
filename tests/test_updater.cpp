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

static std::string write_tmp(const char *name, const char *data, size_t n)
{
    FILE *f = std::fopen(name, "wb");
    if (f) {
        std::fwrite(data, 1, n, f);
        std::fclose(f);
    }
    return name;
}

static bool exists(const std::string &p)
{
    FILE *f = std::fopen(p.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

TEST(update_verify_accepts_matching_hash_and_keeps_file)
{
    std::string p = write_tmp("updater_ok.bin", "abc", 3);
    CHECK(verify_file(p, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(verify_file(p, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    CHECK(exists(p));
    std::remove(p.c_str());
}

TEST(update_verify_rejects_and_deletes_tampered_file)
{
    std::string p = write_tmp("updater_bad.bin", "abd", 3);
    CHECK(!verify_file(p, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(!exists(p));
}

TEST(update_verify_rejects_malformed_or_missing_expectation)
{
    std::string p = write_tmp("updater_mal.bin", "abc", 3);
    CHECK(!verify_file(p, ""));
    CHECK(!exists(p));
    p = write_tmp("updater_mal2.bin", "abc", 3);
    CHECK(!verify_file(p, "not-a-hash"));
    CHECK(!exists(p));
    CHECK(!verify_file("Z:\\does\\not\\exist.bin", std::string(64, 'a')));
}

TEST(update_download_refuses_foreign_and_plain_http_sources)
{
    std::string p = "updater_foreign.exe";
    CHECK(!download("https://evil.example/DbPro-Setup-9.9.9.exe", p, nullptr).empty());
    CHECK(!download("http://github.com/GonzaloCN-Dev/db-pro/releases/download/v9/x.exe", p, nullptr).empty());
    CHECK(!download("https://github.com/GonzaloCN-Dev/other/releases/download/v9/x.exe", p, nullptr).empty());
    CHECK(!exists(p));
    CHECK(!exists(p + ".part"));
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