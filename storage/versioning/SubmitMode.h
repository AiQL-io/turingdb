#pragma once

#include <stdint.h>

namespace db {

enum class SubmitMode : uint8_t {
    MERGE = 0,
    SQUASH,
};

}
