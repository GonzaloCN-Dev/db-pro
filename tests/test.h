#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct TestCase {
    const char *name;
    std::function<void()> fn;
};
std::vector<TestCase> &registry();
extern int g_failures;

struct Reg {
    Reg(const char *n, std::function<void()> f) { registry().push_back({ n, std::move(f) }); }
};

#define TEST(name) \
    static void name(); \
    static Reg reg_##name(#name, name); \
    static void name()

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        auto _a = (a); \
        auto _b = (b); \
        if (!(_a == _b)) { \
            std::printf("    FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures; \
        } \
    } while (0)
