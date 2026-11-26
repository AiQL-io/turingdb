#pragma once

#include <stddef.h>

#include <string>

namespace turing::test {

size_t getAllocatedBytes(const std::string& path);

// Whether the filesystem holding the directory keeps the unwritten ranges of a file as holes
bool directoryKeepsHoles(const std::string& directory);

}
