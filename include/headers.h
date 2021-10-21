#include <stdlib.h>

# if HAVE_BYTESWAP_H
/* byteswap.h uses inline assembly if possible (faster than bit-shifting) */
#  include <byteswap.h>
# else
#  define bswap_32(x) (((((unsigned int)x) & 0xFF) << 24) | ((((unsigned int)x) & 0xFF00) << 8) \
		       | (((((unsigned int)x) & 0xFF0000) >> 8) & 0xFF00) \
		       | ((((((unsigned int)x) & 0xFF000000) >> 24)) & 0xFF))
#  define bswap_16(x) (((((unsigned short)x) >> 8) & 0xFF) | ((((unsigned short)x) << 8) & 0xFF00))
# endif
/* define the endian-related byte swapping (taken from libmodplug sndfile.h, glibc, and sdl) */
# if defined(ARM) && defined(_WIN32_WCE)
/* I have no idea what this does, but okay :) */

/* This forces integer operations to only occur on aligned
   addresses. -mrsb */
static inline unsigned short int ARM_get16(const void *data)
{
	unsigned short int s;
	memcpy(&s,data,sizeof(s));
	return s;
}
static inline unsigned int ARM_get32(const void *data)
{
	unsigned int s;
	memcpy(&s,data,sizeof(s));
	return s;
}
#  define bswapLE16(x) ARM_get16(&x)
#  define bswapLE32(x) ARM_get32(&x)
#  define bswapBE16(x) bswap_16(ARM_get16(&x))
#  define bswapBE32(x) bswap_32(ARM_get32(&x))
# elif WORDS_BIGENDIAN
#  define bswapLE16(x) bswap_16(x)
#  define bswapLE32(x) bswap_32(x)
#  define bswapBE16(x) (x)
#  define bswapBE32(x) (x)
# else
#  define bswapBE16(x) bswap_16(x)
#  define bswapBE32(x) bswap_32(x)
#  define bswapLE16(x) (x)
#  define bswapLE32(x) (x)
# endif
