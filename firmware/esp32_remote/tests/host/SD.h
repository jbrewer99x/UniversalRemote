#pragma once
inline bool testFileExists = true;
struct TestSD { bool exists(const char*) { return testFileExists; } };
inline TestSD SD;
