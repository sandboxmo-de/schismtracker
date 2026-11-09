/*
 * Generates the fixed lookup tables of the OPL emulators (player/fmopl2.c
 * and player/fmopl3.c), which used to be filled in at runtime by their
 * init_tables(). The code below is that function from each file, unchanged
 * apart from where it writes its output; the emulators are derived from
 * MAME's fmopl.c and ymf262.c.
 *
 *     cc -O2 -o gen-opl-tables scripts/gen-opl-tables.c -lm
 *     ./gen-opl-tables opl2 > include/player/fmopl2_tables.h
 *     ./gen-opl-tables opl3 > include/player/fmopl3_tables.h
 *
 * scripts/check-tables.sh checks that the committed headers match.
 */

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif

#define ENV_BITS        10
#define ENV_LEN         (1<<ENV_BITS)
#define ENV_STEP        (128.0/ENV_LEN)

#define SIN_BITS        10
#define SIN_LEN         (1<<SIN_BITS)
#define SIN_MASK        (SIN_LEN-1)

#define TL_RES_LEN      (256)   /* 8 bits addressing (real chip) */

#define OPL2_TL_TAB_LEN (12*2*TL_RES_LEN)
#define OPL3_TL_TAB_LEN (13*2*TL_RES_LEN)

static int32_t tl_tab[OPL3_TL_TAB_LEN];
static uint32_t sin_tab[SIN_LEN * 8];

/* ------------------------------------------------------------------------ */
/* fmopl2.c */

static void opl2_init_tables(void)
{
#define TL_TAB_LEN OPL2_TL_TAB_LEN
	int32_t i,x;
	int32_t n;
	double o,m;

	for (x=0; x<TL_RES_LEN; x++)
	{
		m = (1<<16) / pow(2.0, (x+1) * (ENV_STEP/4.0) / 8.0);
		m = floor(m);

		/* we never reach (1<<16) here due to the (x+1) */
		/* result fits within 16 bits at maximum */

		n = (int32_t)m; /* 16 bits here */
		n >>= 4;        /* 12 bits here */
		if (n&1)        /* round to nearest */
			n = (n>>1)+1;
		else
			n = n>>1;
						/* 11 bits here (rounded) */
		n <<= 1;        /* 12 bits here (as in real chip) */
		tl_tab[ x*2 + 0 ] = n;
		tl_tab[ x*2 + 1 ] = -tl_tab[ x*2 + 0 ];

		for (i=1; i<12; i++)
		{
			tl_tab[ x*2+0 + i*2*TL_RES_LEN ] =  tl_tab[ x*2+0 ]>>i;
			tl_tab[ x*2+1 + i*2*TL_RES_LEN ] = -tl_tab[ x*2+0 + i*2*TL_RES_LEN ];
		}
	}

	for (i=0; i<SIN_LEN; i++)
	{
		/* non-standard sinus */
		m = sin( ((i*2)+1) * M_PI / SIN_LEN ); /* checked against the real chip */

		/* we never reach zero here due to ((i*2)+1) */

		if (m>0.0)
			o = 8*log(1.0/m)/log(2.0);  /* convert to 'decibels' */
		else
			o = 8*log(-1.0/m)/log(2.0); /* convert to 'decibels' */

		o = o / (ENV_STEP/4);

		n = (int32_t)(2.0*o);
		if (n&1)                        /* round to nearest */
			n = (n>>1)+1;
		else
			n = n>>1;

		sin_tab[ i ] = n*2 + (m>=0.0? 0: 1 );
	}

	for (i=0; i<SIN_LEN; i++)
	{
		/* waveform 1:  __      __     */
		/*             /  \____/  \____*/
		/* output only first half of the sinus waveform (positive one) */

		if (i & (1<<(SIN_BITS-1)) )
			sin_tab[1*SIN_LEN+i] = TL_TAB_LEN;
		else
			sin_tab[1*SIN_LEN+i] = sin_tab[i];

		/* waveform 2:  __  __  __  __ */
		/*             /  \/  \/  \/  \*/
		/* abs(sin) */

		sin_tab[2*SIN_LEN+i] = sin_tab[i & (SIN_MASK>>1) ];

		/* waveform 3:  _   _   _   _  */
		/*             / |_/ |_/ |_/ |_*/
		/* abs(output only first quarter of the sinus waveform) */

		if (i & (1<<(SIN_BITS-2)) )
			sin_tab[3*SIN_LEN+i] = TL_TAB_LEN;
		else
			sin_tab[3*SIN_LEN+i] = sin_tab[i & (SIN_MASK>>2)];
	}
#undef TL_TAB_LEN
}

/* ------------------------------------------------------------------------ */
/* fmopl3.c */

static void opl3_init_tables(void)
{
#define TL_TAB_LEN OPL3_TL_TAB_LEN
	int32_t i,x;
	int32_t n;
	double o,m;

	for (x=0; x<TL_RES_LEN; x++)
	{
		m = (1<<16) / pow(2.0, (x+1) * (ENV_STEP/4.0) / 8.0);
		m = floor(m);

		/* we never reach (1<<16) here due to the (x+1) */
		/* result fits within 16 bits at maximum */

		n = (int32_t)m; /* 16 bits here */
		n >>= 4;        /* 12 bits here */
		if (n&1)        /* round to nearest */
			n = (n>>1)+1;
		else
			n = n>>1;
						/* 11 bits here (rounded) */
		n <<= 1;        /* 12 bits here (as in real chip) */
		tl_tab[ x*2 + 0 ] = n;
		tl_tab[ x*2 + 1 ] = ~tl_tab[ x*2 + 0 ]; /* this *is* different from OPL2 (verified on real YMF262) */

		for (i=1; i<13; i++)
		{
			tl_tab[ x*2+0 + i*2*TL_RES_LEN ] =  tl_tab[ x*2+0 ]>>i;
			tl_tab[ x*2+1 + i*2*TL_RES_LEN ] = ~tl_tab[ x*2+0 + i*2*TL_RES_LEN ];  /* this *is* different from OPL2 (verified on real YMF262) */
		}
	}

	for (i=0; i<SIN_LEN; i++)
	{
		/* non-standard sinus */
		m = sin( ((i*2)+1) * M_PI / SIN_LEN ); /* checked against the real chip */

		/* we never reach zero here due to ((i*2)+1) */

		if (m>0.0)
			o = 8*log(1.0/m)/log(2.0);  /* convert to 'decibels' */
		else
			o = 8*log(-1.0/m)/log(2.0); /* convert to 'decibels' */

		o = o / (ENV_STEP/4);

		n = (int32_t)(2.0*o);
		if (n&1)                        /* round to nearest */
			n = (n>>1)+1;
		else
			n = n>>1;

		sin_tab[ i ] = n*2 + (m>=0.0? 0: 1 );
	}

	for (i=0; i<SIN_LEN; i++)
	{
		/* these 'pictures' represent _two_ cycles */
		/* waveform 1:  __      __     */
		/*             /  \____/  \____*/
		/* output only first half of the sinus waveform (positive one) */

		if (i & (1<<(SIN_BITS-1)) )
			sin_tab[1*SIN_LEN+i] = TL_TAB_LEN;
		else
			sin_tab[1*SIN_LEN+i] = sin_tab[i];

		/* waveform 2:  __  __  __  __ */
		/*             /  \/  \/  \/  \*/
		/* abs(sin) */

		sin_tab[2*SIN_LEN+i] = sin_tab[i & (SIN_MASK>>1) ];

		/* waveform 3:  _   _   _   _  */
		/*             / |_/ |_/ |_/ |_*/
		/* abs(output only first quarter of the sinus waveform) */

		if (i & (1<<(SIN_BITS-2)) )
			sin_tab[3*SIN_LEN+i] = TL_TAB_LEN;
		else
			sin_tab[3*SIN_LEN+i] = sin_tab[i & (SIN_MASK>>2)];

		/* waveform 4:                 */
		/*             /\  ____/\  ____*/
		/*               \/      \/    */
		/* output whole sinus waveform in half the cycle(step=2) and output 0 on the other half of cycle */

		if (i & (1<<(SIN_BITS-1)) )
			sin_tab[4*SIN_LEN+i] = TL_TAB_LEN;
		else
			sin_tab[4*SIN_LEN+i] = sin_tab[i*2];

		/* waveform 5:                 */
		/*             /\/\____/\/\____*/
		/*                             */
		/* output abs(whole sinus) waveform in half the cycle(step=2) and output 0 on the other half of cycle */

		if (i & (1<<(SIN_BITS-1)) )
			sin_tab[5*SIN_LEN+i] = TL_TAB_LEN;
		else
			sin_tab[5*SIN_LEN+i] = sin_tab[(i*2) & (SIN_MASK>>1) ];

		/* waveform 6: ____    ____    */
		/*                             */
		/*                 ____    ____*/
		/* output maximum in half the cycle and output minimum on the other half of cycle */

		if (i & (1<<(SIN_BITS-1)) )
			sin_tab[6*SIN_LEN+i] = 1;   /* negative */
		else
			sin_tab[6*SIN_LEN+i] = 0;   /* positive */

		/* waveform 7:                 */
		/*             |\____  |\____  */
		/*                   \|      \|*/
		/* output sawtooth waveform    */

		if (i & (1<<(SIN_BITS-1)) )
			x = ((SIN_LEN-1)-i)*16 + 1; /* negative: from 8177 to 1 */
		else
			x = i*16;   /*positive: from 0 to 8176 */

		if (x > TL_TAB_LEN)
			x = TL_TAB_LEN; /* clip to the allowed range */

		sin_tab[7*SIN_LEN+i] = x;
	}
#undef TL_TAB_LEN
}

/* ------------------------------------------------------------------------ */

static void print_table(const char *decl, const char *name, const char *len, int is_signed,
	const void *tab, int n)
{
	int i;

	printf("static const %s %s[%s] = {\n", decl, name, len);
	for (i = 0; i < n; i++) {
		if (!(i % 16))
			printf("\t");
		if (is_signed)
			printf("%" PRId32 ",", ((const int32_t *)tab)[i]);
		else
			printf("%" PRIu32 ",", ((const uint32_t *)tab)[i]);
		printf((i % 16 == 15 || i == n - 1) ? "\n" : " ");
	}
	printf("};\n");
}

int main(int argc, char **argv)
{
	int opl3;

	if (argc != 2 || (strcmp(argv[1], "opl2") && strcmp(argv[1], "opl3"))) {
		fprintf(stderr, "usage: %s opl2|opl3\n", argv[0]);
		return 2;
	}
	opl3 = !strcmp(argv[1], "opl3");

	if (opl3)
		opl3_init_tables();
	else
		opl2_init_tables();

	printf("/* Generated by scripts/gen-opl-tables.c %s; don't edit. */\n\n", argv[1]);
	print_table("int32_t", "tl_tab", "TL_TAB_LEN", 1, tl_tab,
		opl3 ? OPL3_TL_TAB_LEN : OPL2_TL_TAB_LEN);
	printf("\n");
	print_table("uint32_t", "sin_tab", opl3 ? "SIN_LEN * 8" : "SIN_LEN * 4", 0, sin_tab,
		SIN_LEN * (opl3 ? 8 : 4));

	return 0;
}
