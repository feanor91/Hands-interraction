// TestFramework.h — mini cadre de tests maison (aucune dépendance).
//
// Usage :
//     TEST(MonTest) { CHECK(1 + 1 == 2); CHECK_EQ(a, b); }
// NOTE C++ : la macro TEST déclare une fonction et, grâce à un objet global
// construit AVANT main(), l'enregistre dans une liste (équivalent d'un
// attribut [Fact] découvert par réflexion en C#, mais fait « à la main »).
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& FailureCount() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

template <class A, class B>
void CheckEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!(a == b)) {
        std::ostringstream ss;
        ss << file << ":" << line << "  CHECK_EQ(" << ea << ", " << eb << ") a echoue : " << a << " != " << b;
        std::cerr << ss.str() << "\n";
        ++FailureCount();
    }
}

}  // namespace testfw

#define TEST(name)                                              \
    static void test_##name();                                  \
    static testfw::Registrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << "  CHECK(" #cond ") a echoue\n"; \
            ++testfw::FailureCount();                                                   \
        }                                                                               \
    } while (0)

#define CHECK_EQ(a, b) testfw::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)
