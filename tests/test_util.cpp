#include "TestFramework.h"
#include "util/StringUtil.h"

using namespace hands::util;

TEST(Util_Trim_et_SplitList) {
    CHECK_EQ(std::string(Trim("  a b \t\r\n")), std::string("a b"));
    CHECK_EQ(std::string(Trim("   ")), std::string(""));
    auto v = SplitList(" a, b ,,c ,");
    CHECK_EQ(v.size(), size_t(3));
    CHECK_EQ(v[0], std::string("a"));
    CHECK_EQ(v[2], std::string("c"));
    CHECK(SplitList("").empty());
}

TEST(Util_FileNameOf_windows_et_posix) {
    CHECK_EQ(FileNameOf("C:\\Games\\MSFS\\FlightSimulator2024.exe"), std::string("FlightSimulator2024.exe"));
    CHECK_EQ(FileNameOf("/usr/bin/foo"), std::string("foo"));
    CHECK_EQ(FileNameOf("Foo.exe"), std::string("Foo.exe"));
}

TEST(Util_EqualsIgnoreCase) {
    CHECK(EqualsIgnoreCase("FlightSimulator2024.EXE", "flightsimulator2024.exe"));
    CHECK(!EqualsIgnoreCase("a", "b"));
}
