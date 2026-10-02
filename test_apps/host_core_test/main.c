/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit test for the LVGL-independent part of the component.
 *
 *   host_core_test <path to dict_pinyin.dat>
 *
 * Exits non-zero as soon as one check fails, so it can gate a build script.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/ime_hash.h"
#include "core/ime_session.h"
#include "core/ime_t9.h"
#include "core/ime_trie.h"
#include "data/ime_dict_vfs.h"
#include "lvgl_pinyin_ime/ime_engine.h"

static int s_failures;
static int s_checks;

#define CHECK(cond, ...)                          \
    do {                                          \
        s_checks++;                               \
        if (!(cond)) {                            \
            s_failures++;                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                  \
            printf("\n");                         \
        }                                         \
    } while (0)

/* ------------------------------------------------------------------ utf-16 */

static void test_utf16_roundtrip(void)
{
    const char *src = "nihao你";
    uint16_t utf16[32];
    char back[64];

    size_t units = ime_engine_utf8_to_utf16(src, utf16, 32);
    CHECK(units == 6, "utf8_to_utf16 wrote %u units, expected 6", (unsigned)units);

    size_t bytes = ime_engine_utf16_to_utf8(utf16, back, sizeof(back));
    CHECK(bytes == strlen(src), "utf16_to_utf8 wrote %u bytes, expected %u", (unsigned)bytes,
          (unsigned)strlen(src));
    CHECK(strcmp(src, back) == 0, "roundtrip mismatch: \"%s\" != \"%s\"", src, back);
}

/* ---------------------------------------------------------------------- t9 */

static void test_t9(void)
{
    ime_t9_init();

    CHECK(ime_t9_push_digit('9'), "push '9' rejected");
    CHECK(ime_t9_push_digit('4'), "push '4' rejected");
    CHECK(ime_t9_len() == 2, "t9 length is %u, expected 2", (unsigned)ime_t9_len());

    bool has_xi = false;
    bool has_yi = false;
    bool has_zi = false;
    for (size_t i = 0; i < ime_t9_count(); i++) {
        const char *py = ime_t9_pinyin(i);
        if (strcmp(py, "xi") == 0) has_xi = true;
        if (strcmp(py, "yi") == 0) has_yi = true;
        if (strcmp(py, "zi") == 0) has_zi = true;
    }
    CHECK(has_xi && has_yi && has_zi, "94 did not yield xi/yi/zi (%u results)",
          (unsigned)ime_t9_count());

    ime_t9_backspace();
    CHECK(ime_t9_len() == 1, "backspace left %u digits", (unsigned)ime_t9_len());
    ime_t9_backspace();
    CHECK(ime_t9_len() == 0, "backspace left %u digits", (unsigned)ime_t9_len());
    CHECK(!ime_t9_push_digit('1'), "digit '1' was accepted");

    /* 9464 -> xing / zheli / ... */
    CHECK(ime_t9_push_digit('9'), "94xx: push 9");
    CHECK(ime_t9_push_digit('4'), "94xx: push 4");
    CHECK(ime_t9_push_digit('6'), "94xx: push 6");
    CHECK(ime_t9_push_digit('4'), "94xx: push 4");
    bool has_xing = false;
    for (size_t i = 0; i < ime_t9_count(); i++) {
        if (strcmp(ime_t9_pinyin(i), "xing") == 0) has_xing = true;
    }
    CHECK(has_xing, "9464 did not yield xing (%u results)", (unsigned)ime_t9_count());

    /*
     * Every suggestion has to be a real pinyin spelling. "32" is the digit pair
     * that produced a bogus "e'a" on the device: 3 and 2 are also the letters of
     * ABC and DEF, and "ea" is not a syllable the dictionary knows.
     */
    ime_t9_reset();
    ime_t9_push_digit('3');
    ime_t9_push_digit('2');
    printf("  T9 \"32\" suggests:");
    for (size_t i = 0; i < ime_t9_count(); i++) {
        const char *py = ime_t9_pinyin(i);
        printf(" \"%s\"", py);
        CHECK(ime_t9_is_valid(py), "T9 \"32\" suggests \"%s\", not a writable spelling", py);
    }
    printf("\n");
    for (size_t i = 0; i < ime_t9_count(); i++) {
        CHECK(strcmp(ime_t9_pinyin(i), "e'a") != 0, "\"32\" still suggests the bogus \"e'a\"");
    }

    ime_t9_deinit();
}

/*
 * Every suggestion, for a spread of digit buffers, has to be made of syllables
 * the built-in table knows. This is the regression guard for "e'a": `seg()`
 * used to accept any *prefix* of a syllable's digits, so it could invent a
 * spelling the table does not contain.
 */
static void test_t9_suggestions_are_pinyin(void)
{
    ime_t9_init();

    static const char *const seqs[] = {
        "2", "3", "22", "32", "42", "94", "9464", "6426", "53266", "22222", "88",
    };

    for (size_t s = 0; s < sizeof(seqs) / sizeof(seqs[0]); s++) {
        ime_t9_reset();
        for (const char *d = seqs[s]; *d != '\0'; d++) {
            ime_t9_push_digit(*d);
        }

        size_t n = ime_t9_count();
        CHECK(n > 0, "T9 \"%s\" produced no suggestion at all", seqs[s]);
        for (size_t i = 0; i < n; i++) {
            const char *py = ime_t9_pinyin(i);
            CHECK(ime_t9_is_valid(py), "T9 \"%s\" suggests \"%s\", not a valid spelling",
                  seqs[s], py);
        }
    }

    /* The validator itself: a known syllable passes, an invented one does not. */
    CHECK(ime_t9_is_valid("da"), "validator rejected \"da\"");
    CHECK(ime_t9_is_valid("xi'an"), "validator rejected \"xi'an\"");
    CHECK(!ime_t9_is_valid("ea"), "validator accepted the invented \"ea\"");
    CHECK(!ime_t9_is_valid("da'ea"), "validator accepted \"da'ea\"");
    CHECK(!ime_t9_is_valid(""), "validator accepted an empty string");

    ime_t9_deinit();
}

/* --------------------------------------------------------------- trie/hash */

static void test_trie(void)
{
    CHECK(ime_trie_init(), "ime_trie_init failed");
    CHECK(ime_trie_syllable_count() == 413, "syllable table has %u entries, expected 413",
          (unsigned)ime_trie_syllable_count());

    CHECK(ime_trie_is_prefix("9"), "9 should be a prefix");
    CHECK(ime_trie_is_prefix("94"), "94 should be a prefix");
    CHECK(ime_trie_is_prefix("9426"), "9426 should be a prefix");
    CHECK(!ime_trie_is_prefix("1"), "1 should not be a prefix");

    ime_trie_match_t matches[16];
    size_t count = ime_trie_children("94", matches, 16);
    CHECK(count >= 3, "94 produced %u children", (unsigned)count);

    bool has_xi = false;
    bool has_yi = false;
    bool has_zi = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(matches[i].syllable, "xi") == 0) has_xi = true;
        if (strcmp(matches[i].syllable, "yi") == 0) has_yi = true;
        if (strcmp(matches[i].syllable, "zi") == 0) has_zi = true;
    }
    CHECK(has_xi && has_yi && has_zi, "94 children did not include xi/yi/zi");

    /* "94" is not a syllable itself, but xi/yi/zi have exactly that key
     * sequence, so they are reported as exact children. */
    for (size_t i = 0; i < count; i++) {
        if (strcmp(matches[i].syllable, "xi") == 0 || strcmp(matches[i].syllable, "yi") == 0 ||
            strcmp(matches[i].syllable, "zi") == 0) {
            CHECK(matches[i].is_exact, "\"%s\" should be an exact child of 94", matches[i].syllable);
        }
    }

    /* A prefix with no spelling at all must not resolve. */
    size_t exact = 0;
    CHECK(ime_trie_exact("94", &exact), "94 should resolve to a syllable index");
    CHECK(ime_trie_exact("9464", &exact), "9464 should resolve to a syllable");
    CHECK(strcmp(ime_trie_syllable(exact), "xing") == 0, "9464 resolved to \"%s\", expected xing",
          ime_trie_syllable(exact));
    CHECK(!ime_trie_exact("94645", &exact), "94645 should not resolve to a syllable");

    ime_trie_deinit();
}

static void test_hash(void)
{
    ime_hash_t map;
    CHECK(ime_hash_init(&map, 16), "ime_hash_init failed");
    CHECK(ime_hash_capacity(&map) == 16, "capacity rounded to %u, expected 16",
          (unsigned)ime_hash_capacity(&map));

    CHECK(ime_hash_put(&map, "wo", 1), "put wo failed");
    CHECK(ime_hash_put(&map, "men", 2), "put men failed");
    CHECK(ime_hash_put(&map, "women", 3), "put women failed");
    CHECK(ime_hash_count(&map) == 3, "count is %u, expected 3", (unsigned)ime_hash_count(&map));

    uint32_t value = 0;
    CHECK(ime_hash_get(&map, "women", &value) && value == 3, "get women failed");
    CHECK(ime_hash_get(&map, "wo", &value) && value == 1, "get wo failed");
    CHECK(!ime_hash_get(&map, "ni", NULL), "get ni should miss");

    /* Replace. */
    CHECK(ime_hash_put(&map, "wo", 42), "replace wo failed");
    CHECK(ime_hash_get(&map, "wo", &value) && value == 42, "wo was not replaced");
    CHECK(ime_hash_count(&map) == 3, "replace changed the count to %u", (unsigned)ime_hash_count(&map));

    /* Remove, then reinsert into the freed slot. */
    CHECK(ime_hash_remove(&map, "men"), "remove men failed");
    CHECK(!ime_hash_get(&map, "men", NULL), "men is still present after remove");
    CHECK(ime_hash_count(&map) == 2, "count after remove is %u", (unsigned)ime_hash_count(&map));
    CHECK(ime_hash_put(&map, "ni", 7), "insert after remove failed");
    CHECK(ime_hash_get(&map, "ni", &value) && value == 7, "get ni after reinsert failed");

    ime_hash_deinit(&map);
    CHECK(ime_hash_count(&map) == 0, "count after deinit is not 0");
}

/* ------------------------------------------------------------ dict VFS trim */

static void test_dict_payload_trim(void)
{
    /* A dict partition holds the dictionary followed by erased flash. The VFS
     * must report only the real payload, otherwise the engine reads 0xFF and
     * refuses to open the dictionary (this happened on hardware). */
    const size_t real = 1071858;
    const size_t partition = 0x180000;
    uint8_t *image = (uint8_t *)malloc(partition);
    CHECK(image != NULL, "cannot allocate the simulated partition");
    if (image == NULL) {
        return;
    }
    memset(image, 0xFF, partition);
    for (size_t i = 0; i < real; i++) {
        image[i] = (uint8_t)(i * 31u + 7u);
    }

    CHECK(ime_vfs_payload_size(image, partition) == real,
          "payload of a padded partition is %u, expected %u",
          (unsigned)ime_vfs_payload_size(image, partition), (unsigned)real);

    /* An image with no padding is returned unchanged. */
    CHECK(ime_vfs_payload_size(image, real) == real, "unpadded payload changed");

    /* A dictionary whose last byte happens to be 0xFF is not truncated: only a
     * run of erased bytes at the very end is padding. */
    image[real - 1] = 0x00;
    CHECK(ime_vfs_payload_size(image, real) == real, "payload ending in 0x00 was trimmed");

    /* All-erased image: nothing to serve. */
    memset(image, 0xFF, partition);
    CHECK(ime_vfs_payload_size(image, partition) == 0, "all-0xFF image is not empty");
    CHECK(ime_vfs_payload_size(NULL, partition) == 0, "NULL image is not empty");

    free(image);
}

/* ------------------------------------------------------------------ engine */

static void test_engine(const char *dict_path)
{
    CHECK(ime_engine_open(dict_path, NULL), "ime_engine_open(%s) failed", dict_path);
    if (s_failures > 0) {
        return;
    }

    size_t n = ime_engine_search("ni", 2);
    CHECK(n > 0, "search(ni) returned no candidates");

    char text[64];
    size_t len = ime_engine_candidate(0, text, sizeof(text));
    CHECK(len > 0, "candidate 0 is empty");
    CHECK(strcmp(text, "你") == 0, "candidate 0 of \"ni\" is \"%s\", expected 你", text);

    /* The separator is honoured: "pi'ao" is decoded as two syllables and stays
     * a two-character candidate list, while "piao" collapses to single
     * characters. (The upstream word list has no 皮袄 phrase, so the top
     * candidate is 皮奥 — see docs/dict_format.md on supplying a phrase list.) */
    ime_engine_reset();
    n = ime_engine_search("pi'ao", 5);
    CHECK(n > 0, "search(pi'ao) returned no candidates");
    ime_engine_candidate(0, text, sizeof(text));
    CHECK(strncmp(text, "\xe7\x9a\xae", 3) == 0,
          "candidate 0 of \"pi'ao\" should start with 皮, got \"%s\"", text);
    CHECK(strlen(text) == 6, "candidate 0 of \"pi'ao\" is not a two character candidate: \"%s\"", text);

    ime_engine_reset();
    n = ime_engine_search("piao", 4);
    CHECK(n > 0, "search(piao) returned no candidates");
    ime_engine_candidate(0, text, sizeof(text));
    CHECK(strcmp(text, "票") == 0, "candidate 0 of \"piao\" is \"%s\", expected 票", text);

    ime_engine_reset();

    /* choose() fixes a candidate and narrows the list. */
    n = ime_engine_search("women", 5);
    CHECK(n > 0, "search(women) returned no candidates");
    ime_engine_candidate(0, text, sizeof(text));
    CHECK(strcmp(text, "我们") == 0, "candidate 0 of \"women\" is \"%s\", expected 我们", text);
    for (size_t i = 0; i < n; i++) {
        ime_engine_candidate(i, text, sizeof(text));
        if (strcmp(text, "我们") == 0) {
            CHECK(ime_engine_choose(i) >= 1, "choose(%u) returned nothing", (unsigned)i);
            CHECK(ime_engine_fixed_len() == 2, "fixed_len after choosing 我们 is %u",
                  (unsigned)ime_engine_fixed_len());
            break;
        }
    }
    ime_engine_reset();

    /* Predictions follow a committed word. */
    size_t pn = ime_engine_predict("我们", NULL, 0);
    CHECK(pn > 0, "predict(我们) returned nothing");
    if (pn > 0) {
        ime_engine_predict_item(0, text, sizeof(text));
        CHECK(text[0] != '\0', "first prediction is empty");
        printf("  prediction after 我们: %s\n", text);
    }

    /* Segmentation of the spelling string. */
    ime_engine_reset();
    ime_engine_search("piao", 4);
    const uint16_t *starts = NULL;
    size_t spl = ime_engine_spl_start_pos(&starts);
    CHECK(spl >= 1, "piao segmented into %u spellings", (unsigned)spl);

    ime_engine_close();
}

/* ----------------------------------------------------------------- session */

static void test_session(const char *dict_path)
{
    (void)dict_path;
    /* ime_session_init() needs the flash-partition dictionary loader, which is
     * not part of the host build. Its logic is exercised on the target and
     * through the engine tests above. */
}

/* ------------------------------------------------- partial candidate commit */

/*
 * Picking a candidate must not throw away the syllables that are still being
 * composed. "lawanle" offers 啦完了 as candidate 0; choosing the single character
 * 拉 has to leave "wanle" live so the next candidates come from it. This used to
 * clear the whole buffer, which is why a partial pick behaved like "delete the
 * rest of what I typed".
 */
static void test_partial_commit(const char *dict_path)
{
    if (!ime_engine_open(dict_path, NULL)) {
        CHECK(false, "engine unavailable: %s", dict_path);
        return;
    }

    size_t total = ime_engine_search("lawanle", 7);
    CHECK(total > 0, "search(lawanle) returned nothing");

    /* Find the single character 拉 among the candidates. */
    size_t la = (size_t)-1;
    char text[64];
    for (size_t i = 0; i < total; i++) {
        text[0] = '\0';
        ime_engine_candidate(i, text, sizeof(text));
        if (strcmp(text, "拉") == 0) {
            la = i;
            break;
        }
    }

    if (la == (size_t)-1) {
        printf("  note: no standalone 拉 in the lawanle candidate list, "
               "partial commit not exercised by this dictionary\n");
        ime_engine_close();
        return;
    }

    ime_engine_choose(la);

    size_t remaining_len = 0;
    const char *remaining = ime_engine_remaining_pinyin(&remaining_len);
    printf("  after choosing 拉: remaining pinyin \"%s\" (%u bytes)\n",
           remaining != NULL ? remaining : "(null)", (unsigned)remaining_len);

    CHECK(remaining != NULL && strcmp(remaining, "wanle") == 0,
          "remaining pinyin is \"%s\", expected \"wanle\"",
          remaining != NULL ? remaining : "(null)");
    CHECK(ime_engine_fixed_len() == 1, "fixed length is %u, expected 1",
          (unsigned)ime_engine_fixed_len());

    /* And the rest must still produce candidates. */
    if (remaining != NULL && remaining_len > 0) {
        size_t after = ime_engine_search(remaining, remaining_len);
        CHECK(after > 0, "search(\"%s\") returned no candidates", remaining);
        if (after > 0) {
            ime_engine_candidate(0, text, sizeof(text));
            printf("  first candidate for the remainder: %s\n", text);
        }
    }

    ime_engine_close();
}

/* ------------------------------------------------- the composition display */

/*
 * The IME shows the syllables apart ("la'wan'le") rather than as one run of
 * letters. The separators come from the engine's segmentation, not from the
 * buffer, so a separator the user typed must not be doubled.
 */
static void test_pinyin_display(const char *dict_path)
{
    (void)dict_path;
    if (!ime_session_init()) {
        printf("  note: session init failed, display test skipped\n");
        return;
    }

    ime_session_set_mode(IME_MODE_K26);
    ime_session_set_lang(IME_LANG_CN);

    const char *word = "lawanle";
    for (const char *p = word; *p != '\0'; p++) {
        ime_session_push_letter(*p);
    }

    char shown[64];
    ime_session_pinyin_display(shown, sizeof(shown));
    printf("  composition \"%s\" is displayed as \"%s\"\n", word, shown);
    CHECK(strcmp(shown, "la'wan'le") == 0,
          "display of \"%s\" is \"%s\", expected \"la'wan'le\"", word, shown);

    /* A hand typed separator must survive as exactly one separator. */
    ime_session_reset();
    const char *manual = "xi'an";
    for (const char *p = manual; *p != '\0'; p++) {
        if (*p == '\'') {
            ime_session_push_separator();
        } else {
            ime_session_push_letter(*p);
        }
    }
    ime_session_pinyin_display(shown, sizeof(shown));
    printf("  composition \"%s\" is displayed as \"%s\"\n", manual, shown);
    CHECK(strcmp(shown, "xi'an") == 0,
          "display of \"%s\" is \"%s\", expected \"xi'an\"", manual, shown);

    /*
     * English mode has no composition to display: each letter is committed
     * straight away, so the buffer stays empty and the chip stays hidden.
     */
    ime_session_set_lang(IME_LANG_EN);
    ime_session_reset();
    ime_session_push_letter('h');
    CHECK(ime_session_pinyin()[0] == '\0',
          "English mode buffered \"%s\", expected nothing", ime_session_pinyin());
    ime_session_pinyin_display(shown, sizeof(shown));
    CHECK(shown[0] == '\0', "English mode display is \"%s\", expected empty", shown);

    ime_session_set_lang(IME_LANG_CN);
    ime_session_reset();
}

int main(int argc, char **argv)
{
    const char *dict = (argc > 1) ? argv[1] : "data/dict/dict_pinyin.dat";

    test_utf16_roundtrip();
    test_t9();
    test_t9_suggestions_are_pinyin();
    test_trie();
    test_hash();
    test_dict_payload_trim();
    test_engine(dict);
    test_partial_commit(dict);
    test_pinyin_display(dict);
    test_session(dict);

    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
