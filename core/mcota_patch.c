#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif

#include "mcota_patch.h"

typedef struct {
  const uint8_t *in;
  uint32_t in_len;
  uint32_t pos;
  uint32_t hold;
  uint32_t held;
  uint8_t *out;
  uint32_t cap;
  uint32_t op;
  bool bad;
} inflate_t;

typedef struct {
  uint16_t count[16];
  uint16_t symbol[288];
} huffman_t;

static huffman_t inflate_lit;
static huffman_t inflate_dist;
static uint8_t inflate_lengths[320];

static const uint16_t length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t length_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t length_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static uint32_t take_bits(inflate_t *s, uint32_t n) {
  uint32_t value;
  while (s->held < n) {
    if (s->pos >= s->in_len) { s->bad = true; return 0u; }
    s->hold |= (uint32_t)s->in[s->pos++] << s->held;
    s->held += 8u;
  }
  value = s->hold & ((1u << n) - 1u);
  s->hold >>= n;
  s->held -= n;
  return value;
}

static void build_huffman(huffman_t *h, const uint8_t *lengths, uint32_t n) {
  uint16_t offsets[16];
  uint32_t i;
  for (i = 0; i < 16u; i++) h->count[i] = 0;
  for (i = 0; i < n; i++) h->count[lengths[i]]++;
  h->count[0] = 0;
  offsets[1] = 0;
  for (i = 1; i < 15u; i++) offsets[i + 1u] = (uint16_t)(offsets[i] + h->count[i]);
  for (i = 0; i < n; i++) {
    if (lengths[i]) h->symbol[offsets[lengths[i]]++] = (uint16_t)i;
  }
}

static uint32_t take_symbol(inflate_t *s, const huffman_t *h) {
  int32_t code = 0, first = 0, index = 0;
  uint32_t len;
  for (len = 1; len <= 15u; len++) {
    int32_t count = h->count[len];
    code |= (int32_t)take_bits(s, 1u);
    if (s->bad) return 0u;
    if (code - count < first) return h->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  s->bad = true;
  return 0u;
}

static void inflate_codes(inflate_t *s) {
  while (!s->bad) {
    uint32_t symbol = take_symbol(s, &inflate_lit);
    if (s->bad) return;
    if (symbol < 256u) {
      if (s->op >= s->cap) { s->bad = true; return; }
      s->out[s->op++] = (uint8_t)symbol;
    } else if (symbol == 256u) {
      return;
    } else {
      uint32_t len, dist, i;
      symbol -= 257u;
      if (symbol >= 29u) { s->bad = true; return; }
      len = length_base[symbol] + take_bits(s, length_extra[symbol]);
      symbol = take_symbol(s, &inflate_dist);
      if (s->bad || symbol >= 30u) { s->bad = true; return; }
      dist = dist_base[symbol] + take_bits(s, dist_extra[symbol]);
      if (s->bad || dist > s->op || len > s->cap - s->op) { s->bad = true; return; }
      for (i = 0; i < len; i++, s->op++) s->out[s->op] = s->out[s->op - dist];
    }
  }
}

static void inflate_dynamic(inflate_t *s) {
  uint32_t lit = take_bits(s, 5u) + 257u, dist = take_bits(s, 5u) + 1u, codes = take_bits(s, 4u) + 4u, i;
  if (s->bad || lit > 286u || dist > 30u) { s->bad = true; return; }
  for (i = 0; i < 19u; i++) inflate_lengths[i] = 0;
  for (i = 0; i < codes; i++) inflate_lengths[length_order[i]] = (uint8_t)take_bits(s, 3u);
  build_huffman(&inflate_lit, inflate_lengths, 19u);
  for (i = 0; i < lit + dist && !s->bad; ) {
    uint32_t symbol = take_symbol(s, &inflate_lit), repeat, value = 0;
    if (s->bad) return;
    if (symbol < 16u) { inflate_lengths[i++] = (uint8_t)symbol; continue; }
    if (symbol == 16u) {
      if (i == 0u) { s->bad = true; return; }
      value = inflate_lengths[i - 1u];
      repeat = 3u + take_bits(s, 2u);
    } else if (symbol == 17u) {
      repeat = 3u + take_bits(s, 3u);
    } else {
      repeat = 11u + take_bits(s, 7u);
    }
    if (i + repeat > lit + dist) { s->bad = true; return; }
    while (repeat--) inflate_lengths[i++] = (uint8_t)value;
  }
  if (s->bad || inflate_lengths[256] == 0u) { s->bad = true; return; }
  build_huffman(&inflate_dist, inflate_lengths + lit, dist);
  build_huffman(&inflate_lit, inflate_lengths, lit);
  inflate_codes(s);
}

static void inflate_fixed(inflate_t *s) {
  uint32_t i;
  for (i = 0; i < 144u; i++) inflate_lengths[i] = 8;
  for (; i < 256u; i++) inflate_lengths[i] = 9;
  for (; i < 280u; i++) inflate_lengths[i] = 7;
  for (; i < 288u; i++) inflate_lengths[i] = 8;
  build_huffman(&inflate_lit, inflate_lengths, 288u);
  for (i = 0; i < 30u; i++) inflate_lengths[i] = 5;
  build_huffman(&inflate_dist, inflate_lengths, 30u);
  inflate_codes(s);
}

static void inflate_stored(inflate_t *s) {
  uint32_t len, i;
  s->hold = 0;
  s->held = 0;
  if (s->pos + 4u > s->in_len) { s->bad = true; return; }
  len = (uint32_t)s->in[s->pos] | ((uint32_t)s->in[s->pos + 1u] << 8);
  if ((len ^ 0xFFFFu) != ((uint32_t)s->in[s->pos + 2u] | ((uint32_t)s->in[s->pos + 3u] << 8))) { s->bad = true; return; }
  s->pos += 4u;
  if (len > s->in_len - s->pos || len > s->cap - s->op) { s->bad = true; return; }
  for (i = 0; i < len; i++) s->out[s->op++] = s->in[s->pos++];
}

int32_t mcota_inflate(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_cap) {
  inflate_t s;
  uint32_t last;
  s.in = in; s.in_len = in_len; s.pos = 0; s.hold = 0; s.held = 0; s.out = out; s.cap = out_cap; s.op = 0; s.bad = false;
  do {
    uint32_t type;
    last = take_bits(&s, 1u);
    type = take_bits(&s, 2u);
    if (s.bad) return -1;
    if (type == 0u) inflate_stored(&s);
    else if (type == 1u) inflate_fixed(&s);
    else if (type == 2u) inflate_dynamic(&s);
    else s.bad = true;
    if (s.bad) return -1;
  } while (!last);
  return (int32_t)s.op;
}

typedef struct {
  uint8_t *out;
  uint32_t cap;
  uint32_t op;
  uint32_t hold;
  uint32_t held;
  bool full;
} deflate_t;

static uint16_t deflate_head[1024];
static uint16_t deflate_tokens[2u * MCOTA_PAGE_SIZE];
static uint16_t deflate_freq[286 + 30 + 19];
static uint8_t deflate_len[286 + 30 + 19];
static uint16_t deflate_code[286 + 30 + 19];
static uint32_t deflate_weight[572];
static uint16_t deflate_parent[572];
static uint8_t deflate_taken[572];
static uint8_t deflate_sequence[316];

static void put_bits(deflate_t *s, uint32_t value, uint32_t n) {
  s->hold |= value << s->held;
  s->held += n;
  while (s->held >= 8u) {
    if (s->op >= s->cap) { s->full = true; s->held = 0; return; }
    s->out[s->op++] = (uint8_t)s->hold;
    s->hold >>= 8;
    s->held -= 8u;
  }
}

static void put_code(deflate_t *s, uint32_t code, uint32_t n) {
  uint32_t reversed = 0, i;
  for (i = 0; i < n; i++) reversed |= ((code >> i) & 1u) << (n - 1u - i);
  put_bits(s, reversed, n);
}

static uint32_t tokenize(const uint8_t *in, uint32_t len) {
  uint32_t i = 0, count = 0, k;
  for (k = 0; k < 1024u; k++) deflate_head[k] = 0;
  while (i < len) {
    uint32_t best = 0, dist = 0;
    if (i + 3u <= len) {
      uint32_t hash = (((uint32_t)in[i] << 7) ^ ((uint32_t)in[i + 1u] << 4) ^ (uint32_t)in[i + 2u] ^ ((uint32_t)in[i + 2u] << 9)) & 1023u;
      uint32_t at = deflate_head[hash];
      deflate_head[hash] = (uint16_t)(i + 1u);
      if (at > 0u) {
        uint32_t from = at - 1u, n = 0, most = len - i < 258u ? len - i : 258u;
        while (n < most && in[from + n] == in[i + n]) n++;
        if (n >= 3u) { best = n; dist = i - from; }
      }
    }
    if (best >= 3u) {
      deflate_tokens[count++] = (uint16_t)(0x8000u | best);
      deflate_tokens[count++] = (uint16_t)dist;
      i += best;
    } else {
      deflate_tokens[count++] = in[i++];
    }
  }
  return count;
}

static uint32_t length_symbol(uint32_t len) {
  uint32_t symbol = 28u;
  while (length_base[symbol] > len) symbol--;
  return symbol;
}

static uint32_t dist_symbol(uint32_t dist) {
  uint32_t symbol = 29u;
  while (dist_base[symbol] > dist) symbol--;
  return symbol;
}

static void make_lengths(const uint16_t *freq, uint32_t n, uint8_t *len, uint32_t limit) {
  uint32_t shift = 0, i;
  for (;;) {
    uint32_t count = 0, next = n, last = 0, deepest = 0;
    for (i = 0; i < n; i++) {
      deflate_weight[i] = freq[i] ? ((uint32_t)freq[i] >> shift) | 1u : 0u;
      deflate_taken[i] = 0;
      len[i] = 0;
      if (freq[i]) { count++; last = i; }
    }
    if (count == 0u) return;
    if (count == 1u) { len[last] = 1; return; }
    while (count > 1u) {
      uint32_t a = 0xFFFFu, b = 0xFFFFu;
      for (i = 0; i < next; i++) {
        if (deflate_taken[i] || deflate_weight[i] == 0u) continue;
        if (a == 0xFFFFu || deflate_weight[i] < deflate_weight[a]) { b = a; a = i; }
        else if (b == 0xFFFFu || deflate_weight[i] < deflate_weight[b]) b = i;
      }
      deflate_weight[next] = deflate_weight[a] + deflate_weight[b];
      deflate_taken[next] = 0;
      deflate_taken[a] = 1;
      deflate_taken[b] = 1;
      deflate_parent[a] = (uint16_t)next;
      deflate_parent[b] = (uint16_t)next;
      next++;
      count--;
    }
    for (i = 0; i < n; i++) {
      uint32_t depth = 0, at = i;
      if (!freq[i]) continue;
      while (at != next - 1u) { at = deflate_parent[at]; depth++; }
      len[i] = (uint8_t)depth;
      if (depth > deepest) deepest = depth;
    }
    if (deepest <= limit) return;
    shift++;
  }
}

static void make_codes(const uint8_t *len, uint16_t *code, uint32_t n) {
  uint16_t count[16], next[16];
  uint32_t i, value = 0;
  for (i = 0; i < 16u; i++) count[i] = 0;
  for (i = 0; i < n; i++) count[len[i]]++;
  count[0] = 0;
  for (i = 1; i < 16u; i++) {
    value = (value + count[i - 1u]) << 1;
    next[i] = (uint16_t)value;
  }
  for (i = 0; i < n; i++) {
    if (len[i]) code[i] = next[len[i]]++;
  }
}

static void put_tokens(deflate_t *s, uint32_t count, const uint8_t *lit_len, const uint16_t *lit_code, const uint8_t *dist_len, const uint16_t *dist_code) {
  uint32_t i;
  for (i = 0; i < count && !s->full; i++) {
    uint32_t token = deflate_tokens[i];
    if (token & 0x8000u) {
      uint32_t len = token & 0x7FFFu, dist = deflate_tokens[++i], symbol = length_symbol(len);
      put_code(s, lit_code[257u + symbol], lit_len[257u + symbol]);
      put_bits(s, len - length_base[symbol], length_extra[symbol]);
      symbol = dist_symbol(dist);
      put_code(s, dist_code[symbol], dist_len[symbol]);
      put_bits(s, dist - dist_base[symbol], dist_extra[symbol]);
    } else {
      put_code(s, lit_code[token], lit_len[token]);
    }
  }
  put_code(s, lit_code[256], lit_len[256]);
  if (s->held > 0u) put_bits(s, 0u, 8u - s->held);
}

static uint32_t deflate_dynamic(uint32_t count, uint8_t *out, uint32_t cap) {
  deflate_t s;
  uint16_t *lit_freq = deflate_freq, *dist_freq = deflate_freq + 286, *cl_freq = deflate_freq + 316;
  uint8_t *lit_len = deflate_len, *dist_len = deflate_len + 286, *cl_len = deflate_len + 316;
  uint16_t *lit_code = deflate_code, *dist_code = deflate_code + 286, *cl_code = deflate_code + 316;
  uint32_t i, lit = 286u, dist = 30u, total, codes = 19u, items = 0;
  for (i = 0; i < 335u; i++) deflate_freq[i] = 0;
  for (i = 0; i < count; i++) {
    uint32_t token = deflate_tokens[i];
    if (token & 0x8000u) {
      lit_freq[257u + length_symbol(token & 0x7FFFu)]++;
      dist_freq[dist_symbol(deflate_tokens[++i])]++;
    } else {
      lit_freq[token]++;
    }
  }
  lit_freq[256] = 1;
  if (dist_freq[0] == 0u) dist_freq[0] = 1;
  if (dist_freq[1] == 0u) dist_freq[1] = 1;
  make_lengths(lit_freq, 286u, lit_len, 15u);
  make_lengths(dist_freq, 30u, dist_len, 15u);
  make_codes(lit_len, lit_code, 286u);
  make_codes(dist_len, dist_code, 30u);
  while (lit > 257u && lit_len[lit - 1u] == 0u) lit--;
  while (dist > 1u && dist_len[dist - 1u] == 0u) dist--;
  total = lit + dist;
  for (i = 0; i < total; ) {
    uint32_t value = i < lit ? lit_len[i] : dist_len[i - lit], run = 1;
    while (i + run < total && (i + run < lit ? lit_len[i + run] : dist_len[i + run - lit]) == value) run++;
    if (value == 0u && run >= 3u) {
      if (run > 138u) run = 138u;
      deflate_sequence[items++] = run >= 11u ? 18u : 17u;
      deflate_sequence[items++] = (uint8_t)run;
      cl_freq[run >= 11u ? 18u : 17u]++;
      i += run;
    } else {
      deflate_sequence[items++] = (uint8_t)value;
      cl_freq[value]++;
      i++;
    }
  }
  make_lengths(cl_freq, 19u, cl_len, 7u);
  make_codes(cl_len, cl_code, 19u);
  while (codes > 4u && cl_len[length_order[codes - 1u]] == 0u) codes--;
  s.out = out; s.cap = cap; s.op = 0; s.hold = 0; s.held = 0; s.full = false;
  put_bits(&s, 5u, 3u);
  put_bits(&s, lit - 257u, 5u);
  put_bits(&s, dist - 1u, 5u);
  put_bits(&s, codes - 4u, 4u);
  for (i = 0; i < codes; i++) put_bits(&s, cl_len[length_order[i]], 3u);
  for (i = 0; i < items; i++) {
    uint32_t symbol = deflate_sequence[i];
    put_code(&s, cl_code[symbol], cl_len[symbol]);
    if (symbol == 17u) put_bits(&s, (uint32_t)deflate_sequence[++i] - 3u, 3u);
    else if (symbol == 18u) put_bits(&s, (uint32_t)deflate_sequence[++i] - 11u, 7u);
  }
  put_tokens(&s, count, lit_len, lit_code, dist_len, dist_code);
  return s.full ? 0u : s.op;
}

uint32_t mcota_deflate(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t cap) {
  uint32_t n = deflate_dynamic(tokenize(in, len), out, cap), k;
  if (n > 0u && n < len + 5u) return n;
  if (len + 5u > cap) return 0u;
  out[0] = 1u;
  out[1] = (uint8_t)len;
  out[2] = (uint8_t)(len >> 8);
  out[3] = (uint8_t)~len;
  out[4] = (uint8_t)~(len >> 8);
  for (k = 0; k < len; k++) out[5u + k] = in[k];
  return len + 5u;
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool is_branch(const uint8_t *b) {
  return (b[1] & 0xF8u) == 0xF0u && ((b[3] & 0xF8u) == 0xF8u || (b[3] & 0xF8u) == 0xB8u);
}

static uint32_t branch_get(const uint8_t *b) {
  return ((((uint32_t)b[1] & 7u) << 19) | ((uint32_t)b[0] << 11) | (((uint32_t)b[3] & 7u) << 8) | (uint32_t)b[2]) << 1;
}

static void branch_put(uint8_t *b, uint32_t value) {
  value = (value & 0x7FFFFFu) >> 1;
  b[1] = (uint8_t)(0xF0u | ((value >> 19) & 7u));
  b[0] = (uint8_t)(value >> 11);
  b[3] = (uint8_t)((b[3] & 0xF8u) | ((value >> 8) & 7u));
  b[2] = (uint8_t)value;
}

typedef struct {
  const uint8_t *p;
  const uint8_t *end;
  bool bad;
} reader_t;

static uint32_t rd_var(reader_t *r) {
  uint32_t value = 0, shift = 0;
  for (;;) {
    uint8_t b;
    if (r->p >= r->end || shift > 28u) { r->bad = true; return 0u; }
    b = *r->p++;
    value |= (uint32_t)(b & 0x7Fu) << shift;
    if (!(b & 0x80u)) return value;
    shift += 7u;
  }
}

static uint32_t unzig(uint32_t v) {
  return (v >> 1) ^ (0u - (v & 1u));
}

const uint8_t *mcota_view_parse(mcota_view_t *v, const uint8_t *p, const uint8_t *end, bool thumb, bool xtensa) {
  reader_t r = { p, end, false };
  uint32_t i;
  v->thumb = thumb;
  v->xtensa = xtensa;
  v->base = rd_var(&r) << 12;
  if (r.bad || r.p >= r.end) return 0;
  v->shift_count = *r.p++;
  v->shifts = r.p;
  for (i = 0; i < v->shift_count; i++) {
    rd_var(&r);
    if (rd_var(&r) == 0u) return 0;
    rd_var(&r);
  }
  v->old_code = v->old_code_end = v->new_code = v->new_code_end = 0u;
  if (xtensa) {
    v->old_code = rd_var(&r);
    v->old_code_end = v->old_code + rd_var(&r);
    v->new_code = rd_var(&r);
    v->new_code_end = v->new_code + rd_var(&r);
  }
  return r.bad ? 0 : r.p;
}

typedef struct {
  const uint8_t *shifts;
  uint32_t count;
  uint32_t low;
  uint32_t high;
} shifts_t;

static void shifts_open(shifts_t *t, const uint8_t *shifts, uint32_t count) {
  reader_t r = { shifts, shifts + count * 15u, false };
  uint32_t i, at = 0;
  t->shifts = shifts;
  t->count = count;
  t->low = t->high = 0;
  for (i = 0; i < count; i++) {
    at += rd_var(&r);
    if (i == 0u) t->low = at;
    at += rd_var(&r);
    rd_var(&r);
  }
  t->high = at;
}

static uint32_t shift_of(const shifts_t *t, uint32_t target) {
  reader_t r = { t->shifts, t->shifts + t->count * 15u, false };
  uint32_t i, at = 0;
  if (target < t->low || target >= t->high) return 0u;
  for (i = 0; i < t->count; i++) {
    uint32_t length, delta;
    at += rd_var(&r);
    if (target < at) return 0u;
    length = rd_var(&r);
    delta = unzig(rd_var(&r));
    if (target - at < length) return delta;
    at += length;
  }
  return 0u;
}

static void l32r(uint8_t *data, uint32_t len, uint32_t pos, uint32_t low, uint32_t high, const shifts_t *t) {
  uint32_t i = pos < low ? low - pos : 0u;
  if (high < pos + len) len = high > pos ? high - pos : 0u;
  while (i + 3u <= len) {
    uint32_t op = data[i] & 0x0Fu;
    if (op >= 8u) { i += 2u; continue; }
    if (op == 1u) {
      uint32_t imm = (uint32_t)data[i + 1u] | ((uint32_t)data[i + 2u] << 8), at = pos + i + 3u, value;
      if (t) {
        value = imm + (at >> 2);
        value += shift_of(t, (at & ~3u) + ((imm | 0xFFFF0000u) << 2)) >> 2;
      } else {
        value = imm - (at >> 2);
      }
      data[i + 1u] = (uint8_t)value;
      data[i + 2u] = (uint8_t)(value >> 8);
    }
    i += 3u;
  }
}

void mcota_view_predict(const mcota_view_t *v, uint8_t *data, uint32_t len, uint32_t pos) {
  shifts_t t;
  uint32_t i;
  shifts_open(&t, v->shifts, v->shift_count);
  if (v->shift_count) {
    for (i = 0; i + 4u <= len; i += 4u) {
      uint32_t value = rd32(data + i);
      uint32_t delta = shift_of(&t, (value & ~1u) - v->base);
      if (delta) {
        value += delta;
        data[i] = (uint8_t)value;
        data[i + 1u] = (uint8_t)(value >> 8);
        data[i + 2u] = (uint8_t)(value >> 16);
        data[i + 3u] = (uint8_t)(value >> 24);
      }
    }
  }
  if (v->xtensa) l32r(data, len, pos, v->old_code, v->old_code_end, &t);
  if (!v->thumb) return;
  i = 0;
  while (i + 4u <= len) {
    if (is_branch(data + i)) {
      uint32_t target = (branch_get(data + i) + pos + i + 4u) & 0x7FFFFFu;
      branch_put(data + i, target + shift_of(&t, target));
      i += 4u;
    } else {
      i += 2u;
    }
  }
}

void mcota_view_restore(const mcota_view_t *v, uint8_t *data, uint32_t len, uint32_t pos) {
  uint32_t i = 0;
  if (v->xtensa) l32r(data, len, pos, v->new_code, v->new_code_end, 0);
  if (!v->thumb) return;
  while (i + 4u <= len) {
    if (is_branch(data + i)) {
      branch_put(data + i, branch_get(data + i) - (pos + i + 4u));
      i += 4u;
    } else {
      i += 2u;
    }
  }
}

bool mcota_block_apply(mcota_old_read_t old_read, void *ctx, uint32_t old_size,
                      const uint8_t *block, uint32_t block_len, uint8_t *work, uint32_t work_cap,
                      uint8_t *page, uint32_t page_len, uint32_t page_pos) {
  int32_t n = mcota_inflate(block, block_len, work, work_cap);
  uint32_t ops, total_add = 0, total_extra = 0, out = 0, i, zeros = 0xFFFFFFFFu, left = 0, expect = page_pos;
  reader_t head, gaps;
  const uint8_t *diff, *extra;
  bool sparse;
  if (n < 1) return false;
  ops = work[0] & 0x7Fu;
  sparse = (work[0] & 0x80u) != 0u;
  if (ops > MCOTA_BLOCK_OPS_MAX) return false;
  head.p = work + 1u;
  head.end = work + n;
  head.bad = false;
  for (i = 0; i < ops; i++) {
    uint32_t add = rd_var(&head), ext = rd_var(&head);
    if (add) rd_var(&head);
    if (add > page_len || ext > page_len) return false;
    total_add += add;
    total_extra += ext;
  }
  if (head.bad || total_add + total_extra != page_len) return false;
  gaps = head;
  diff = head.p;
  if (sparse) {
    left = rd_var(&gaps);
    if (gaps.bad || left > total_add) return false;
    diff = gaps.p;
    for (i = 0; i < left; i++) {
      reader_t skip = { diff, head.end, false };
      rd_var(&skip);
      if (skip.bad) return false;
      diff = skip.p;
    }
    gaps.end = diff;
    if ((uint32_t)(head.end - diff) < left) return false;
    extra = diff + left;
    if (left) zeros = rd_var(&gaps);
  } else {
    if ((uint32_t)(head.end - diff) < total_add) return false;
    extra = diff + total_add;
  }
  if ((uint32_t)(head.end - extra) != total_extra) return false;
  head.p = work + 1u;
  for (i = 0; i < ops; i++) {
    uint32_t add = rd_var(&head), ext = rd_var(&head), pos = 0, k;
    if (add) {
      pos = expect + unzig(rd_var(&head));
      expect = pos + add;
    }
    expect += ext;
    for (k = 0; k < add; ) {
      uint8_t buf[64];
      uint32_t chunk = add - k > sizeof(buf) ? (uint32_t)sizeof(buf) : add - k, j;
      for (j = 0; j < chunk; j++) buf[j] = 0;
      if (pos + k < old_size) {
        uint32_t avail = old_size - (pos + k);
        old_read(ctx, pos + k, buf, avail < chunk ? avail : chunk);
      }
      for (j = 0; j < chunk; j++) {
        uint8_t d = 0;
        if (!sparse) {
          d = *diff++;
        } else if (zeros) {
          zeros--;
        } else {
          d = *diff++;
          left--;
          zeros = left ? rd_var(&gaps) : 0xFFFFFFFFu;
        }
        page[out++] = (uint8_t)(buf[j] + d);
      }
      k += chunk;
    }
    for (k = 0; k < ext; k++) page[out++] = *extra++;
  }
  return out == page_len && !gaps.bad && (!sparse || left == 0u);
}
