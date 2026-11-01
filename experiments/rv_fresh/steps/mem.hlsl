// The layout of a core for step 8 on: one array of words `m`, 8 x 8 pixels of 8 texels (512 texels, 2,048 words), so that a pixel's
// output is the same few reads whatever it holds. Texel 0-7 registers (and the sink in texel 8's first word), 8 the pc and the count,
// 16 on the entries' tags (4 a texel), then their data. A pass starts with no entries: the commit has put them in RAM.
#define N_ENT 368
#define TAG0 (16 * 4)
#define DATA0 (TAG0 + N_ENT)   // (a tag a word; N_ENT is 92 texels)
#define EMPTY 0xffffffffu
#define TEXEL_OF_SLOT_TAG(i) (16 + (i) / 4)
