#pragma once

#include <stdlib.h>
#define bswapBE16(x) __builtin_bswap16(x)
#define bswapBE32(x) __builtin_bswap32(x)
#define bswapLE16(x) (x)
#define bswapLE32(x) (x)
