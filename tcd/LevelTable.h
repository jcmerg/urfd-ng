#pragma once

// the index of `table` (n levels, rising) closest to table[from] + offset,
// `from` itself on a tie: the new code of a gain parameter (AMBE b2, IMBE b2)
// for a level change
#include <cmath>

static inline int NearestLevel(const float *table, int n, int from, float offset)
{
	const float want = table[from] + offset;
	int best = from;
	for (int i = 0; i < n; i++)
		if (fabsf(table[i] - want) < fabsf(table[best] - want))
			best = i;
	return best;
}
