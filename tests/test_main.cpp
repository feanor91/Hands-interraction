#include "TestFramework.h"

int main() {
    int run = 0;
    for (const auto& t : testfw::Registry()) {
        const int before = testfw::FailureCount();
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "EXCEPTION dans " << t.name << " : " << e.what() << "\n";
            ++testfw::FailureCount();
        }
        ++run;
        std::cout << (testfw::FailureCount() == before ? "[ OK ] " : "[FAIL] ") << t.name << "\n";
    }
    std::cout << run << " test(s), " << testfw::FailureCount() << " echec(s)\n";
    return testfw::FailureCount() == 0 ? 0 : 1;
}
