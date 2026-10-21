#pragma once

#include <stdint.h>

#include "EnumToString.h"

namespace db {

enum class UnicodeNormalForm : uint8_t {
    NFC = 0,
    NFD,
    NFKC,
    NFKD,

    _SIZE,
};

using UnicodeNormalFormName = EnumToString<UnicodeNormalForm>::Create<
    EnumStringPair<UnicodeNormalForm::NFC, "NFC">,
    EnumStringPair<UnicodeNormalForm::NFD, "NFD">,
    EnumStringPair<UnicodeNormalForm::NFKC, "NFKC">,
    EnumStringPair<UnicodeNormalForm::NFKD, "NFKD">>;

}
