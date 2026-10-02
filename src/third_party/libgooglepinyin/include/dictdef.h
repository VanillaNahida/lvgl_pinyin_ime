/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PINYINIME_INCLUDE_DICTDEF_H__
#define PINYINIME_INCLUDE_DICTDEF_H__

#include <stdlib.h>
#include <inttypes.h>
#include "./utf16char.h"

namespace ime_pinyin {

// Enable the following line when building the binary dictionary model.
//
// Patch P0 (lvgl_pinyin_ime): upstream hard-enabled the model builder, which
// drags dictbuilder.cpp, utf16reader.cpp and the raw word list into the
// firmware image. The firmware needs the decoder only, so it compiles with
// -DPINYINIME_RUNTIME_ONLY; the host toolchain (which still builds
// dict_pinyin.dat) simply leaves the macro undefined.
#ifndef PINYINIME_RUNTIME_ONLY
#define ___BUILD_MODEL___
#endif

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;

typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;
typedef uint64_t uint64;

const bool kPrintDebug0 = false;
const bool kPrintDebug1 = false;
const bool kPrintDebug2 = false;

// The max length of a lemma.
const size_t kMaxLemmaSize = 8;

// The max length of a Pinyin (spelling).
const size_t kMaxPinyinSize = 6;

// The number of half spelling ids. For Chinese Pinyin, there 30 half ids.
// See SpellingTrie.h for details.
const size_t kHalfSpellingIdNum = 29;

// The maximum number of full spellings. For Chinese Pinyin, there are only
// about 410 spellings.
// If change this value is bigger(needs more bits), please also update
// other structures like SpellingNode, to make sure than a spelling id can be
// stored.
// -1 is because that 0 is never used.
const size_t kMaxSpellingNum = 512 - kHalfSpellingIdNum - 1;
const size_t kMaxSearchSteps = 40;

// One character predicts its following characters.
const size_t kMaxPredictSize = (kMaxLemmaSize - 1);

// LemmaIdType must always be size_t.
typedef size_t LemmaIdType;
const size_t kLemmaIdSize = 3;  // Actually, a Id occupies 3 bytes in storage.
const size_t kLemmaIdComposing = 0xffffff;

typedef uint16 LmaScoreType;
typedef uint16 KeyScoreType;

// Number of items with highest score are kept for prediction purpose.
const size_t kTopScoreLemmaNum = 10;

const size_t kMaxPredictNumByGt3 = 1;
const size_t kMaxPredictNumBy3 = 2;
const size_t kMaxPredictNumBy2 = 2;

// The last lemma id (included) for the system dictionary. The system
// dictionary's ids always start from 1.
const LemmaIdType kSysDictIdEnd = 500000;

// The first lemma id for the user dictionary.
const LemmaIdType kUserDictIdStart = 500001;

// The last lemma id (included) for the user dictionary.
const LemmaIdType kUserDictIdEnd = 600000;

typedef struct {
  uint16 half_splid:5;
  uint16 full_splid:11;
} SpellingId, *PSpellingId;


/**
 * We use different node types for different layers
 * Statistical data of the building result for a testing dictionary:
 *                              root,   level 0,   level 1,   level 2,   level 3
 * max son num of one node:     406        280         41          2          -
 * max homo num of one node:      0         90         23          2          2
 * total node num of a layer:     1        406      31766      13516        993
 * total homo num of a layer:     9       5674      44609      12667        995
 *
 * The node number for root and level 0 won't be larger than 500
 * According to the information above, two kinds of nodes can be used; one for
 * root and level 0, the other for these layers deeper than 0.
 *
 * LE = less and equal,
 * A node occupies 16 bytes. so, totallly less than 16 * 500 = 8K
 *
 * Patch P11 (lvgl_pinyin_ime): the two offsets are uint32, not size_t. These
 * nodes are serialised as a raw struct array (fwrite(root_, sizeof(LmaNodeLE0),
 * ...)), so a word-sized field makes the file layout depend on the machine that
 * built the dictionary: 24 bytes per node on a 64-bit host against 16 on the
 * 32-bit target. The reader then walks the array at the wrong stride, every
 * following offset is garbage, and the load corrupts the heap. Both fields are
 * indices into the lemma table (far below 2^32), so uint32 is enough and makes
 * the layout identical everywhere.
 *
 * With uint32 offsets the struct is 4+4+2+2+2 = 14 bytes of fields, padded to
 * 16 by alignment (2 bytes of tail padding) on the 32-bit target and on a 64-bit
 * host alike, because the widest member is 4 bytes either way. Padding at the
 * end is harmless: it is written as zeros and the reader never looks at it. The
 * struct must NOT be packed, which would make it 14 bytes and shift every node.
 */
struct LmaNodeLE0 {
  uint32 son_1st_off;
  uint32 homo_idx_buf_off;
  uint16 spl_idx;
  uint16 num_of_son;
  uint16 num_of_homo;
};

/**
 * GE = great and equal
 * A node occupies 8 bytes. It is already word-size independent: it must NOT be
 * packed, because upstream writes 10 bytes per node (8 bytes of counters plus 2
 * bytes of natural tail padding) and the reader expects exactly that.
 */
struct LmaNodeGE1 {
  uint16 son_1st_off_l;        // Low bits of the son_1st_off
  uint16 homo_idx_buf_off_l;   // Low bits of the homo_idx_buf_off_1
  uint16 spl_idx;
  unsigned char num_of_son;            // number of son nodes
  unsigned char num_of_homo;           // number of homo words
  unsigned char son_1st_off_h;         // high bits of the son_1st_off
  unsigned char homo_idx_buf_off_h;    // high bits of the homo_idx_buf_off
};

#ifdef ___BUILD_MODEL___
struct SingleCharItem {
  float freq;
  char16 hz;
  SpellingId splid;
};

struct LemmaEntry {
  LemmaIdType idx_by_py;
  LemmaIdType idx_by_hz;
  char16 hanzi_str[kMaxLemmaSize + 1];

  // The SingleCharItem id for each Hanzi.
  uint16 hanzi_scis_ids[kMaxLemmaSize];

  uint16 spl_idx_arr[kMaxLemmaSize + 1];
  char pinyin_str[kMaxLemmaSize][kMaxPinyinSize + 1];
  unsigned char hz_str_len;
  float freq;
};
#endif  // ___BUILD_MODEL___

}  //  namespace ime_pinyin

#endif  // PINYINIME_INCLUDE_DICTDEF_H__
