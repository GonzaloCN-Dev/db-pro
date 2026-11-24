#include "selftest.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dbp {

namespace {

uint32_t crc32(const uint8_t *d, size_t n, uint32_t c = 0)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    c = ~c;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

void be32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}

void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
    be32(out, (uint32_t)data.size());
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc32(body.data(), body.size()));
}

// Minimal PNG writer (uncompressed deflate blocks): enough for diagnostics, no extra dependency.
bool write_png(const std::string &path, const slint::SharedPixelBuffer<slint::Rgba8Pixel> &img)
{
    uint32_t w = img.width(), h = img.height();
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    const slint::Rgba8Pixel *px = img.begin();
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < w; ++x) {
            const auto &p = px[(size_t)y * w + x];
            raw.push_back(p.r); raw.push_back(p.g); raw.push_back(p.b); raw.push_back(p.a);
        }
    }
    std::vector<uint8_t> z { 0x78, 0x01 };
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF)); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF)); z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);

    std::vector<uint8_t> png { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr.insert(ihdr.end(), { 8, 6, 0, 0, 0 });
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char *)png.data(), (std::streamsize)png.size());
    return (bool)f;
}

// Slint maps special keys to these code points.
std::string key_text(const std::string &name)
{
    if (name == "enter") return "\n";
    if (name == "esc") return "\x1b";
    if (name == "tab") return "\t";
    if (name == "backspace") return "\b";
    if (name == "delete") return "\x7f";
    if (name == "up") return "\xEF\x9C\x80";    // U+F700
    if (name == "down") return "\xEF\x9C\x81";  // U+F701
    if (name == "left") return "\xEF\x9C\x82";  // U+F702
    if (name == "right") return "\xEF\x9C\x83"; // U+F703
    if (name == "pgup") return "\xEF\x9C\xA9";  // U+F72C
    if (name == "pgdn") return "\xEF\x9C\xAA";  // U+F72D
    return {};
}

} // namespace

SelfTest::SelfTest(slint::Window &window, std::string script_path, std::string out_dir) : win_(window), out_dir_(std::move(out_dir))
{
    std::ifstream f(script_path);
    std::string l;
    while (std::getline(f, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty() && l[0] != '#') lines_.push_back(l);
    }
    std::filesystem::create_directories(out_dir_);
}

void SelfTest::start()
{
    timer_.start(slint::TimerMode::Repeated, std::chrono::milliseconds(50), [this] { step(); });
}

void SelfTest::step()
{
    if (wait_ms_ > 0) { wait_ms_ -= 50; return; }
    if (next_ >= lines_.size()) { timer_.stop(); return; }
    run_line(lines_[next_++]);
}

bool SelfTest::run_line(const std::string &line)
{
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    auto click = [&](slint::PointerEventButton btn, int count) {
        float x = 0, y = 0;
        in >> x >> y;
        slint::LogicalPosition p({ x, y });
        win_.dispatch_pointer_move_event(p);
        for (int i = 0; i < count; ++i) {
            win_.dispatch_pointer_press_event(p, btn);
            win_.dispatch_pointer_release_event(p, btn);
        }
    };
    if (cmd == "size") {
        float w = 0, h = 0;
        in >> w >> h;
        win_.set_size(slint::LogicalSize({ w, h }));
        wait_ms_ = 300;
    } else if (cmd == "click") {
        click(slint::PointerEventButton::Left, 1);
        wait_ms_ = 100;
    } else if (cmd == "dclick") {
        click(slint::PointerEventButton::Left, 2);
        wait_ms_ = 100;
    } else if (cmd == "rclick") {
        click(slint::PointerEventButton::Right, 1);
        wait_ms_ = 100;
    } else if (cmd == "type") {
        std::string text;
        std::getline(in, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        for (size_t i = 0; i < text.size();) {
            size_t n = 1;
            unsigned char c = (unsigned char)text[i];
            if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
            std::string ch = text.substr(i, n);
            win_.dispatch_key_press_event(slint::SharedString(ch));
            win_.dispatch_key_release_event(slint::SharedString(ch));
            i += n;
        }
        wait_ms_ = 100;
    } else if (cmd == "key") {
        std::string name;
        in >> name;
        std::string t = key_text(name);
        if (t.empty()) { std::fprintf(stderr, "selftest: unknown key %s\n", name.c_str()); return false; }
        win_.dispatch_key_press_event(slint::SharedString(t));
        win_.dispatch_key_release_event(slint::SharedString(t));
        wait_ms_ = 100;
    } else if (cmd == "wait") {
        in >> wait_ms_;
    } else if (cmd == "shot") {
        std::string name;
        in >> name;
        auto img = win_.take_snapshot();
        if (img) write_png(out_dir_ + "\\" + name + ".png", *img);
        else std::fprintf(stderr, "selftest: snapshot unavailable\n");
    } else if (cmd == "quit") {
        slint::quit_event_loop();
    } else {
        std::fprintf(stderr, "selftest: unknown command %s\n", cmd.c_str());
        return false;
    }
    return true;
}

} // namespace dbp
