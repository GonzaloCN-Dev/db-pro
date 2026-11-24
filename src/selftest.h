#pragma once

#include <string>
#include <vector>

#include <slint.h>

namespace dbp {

// Developer-only UI driver, active only when DBPRO_SELFTEST=<script file> is set.
// It injects pointer/key events into the Slint window itself and saves snapshots of that window,
// so UI flows can be checked without touching the real desktop. Script commands (one per line, # comments):
//   size W H | click X Y | dclick X Y | rclick X Y | type TEXT | key NAME | wait MS | shot NAME | quit
// Coordinates are logical pixels inside the window. Keys: enter esc tab backspace delete up down left right pgup pgdn.
class SelfTest {
public:
    SelfTest(slint::Window &window, std::string script_path, std::string out_dir);
    void start();

private:
    void step();
    bool run_line(const std::string &line);

    slint::Window &win_;
    std::vector<std::string> lines_;
    std::string out_dir_;
    size_t next_ = 0;
    slint::Timer timer_;
    int wait_ms_ = 0;
};

} // namespace dbp
