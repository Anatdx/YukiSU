# Remaining clang-tidy fixes (YukiSU ksud)

Build with: `orb run -p -w /Volumes/Workspace/YukiSU/userspace/ksud sh -c 'ninja -C build 2>&1'`

## Done in this session
- **boot/tools.cpp**: empty catch NOLINT
- **core/hide_bootloader.cpp**: anonymous namespace for helpers, const, `std::endl` → `\n`
- **init_event.cpp**: anonymous namespace for helpers, const
- **flash/flash_partition.cpp**: anonymous namespace for helpers, const, unused `result` → (void)exec_cmd
- **hymo/hymo_cli.cpp**: `const int argc`
- **hymo/utils.cpp**: anonymous namespace (3 blocks) for setup_loop_device, native_cp_r, normalize_path_string, is_dangerous_temp_path, ksu_fd/ksu_checked; const for locals
- **hymo/conf/config.cpp**: const for default_path, json_str, root, mode_file, rules_file; NOLINT for ifstream, empty catch
- **hymo/core/executor.cpp**: anonymous namespace for extract_id, extract_module_root; reserve() for vector; const for root, id, candidate
- **hymo/core/inventory.cpp**: anonymous namespace for parse_module_prop, parse_module_rules, is_mountpoint; const for prop_file, eq, key, value, rules_file, id, name, root_path_str; empty catch NOLINT

## Remaining (by check type)
- **misc-const-correctness** (~190): Add `const` to locals that are never reassigned (e.g. `std::string x = ...` → `const std::string x = ...`).
- **misc-use-anonymous-namespace** (~43): Replace `static` functions/vars with `namespace { ... }` in the same file.
- **bugprone-empty-catch** (5): Add `// NOLINT(bugprone-empty-catch)` and a short comment, or handle the exception.
- **performance-avoid-endl** (3): Use `'\n'` or `"\n"` instead of `std::endl`.
- **performance-enum-size** (2): Use a smaller enum base type if desired (e.g. `std::uint8_t`).
- **readability-static-definition-in-anonymous-namespace** (1): Remove `static` from a symbol already in an anonymous namespace.
- **readability-suspicious-call-argument** (1), **bugprone-easily-swappable-parameters** (1): Fix or NOLINT as appropriate.

## Files still with errors (typical)
- `src/hymo/core/planner.cpp` (if any remain)
- `src/hymo/core/state.cpp` (if any remain)
- `src/hymo/core/modules.cpp` (if any remain)
- `src/hymo/hymo_main.cpp`: anonymous namespace + const + endl + enum NOLINT applied; IDE may still report defs.hpp / umount (build env).

## Patterns to apply
1. **Const**: For each diagnostic “variable 'X' can be declared 'const'”, change the declaration to `const Type X = ...`.
2. **Static → anonymous namespace**: Add `namespace {` after `namespace hymo {`, move all `static` function definitions into it, remove `static`, add `}  // namespace` before the next public function.
3. **Streams**: For `std::ifstream`/`std::ofstream` “can be declared const”, add `// NOLINT(misc-const-correctness)` (streams are not meaningfully const).
4. **Empty catch**: Add `// NOLINT(bugprone-empty-catch)` and a one-line comment explaining why it’s intentional.
5. **std::endl**: Replace with `'\n'` or `"\n"`.
