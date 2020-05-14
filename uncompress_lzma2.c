/*
 * LZMA2 simplified decompressor
 *
 * Copyright 2020 TAKAI Kousuke
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * This decompressor is based on xz-embedded's xz_dec_lzma2.c,
 * but specialized for single-call (memory-to-memory) decompress.
 *
 * Interface is analogous to zlib's uncompress2(), but uses standard
 * types (void *, size_t) for arguments.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef DEBUG
extern int verbosity;
extern void dbg_printf (const char *, ...)
# ifdef __GNUC__
  __attribute__((format(printf, 1, 2)))
# endif
  ;
# define DBG(...)	do { if (verbosity > 1) dbg_printf(__VA_ARGS__); } while (0)
#else
# define DBG(...)	((void) 0)
#endif

#include "uncompress_lzma2.h"

#define UNLIKELY(Cond)	__builtin_expect((Cond), 0)

/* Range coder constants */
#define RC_SHIFT_BITS	8
#define RC_TOP_BITS	24
#define RC_TOP_VALUE	(1 << RC_TOP_BITS)
#define RC_BIT_MODEL_TOTAL_BITS	11
#define RC_BIT_MODEL_TOTAL	(1 << RC_BIT_MODEL_TOTAL_BITS)
#define RC_MOVE_BITS	5
#define RC_INIT_BYTES	5

#define POS_STATES_MAX	(1 << 4)	/* Maximum number of position states */

enum lzma_state
  {
    STATE_LIT_LIT		= 0,
    STATE_MATCH_LIT_LIT,
    STATE_REP_LIT_LIT,
    STATE_SHORTREP_LIT_LIT,
    STATE_MATCH_LIT,
    STATE_REP_LIT,
    STATE_SHORTREP_LIT,
    STATE_LIT_MATCH,
    STATE_LIT_LONGREP,
    STATE_LIT_SHORTREP,
    STATE_NONLIT_MATCH,
    STATE_NONLIT_REP,
  };

#define STATES		12	/* Total number of states */
#define LIT_STATES	7	/* The previous state was a literal */

#define LITERAL_CODER_SIZE	0x300
#define LITERAL_CODERS_MAX	(1 << 4)

#define MATCH_LEN_MIN		2

#define LEN_LOW_BITS		3
#define LEN_LOW_SYMBOLS		(1 << LEN_LOW_BITS)
#define LEN_MID_BITS		3
#define LEN_MID_SYMBOLS		(1 << LEN_MID_BITS)
#define LEN_HIGH_BITS		8
#define LEN_HIGH_SYMBOLS	(1 << LEN_HIGH_BITS)
#define LEN_SYMBOLS		(LEN_LOW_SYMBOLS + LEN_MID_SYMBOLS + LEN_HIGH_SYMBOLS) /* = 272 */

#define DIST_STATES		4
#define DIST_SLOT_BITS		6
#define DIST_SLOTS		(1 << DIST_SLOT_BITS)
#define DIST_MODEL_START	4
#define DIST_MODEL_END		14
#define FULL_DISTANCE_BITS	(DIST_MODEL_END / 2)
#define FULL_DISTANCES		(1 << FULL_DISTANCE_BITS)

#define ALIGN_BITS		4
#define ALIGN_SIZE		(1 << ALIGN_BITS)
#define ALIGN_MASK		(ALIGN_SIZE - 1)

typedef uint_least16_t	probability_t;
typedef uint_fast16_t	probability_fast_t;

struct lzma_len_dec
  {
    probability_t	choice;
    probability_t	choice2;
    probability_t	low[POS_STATES_MAX][LEN_LOW_SYMBOLS];
    probability_t	mid[POS_STATES_MAX][LEN_MID_SYMBOLS];
    probability_t	high[LEN_HIGH_SYMBOLS];
  };

struct lzma_probabilities
  {
    probability_t	is_match[STATES][POS_STATES_MAX];
    probability_t	is_rep[STATES];
    probability_t	is_rep0[STATES];
    probability_t	is_rep1[STATES];
    probability_t	is_rep2[STATES];
    probability_t	is_rep0_long[STATES][POS_STATES_MAX];
    probability_t	dist_slot[DIST_STATES][DIST_SLOTS];
    probability_t	dist_special[FULL_DISTANCES - DIST_MODEL_END];
    probability_t	dist_align[ALIGN_SIZE];
    struct lzma_len_dec	match_len_dec;
    struct lzma_len_dec	rep_len_dec;
    probability_t	literal[LITERAL_CODERS_MAX][LITERAL_CODER_SIZE];
  };

/* Check against PROBS_TOTAL */
_Static_assert(sizeof(struct lzma_probabilities) ==
	       sizeof(probability_t) * (1846 + LITERAL_CODERS_MAX * LITERAL_CODER_SIZE),
	       "size of lzma_probabilities does not match PROBS_TOTAL");

struct frame
  {
    const uint8_t *	inbuf;
    size_t		incount;
    size_t		inlimit;
    size_t		outcount;
    size_t		rc_limit;
    uint_least8_t	lc, lp, pb;
    uint_least32_t	rc_code;
    uint_least32_t	rc_range;
    size_t		uncompressed, compressed;
    enum lzma_state	state;
    uint_least32_t	rep[4];
    struct lzma_probabilities probs;
  };

static void
rc_reset (struct frame *const frame)
{
  frame->rc_range = UINT32_C(0xFFFFFFFF);
}

static void
lzma_reset (struct frame *const frame)
{
  frame->state = STATE_LIT_LIT;
  frame->rep[0] = 0;
  frame->rep[1] = 0;
  frame->rep[2] = 0;
  frame->rep[3] = 0;
  
  probability_t *probs = &frame->probs.is_match[0][0];
  unsigned int i = sizeof(frame->probs) / sizeof(probability_t);
  do
    *probs++ = RC_BIT_MODEL_TOTAL  / 2;
  while (--i);

  rc_reset(frame);
}

static inline uint_fast32_t
read_unaligned_be32 (const void *const vp)
{
  const uint8_t *const p = vp;

  return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static inline _Bool
rc_normalize (struct frame *const frame)
{
  if (frame->rc_range < RC_TOP_VALUE)
    {
      frame->rc_range <<= RC_SHIFT_BITS;
      if (frame->incount >= frame->rc_limit)
	return 0;
      frame->rc_code = (frame->rc_code << RC_SHIFT_BITS) | frame->inbuf[frame->incount++];
      DBG("rc_normalize: range=%#x, code=%#x", frame->rc_range, frame->rc_code);
    }
  return 1;
}

static int
rc_bit (struct frame *const frame, probability_t *const prob)
{
  probability_fast_t p = *prob;
  uint_fast32_t bound = (frame->rc_range >> RC_BIT_MODEL_TOTAL_BITS) * p;
  int bit;

  if (frame->rc_code < bound)
    {
      frame->rc_range = bound;
      *prob = p + ((RC_BIT_MODEL_TOTAL - p) >> RC_MOVE_BITS);
      bit = 0;
    }
  else
    {
      frame->rc_range -= bound;
      frame->rc_code -= bound;
      *prob = p - (p >> RC_MOVE_BITS);
      bit = 1;
    }
  DBG("rc_bit: bound=%#x, range=%#x, code=%#x, *prob=%#x -> %d",
      bound, frame->rc_range, frame->rc_code, *prob, bit);
  return bit;
}

static unsigned int
rc_bittree (struct frame *const frame, probability_t *const probs,
	    unsigned int const limit)
{
  unsigned int symbol = 1;

  do
    {
      if (!rc_normalize(frame))
	return 0;
      symbol = (symbol << 1) | rc_bit(frame, &probs[symbol]);
    }
  while (symbol < limit);
  return symbol;
}

static unsigned int
lzma_len (struct frame *const frame, struct lzma_len_dec *const l,
	  uint32_t pos_state)
{
  probability_t *probs;
  unsigned int limit;
  unsigned int len;
  unsigned int symbol;

  if (UNLIKELY(!rc_normalize(frame)))
    return 0;
  if (!rc_bit(frame, &l->choice))
    {
      probs = l->low[pos_state];
      limit = LEN_LOW_SYMBOLS;
      len = MATCH_LEN_MIN;
    }
  else
    {
      if (UNLIKELY(!rc_normalize(frame)))
	return 0;
      if (!rc_bit(frame, &l->choice2))
	{
	  probs = l->mid[pos_state];
	  limit = LEN_MID_SYMBOLS;
	  len = MATCH_LEN_MIN + LEN_LOW_SYMBOLS;
	}
      else
	{
	  probs = l->high;
	  limit = LEN_HIGH_SYMBOLS;
	  len = MATCH_LEN_MIN + LEN_LOW_SYMBOLS + LEN_MID_SYMBOLS;
	}
    }
  symbol = rc_bittree(frame, probs, limit);
  if (UNLIKELY(!symbol))
    return 0;
  return len + symbol - limit;
}

enum uncompress_status
uncompress_lzma2 (const void *const inbuf, size_t *const insizep,
		  void *const outbuf, size_t *const outsizep)
{
  enum uncompress_status ret = UNCOMPRESS_OK;
  _Bool need_properties = 0;
  _Bool dict_reset_done = 0;
  struct frame frame;
  size_t dict_origin;

#define outbuf	((uint8_t *) outbuf)

  frame.inbuf = inbuf;
  frame.incount	= 0;
  frame.inlimit	= *insizep;
  frame.outcount = 0;

#if 0
#define RETURN(X)	do { ret = (X); goto finish; } while (0)
#else
#define RETURN(X)	do { ret = (X); DBG("%s:%u: returning %d", __func__, __LINE__, (int) ret); goto finish; } while (0)
#endif

  for (;;)
    {
      uint_fast8_t control;

      if (UNLIKELY(frame.incount >= frame.inlimit))
	RETURN(UNCOMPRESS_INLIMIT);
      control = frame.inbuf[frame.incount++];
      if (control == 0x00)	/* End marker */
	RETURN(UNCOMPRESS_OK);
      else if (control >= 0xE0 || control == 0x01)
	{
	  need_properties = 1;
	  dict_origin = frame.outcount;
	  dict_reset_done = 1;
	}
      else if (UNLIKELY(!dict_reset_done))
	RETURN(UNCOMPRESS_DATA_ERROR);

      if (control >= 0x80)	/* LZMA compressed chunk */
	{
	  uint_least32_t uncompressed, compressed;
	  uint_least32_t out_limit;
	  _Bool more_run;

	  if (control >= 0xC0)
	    need_properties = 0;
	  else if (UNLIKELY(need_properties))
	    RETURN(UNCOMPRESS_DATA_ERROR);

	  if ((frame.inlimit - frame.incount) < 4)
	    RETURN(UNCOMPRESS_INLIMIT);
	  else
	    {
	      const uint8_t *const p = &frame.inbuf[frame.incount];
	      frame.incount += 4;

	      uncompressed = (((control & 0x1F) << 16) |
			      ((p[0] << 8) | p[1])) + 1;
	      compressed = ((p[2] << 8) | p[3]) + 1;
	      DBG("uncompressed = %u, compressed = %u",
		  uncompressed, compressed);
	    }
	  if (control >= 0xC0)
	    {
	      uint_fast8_t props;
	      uint_fast8_t tmp;

	      if (frame.incount >= frame.inlimit)
		RETURN(UNCOMPRESS_INLIMIT);
	      props = frame.inbuf[frame.incount++];
	      DBG("Property %u", props);
	      if (UNLIKELY(props > (4 * 5 + 4) * 9 + 8))
		RETURN(UNCOMPRESS_DATA_ERROR);
	      for (tmp = 0; props >= 5 * 9; tmp++)
		props -= 5 * 9;
	      frame.pb = tmp;
	      for (tmp = 0; props >= 9; tmp++)
		props -= 9;
	      frame.lp = tmp;
	      frame.lc = props;
	      DBG("lc/lp/pb = %u/%u/%u", props, tmp, frame.pb);
	    }
	  if (control >= 0xA0)
	    lzma_reset(&frame);

	  frame.rc_limit = frame.incount + compressed;
	  if (frame.rc_limit > frame.inlimit)
	    frame.rc_limit = frame.inlimit;

	  if (UNLIKELY(compressed < RC_INIT_BYTES))
	    RETURN(UNCOMPRESS_DATA_ERROR);
	  if (UNLIKELY((frame.inlimit - frame.incount) < RC_INIT_BYTES))
	    RETURN(UNCOMPRESS_INLIMIT);
	  frame.rc_code = read_unaligned_be32(&frame.inbuf[frame.incount + 1]);
	  frame.incount += RC_INIT_BYTES;
	  DBG("rc_read_init: code=%u", frame.rc_code);	  

	  out_limit = *outsizep;
	  more_run = 0;
	  if (out_limit - frame.outcount > uncompressed)
	    {
	      out_limit = frame.outcount + uncompressed;
	      more_run = 1;
	    }

	  /* lzma_main */
	  for (;;)
	    {
	      unsigned int pos_state;
	      if (UNLIKELY(!rc_normalize(&frame)))
		goto rc_limit_reached;
	      if (frame.outcount >= out_limit)
		break;
	      pos_state = (frame.outcount - dict_origin) & ((1 << frame.pb) - 1);
	      if (!rc_bit(&frame, &frame.probs.is_match[frame.state][pos_state]))
		{
		  /* lzma_literal_probs */
		  uint_fast8_t prev_byte = (frame.outcount > dict_origin) ? outbuf[frame.outcount - 1] : 0;
		  probability_t *const probs = frame.probs.literal[(prev_byte >> (8 - frame.lc)) |
								   (((frame.outcount - dict_origin) & ((1 << frame.lp) - 1)) << frame.lc)];
		  unsigned int symbol;
		  /* lzma_literal */
		  if (frame.state < LIT_STATES)
		    {
		      symbol = rc_bittree(&frame, probs, 0x100);
		      if (UNLIKELY(!symbol))
			goto rc_limit_reached;
		    }
		  else if (UNLIKELY(frame.outcount - dict_origin <= frame.rep[0]))
		    RETURN(UNCOMPRESS_DATA_ERROR);
		  else
		    {
		      unsigned int match_byte = outbuf[frame.outcount - frame.rep[0] - 1];
		      unsigned int offset = 0x100;
		      
		      symbol = 1;
		      do
			{
			  unsigned int match_bit = (match_byte <<= 1) & offset;
			  unsigned int i = offset + match_bit + symbol;

			  if (UNLIKELY(!rc_normalize(&frame)))
			    goto rc_limit_reached;
			  symbol <<= 1;
			  if (rc_bit(&frame, &probs[i]))
			    {
			      symbol |= 1;
			      offset &= match_bit;
			    }
			  else
			    offset &= ~match_bit;
			}
		      while (symbol < 0x100);
		    }
		  DBG("lzma_literal: symbol=%#x @%u", symbol, frame.outcount);
		  outbuf[frame.outcount++] = symbol;
		  /* lzma_state_literal */
		  if (frame.state <= STATE_SHORTREP_LIT_LIT)
		    frame.state = STATE_LIT_LIT;
		  else if (frame.state <= STATE_LIT_SHORTREP)
		    frame.state -= 3;
		  else
		    frame.state -= 6;
		}
	      else if (UNLIKELY(!rc_normalize(&frame)))
		goto rc_limit_reached;
	      else
		{
		  unsigned int len;

		  if (rc_bit(&frame, &frame.probs.is_rep[frame.state]))
		    {
		      /* lzma_rep_match */
		      DBG("lzma_rep_match");
		      if (UNLIKELY(!rc_normalize(&frame)))
			goto rc_limit_reached;
		      if (!rc_bit(&frame, &frame.probs.is_rep0[frame.state]))
			{
			  if (UNLIKELY(!rc_normalize(&frame)))
			    goto rc_limit_reached;
			  if (!rc_bit(&frame, &frame.probs.is_rep0_long[frame.state][pos_state]))
			    {
			      /* lzma_state_short_rep */
			      frame.state = (frame.state < LIT_STATES ?
					     STATE_LIT_SHORTREP :
					     STATE_NONLIT_REP);
			      len = 1;
			      goto got_len;
			    }
			}
		      else
			{
			  uint_fast32_t tmp;

			  if (UNLIKELY(!rc_normalize(&frame)))
			    goto rc_limit_reached;
			  if (!rc_bit(&frame, &frame.probs.is_rep1[frame.state]))
			    tmp = frame.rep[1];
			  else
			    {
			      if (UNLIKELY(!rc_normalize(&frame)))
				goto rc_limit_reached;
			      if (!rc_bit(&frame, &frame.probs.is_rep2[frame.state]))
				tmp = frame.rep[2];
			      else
				{
				  tmp = frame.rep[3];
				  frame.rep[3] = frame.rep[2];
				}
			      frame.rep[2] = frame.rep[1];
			    }
			  frame.rep[1] = frame.rep[0];
			  frame.rep[0] = tmp;
			}
		      /* lzma_state_long_rep */
		      frame.state = (frame.state < LIT_STATES ?
				     STATE_LIT_LONGREP :
				     STATE_NONLIT_REP);

		      len = lzma_len(&frame, &frame.probs.rep_len_dec, pos_state);
		      if (UNLIKELY(!len))
			goto rc_limit_reached;
		    got_len:
		      ;
		    }
		  else
		    {
		      /* lzma_match */
		      probability_t *probs;
		      unsigned int dist_slot;

		      DBG("lzma_match");

		      /* lzma_state_match */
		      frame.state = (frame.state < LIT_STATES ?
				     STATE_LIT_MATCH :
				     STATE_NONLIT_MATCH);

		      frame.rep[3] = frame.rep[2];
		      frame.rep[2] = frame.rep[1];
		      frame.rep[1] = frame.rep[0];

		      len = lzma_len(&frame, &frame.probs.match_len_dec, pos_state);
		      if (UNLIKELY(!len))
			goto rc_limit_reached;

		      probs = frame.probs.dist_slot[len < (DIST_STATES + MATCH_LEN_MIN) ?
						    len - MATCH_LEN_MIN :
						    DIST_STATES - 1];
		      dist_slot = rc_bittree(&frame, probs, DIST_SLOTS);
		      if (UNLIKELY(!dist_slot))
			goto rc_limit_reached;
		      DBG("dist_slot=%u", dist_slot - DIST_SLOTS);
		      if ((dist_slot -= DIST_SLOTS) < DIST_MODEL_START)
			frame.rep[0] = dist_slot;
		      else
			{
			  unsigned int symbol, mask;
			  unsigned int limit = (dist_slot >> 1) - 1;
			  frame.rep[0] = 2 + (dist_slot & 1);

			  if (dist_slot < DIST_MODEL_END)
			    {
			      frame.rep[0] <<= limit;
			      probs = &frame.probs.dist_special[frame.rep[0] - dist_slot - 1];
			    }
			  else
			    {
			      /* rc_direct */
			      limit -= ALIGN_BITS;
			      do
				{
				  if (UNLIKELY(!rc_normalize(&frame)))
				    goto rc_limit_reached;
				  frame.rc_code -= (frame.rc_range >>= 1);
				  frame.rep[0] <<= 1;
				  if (frame.rc_code >> 31)
				    frame.rc_code += frame.rc_range;
				  else
				    frame.rep[0] |= 1;
				}
			      while (--limit > 0);

			      frame.rep[0] <<= ALIGN_BITS;
			      limit = ALIGN_BITS;
			      probs = frame.probs.dist_align;
			    }
			  /* rc_bittree_reverse */
			  symbol = 1;
			  limit = 1 << limit;
			  mask = 1;
			  do
			    {
			      unsigned int bit;

			      if (UNLIKELY(!rc_normalize(&frame)))
				goto rc_limit_reached;
			      bit = rc_bit(&frame, &probs[symbol]);
			      symbol <<= 1;
			      if (bit)
				{
				  symbol |= 1;
				  frame.rep[0] += mask;
				}
			    }
			  while ((mask <<= 1) < limit);
			}
		    }

		  /* dict_repeat */
		  DBG("dict_repeat: len=%u, dist=%u @%u",
		      len, frame.rep[0], frame.outcount);
		  if (UNLIKELY(frame.outcount - dict_origin <= frame.rep[0]))
		    RETURN(UNCOMPRESS_DATA_ERROR);
		  else
		    {
		      uint8_t *dst = &outbuf[frame.outcount];
		      const uint8_t *src = dst - frame.rep[0] - 1;

		      if (UNLIKELY((out_limit - frame.outcount) < len))
			{
			  len = out_limit - frame.outcount;
			  ret = more_run ? UNCOMPRESS_DATA_ERROR : UNCOMPRESS_OUTLIMIT;
			}
		      frame.outcount += len;
		      do
			*dst++ = *src++;
		      while (--len);

		      if (UNLIKELY(ret != UNCOMPRESS_OK))
			goto finish;
		    }
		}
	    }

	  if (UNLIKELY(frame.incount < frame.rc_limit))
	    RETURN(UNCOMPRESS_DATA_ERROR);
	}
      else if (UNLIKELY(control > 0x02))
	RETURN(UNCOMPRESS_DATA_ERROR);
      else if (UNLIKELY((frame.inlimit - frame.incount) < 2))
	RETURN(UNCOMPRESS_INLIMIT);
      else
	{
	  unsigned int copy_len;
	  const uint8_t *const p = &frame.inbuf[frame.incount];
	  frame.incount += 2;
	  copy_len = (p[0] << 8) + p[1] + 1;
	  if (UNLIKELY((frame.inlimit - frame.incount) < copy_len))
	    {
	      copy_len = frame.inlimit - frame.incount;
	      ret = UNCOMPRESS_INLIMIT;
	    }

	  if (UNLIKELY((*outsizep - frame.outcount) < copy_len))
	    {
	      copy_len = *outsizep - frame.outcount;
	      ret = UNCOMPRESS_OUTLIMIT;
	    }
	  frame.incount += copy_len;
	  memcpy(&outbuf[frame.outcount], &p[2], copy_len);
	  frame.outcount += copy_len;
	  if (UNLIKELY(ret != UNCOMPRESS_OK))
	    goto finish;
	}
    }

 rc_limit_reached:
  ret = (frame.incount >= frame.inlimit ?
	 UNCOMPRESS_INLIMIT :
	 UNCOMPRESS_DATA_ERROR);
 finish:
  *insizep = frame.incount;
  *outsizep = frame.outcount;
  return ret;
}
