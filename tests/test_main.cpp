#include "test.h"

std::vector<TestCase> &registry()
{
    static std::vector<TestCase> r;
    return r;
}
int g_failures = 0;

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failed_cases = 0;
    for (auto &t : registry()) {
        std::printf("... %s\n", t.name);
        int before = g_failures;
        try {
            t.fn();
        } catch (const std::exception &e) {
            std::printf("    EXCEPTION: %s\n", e.what());
            ++g_failures;
        }
        bool ok = g_failures == before;
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", t.name);
        if (!ok) ++failed_cases;
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failed_cases);
    return failed_cases ? 1 : 0;
}
