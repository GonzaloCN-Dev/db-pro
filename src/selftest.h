#pragma once

#include <string>
#include <vector>
#include <functional>
#include <string>

#include <slint.h>

namespace dbp {

// Developer-only UI driver, active only when DBPRO_SELFTEST=<script file> is set.
// It injects pointer/key events into the Slint window itself and saves snapshots of that window,
// so UI flows can be checked without touching the real desktop. Script commands (one per line, # comments):
//   size W H | click X Y | dclick X Y | rclick X Y | type TEXT | key NAME | wait MS
//   assert CONDITION | wait-for CONDITION MS | shot NAME | quit
// Coordinates are logical pixels inside the window. Keys: enter esc tab backspace delete up down left right pgup pgdn.
class SelfTest {
public:
    SelfTest(slint::Window &window, std::string script_path, std::string out_dir,
             std::function<bool(const std::string &)> probe = {});
    void start();

private:
    void step();
    bool run_line(const std::string &line);
    void finish();
    bool probe(const std::string &condition) const;

    slint::Window &win_;
    std::vector<std::string> lines_;
    std::string out_dir_;
    std::function<bool(const std::string &)> probe_;
    size_t next_ = 0;
    slint::Timer timer_;
    int wait_ms_ = 0;
    int wait_for_ms_ = 0;
    std::string wait_for_;
    bool failed_ = false;
    bool finished_ = false;
    std::vector<std::string> failures_;
};

} // namespace dbp
