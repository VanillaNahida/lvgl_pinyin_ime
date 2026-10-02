# lvgl_pinyin_ime

Chinese pinyin input method (IME) component for **LVGL 9.5** on **ESP-IDF 6.1**.

- Engine: [libgooglepinyin](https://salsa.debian.org/input-method-team/libgooglepinyin) (Apache-2.0)
- Keyboards: 26-key full keyboard + 9-key (T9), plus a shared digit / symbol panel set
- Input: full pinyin, double pinyin (six schemes, Xiaohe by default), phrases, whole sentences
- Features: candidate paging, syllable separator `'`, association, user frequency learning, fuzzy pinyin (off by default)
- Targets: ESP32-S3-N16R8 (primary), ESP32-S31, ESP32-P4
- Reference board: ST7789 (SPI, 320 x 240 landscape) + XPT2046 touch

Start with [docs/usage.md](docs/usage.md) if you just want to build, flash and
run the example.

## Status

| Stage | Content | State |
| --- | --- | --- |
| S-1 | Design documents (`docs/`) | done |
| S0 | Project skeleton, example app, three-target build | done |
| S1 | libgooglepinyin port + engine facade + dictionary loading | **done** |
| S2 | Python toolchain (dictionary / charset / font) | **done** (ext dictionary lands with S5) |
| S3 | UI: root, candidate bar, 26-key, panels | **done** |
| S4 | 9-key: T9 buffer, trie, hash | **done** |
| S5 | Double pinyin, fuzzy, association, user dictionary | pending |
| S6 | Host unit tests, docs, three-target regression | in progress |

Two verification paths work today, neither needs hardware:

```powershell
# Engine, T9, trie and hash unit tests on the host (64 assertions)
cmake -S test_apps/host_core_test -B build/host_core_test -G Ninja
cmake --build build/host_core_test
.\build\host_core_test\host_core_test.exe data/dict/dict_pinyin.dat

# Component compile check for a target
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd test_apps\idf_component_build
idf.py set-target esp32s3 ; idf.py build
```

The dictionary is rebuilt from the vendored upstream word list instead of being
downloaded: `python tools/gen_dict.py` (see [docs/usage.md](docs/usage.md)).

## Layout

```
include/lvgl_pinyin_ime/   public API (LVGL widget + LVGL free engine facade)
src/ui/                    LVGL widgets
src/core/                  session state machine and engine helpers
src/data/                  dictionary, user dictionary, font loading
src/third_party/           vendored libgooglepinyin (S1)
partitions/                partition tables (16 MB / 8 MB)
docs/                      usage, keymap, architecture, integration, dict format
examples/k26_k9_demo/      buildable example: ST7789 + XPT2046 + IME host
tools/                     host side Python toolchain (gen_dict, gen_t9_table)
test_apps/host_core_test/  host unit tests for the LVGL-free modules
test_apps/idf_component_build/  compile check for a target, no hardware needed
```

## Quick start

**The repository root is a component, not a project** — `idf.py` has to run from a
project directory, otherwise CMake tries to configure the repo root as a project
and fails with host compiler errors.

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd examples\k26_k9_demo          # <- the project, NOT the repository root
idf.py set-target esp32s3
idf.py -p COM17 flash monitor
```

The first build downloads LVGL 9.5 into `managed_components/`.
ESP32-S31 is still a preview target: `idf.py --preview set-target esp32s31`.

`idf.py flash` writes the bootloader, the partition table and the application.
The application carries the CJK fonts as C arrays (nothing to flash for the
keyboard, candidates or committed Chinese text) and a 1 MB fallback copy of the
dictionary. The optional `dict` and `font` partitions are flashed separately, and
the fonts only need the `font` partition if you switch
`CONFIG_LV_PINYIN_IME_FONT_SRC` from `EMBEDDED` to `PARTITION`:

```powershell
python tools\gen_dict.py          # data/dict/dict_pinyin.dat
python tools\gen_font.py          # generated/lv_font_ime_*.c (embedded, default)
python tools\gen_font.py --format bin   # generated/font_partition.bin (partition mode)
powershell -ExecutionPolicy Bypass -File tools\flash_dict.ps1 -Port COM17 -Dict -Font
```

The demo prints a widget dump at start-up (`lv_pinyin_ime_dump`) and an error if
the engine is missing, so the monitor output answers "why is the keyboard not
there".

The example now shows the full keyboard: candidate bar with the floating pinyin
chip, the 26-key keyboard (plus the P1/P2/P3 symbol panels) and the 9-key
keyboard with its candidate pinyin row. Keys are rounded squares
(`IME_KEY_RADIUS` in `src/ui/lv_pinyin_ime_style.c`). The IME reads its dictionary
from the `dict` flash partition and falls back to a copy linked into the
application, so it works before any partition has been flashed; flash the 1 MB
dictionary with `tools/flash_dict.ps1` to drop the fallback.

The CJK fonts are compiled into the application, so the keyboard, the candidate
bar and committed text all show real glyphs on a freshly flashed board - no font
partition needed. `tools/gen_font.py` post-processes `lv_font_conv`'s LVGL 8
output into the LVGL 9 struct layout; see
[docs/integration.md](docs/integration.md) section 5 for what it patches and why.

## Documentation

- [docs/usage.md](docs/usage.md) - environment, wiring, build/flash, touch calibration
- [docs/keymap.md](docs/keymap.md) - key layout and per-key behaviour
- [docs/architecture.md](docs/architecture.md) - layering, data flow, port patches
- [docs/integration.md](docs/integration.md) - component usage, partitions, fonts, SD dictionary
- [docs/dict_format.md](docs/dict_format.md) - binary formats for dictionary and font images

## License

Apache-2.0. See [LICENSE](LICENSE). The vendored libgooglepinyin keeps its own
Apache-2.0 license and upstream attribution under `src/third_party/`.