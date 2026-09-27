file(READ "${TIV_EMBED_INPUT}" bytes HEX)

string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n    " bytes "${bytes}")

file(WRITE "${TIV_EMBED_OUTPUT}"
"// Generated from ${TIV_EMBED_INPUT}. Do not edit.

#include <cstddef>

namespace Embedded {

extern const unsigned char ${TIV_EMBED_SYMBOL}[];
extern const std::size_t ${TIV_EMBED_SYMBOL}Size;

const unsigned char ${TIV_EMBED_SYMBOL}[] = {
    ${bytes}
};

const std::size_t ${TIV_EMBED_SYMBOL}Size = sizeof(${TIV_EMBED_SYMBOL});

}
")
