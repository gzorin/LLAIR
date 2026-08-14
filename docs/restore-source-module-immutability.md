# A1 — Restore source-module immutability

## Context

`docs/linker-permutation-plan.md` lays out a two-series plan (linker, metallib writer) for the permutation pipeline. B1 already landed. Series A is next, and A1 is the milestone every later milestone (A2–A6, and B3's per-entry cloning) depends on: `lib/Linker/Linker.cpp`'s `Linker::linkModule` currently mutates *distinct* metadata nodes in the source module in place while linking it into a destination, via `RF_ReuseAndMutateDistinctMDs` (LLVM ≥13) / `RF_MoveDistinctMDs` (pre-13). Distinct nodes are most of debug info. This is harmless for a "link once, discard" workload but corrupts the source when the same module is relinked into hundreds of permutation destinations — exactly Bourbon's actual workload. The doc has already decided the fix (`RF_None` in both branches, i.e. clone distinct nodes instead of mutating them) and flagged three related omissions to fix in the same pass. This plan implements that decision and adds the regression test the doc's gate calls for.

## Code changes — `lib/Linker/Linker.cpp`

In `Linker::linkModule`, the per-global-variable metadata loop (~line 382-389):

```cpp
#if LLVM_VERSION_MAJOR >= 13
            GV->addMetadata(MD.first, *MapMetadata(MD.second, VMap, RF_ReuseAndMutateDistinctMDs, TMap.get()));
#else
            GV->addMetadata(MD.first, *MapMetadata(MD.second, VMap, RF_MoveDistinctMDs, &TMap));
#endif
```

becomes `RF_None` in both branches (per the doc: "Both branches use `RF_None`. Distinct nodes get cloned."). This is the actual bug fix — everything else below is the doc's audit note ("`MapValue` for the alias aliasee and the personality function omit `TMap.get()`... aliases are created with `I->getValueType()` rather than `TMap->remapType(...)`"), three small omissions of the type remapper found while auditing this loop:

- Alias creation (~line 358): `GlobalAlias::create(I->getValueType(), ...)` → `GlobalAlias::create(TMap->remapType(I->getValueType()), ...)`.
- Personality function (~line 416): `MapValue(I.getPersonalityFn(), VMap)` → `MapValue(I.getPersonalityFn(), VMap, RF_None, TMap.get())`.
- Alias aliasee (~line 429): `MapValue(C, VMap)` → `MapValue(C, VMap, RF_None, TMap.get())`.

Nothing else in the file changes. The `&TMap` type mismatch in the pre-13 branch (dead code — the tree builds against LLVM 14.0.6, confirmed via `build-deps-macos/install/lib/cmake/llvm/LLVMConfigVersion.cmake`) is left alone; it's not part of A1's stated scope and touching it isn't requested by the doc.

## Regression test (the gate)

The doc's gate: *"Link module A into two separate destinations. A's `NamedMDNode` operands and `DISubprogram` scope pointers are identical before and after both links. Keep as a regression test — it protects every later milestone in both series."*

**Why a `GlobalVariable`, not a `Function`'s `DISubprogram`:** I checked `extsrc/llvm/llvm/lib/Transforms/Utils/CloneFunction.cpp` (vendored floor_llvm, read-only per the doc's no-fork constraint) — `CloneFunctionInto` already computes `RemapFlag = ModuleLevelChanges ? RF_None : RF_NoModuleLevelChanges` and the Linker's call passes `CloneFunctionChangeType::DifferentModule`, so function-level debug info (`DISubprogram` via `F->setSubprogram`) is *already* cloned correctly today and doesn't exercise A1's bug. The actual bug is isolated to the per-`GlobalVariable` metadata loop being patched above. `llvm::DIGlobalVariable` (wrapped in a `DIGlobalVariableExpression`, attached via `GlobalVariable::addDebugInfo`) is a distinct node with a `getScope()` pointer — the same shape the doc's "`DISubprogram` scope pointers" language is illustrating — and it goes through exactly the code path being fixed. This is the faithful way to exercise the gate.

**No existing test harness in `extsrc/llair`.** There's no `tests/` directory and no GoogleTest dependency here (unlike the parent Bourbon repo). The only executable resembling a test is `examples/command-line/test.cpp` → `llair-test`, currently a placeholder ("Hello, llair", no assertions). Per the parent CLAUDE.md's own convention ("plain `assert()`... elsewhere" when no gtest harness exists), I'll extend `test.cpp` with an `assert()`-based check rather than introducing a new test framework.

Plan for `examples/command-line/test.cpp`:
1. Build a source `llair::Module` "src" with a `GlobalVariable`.
2. Use `llvm::DIBuilder` on `src`'s underlying `llvm::Module` to create a `DIFile`, `DICompileUnit`, and a `DIGlobalVariableExpression` (distinct `DIGlobalVariable` + `DIExpression`); attach it via `GV->addDebugInfo(...)`; finalize the builder.
3. Record, before any linking: the `DIGlobalVariable`'s scope pointer (`DIVar->getScope()`) and the source module's `llvm.dbg.cu` `NamedMDNode`'s operand pointer.
4. Create two independent destination `llair::Module`s, `dst1` and `dst2`.
5. `linkModules(&dst1, &src)`, then assert both recorded pointers on `src` are unchanged (identity, not just value-equality — this is precisely what `RF_ReuseAndMutateDistinctMDs` would have violated).
6. `linkModules(&dst2, &src)`, assert again.

Wiring: `examples/command-line/CMakeLists.txt`'s `llair-test` target currently links only `LLAIR`; add `LLAIRLinker` (the target defined in `lib/Linker/CMakeLists.txt`) to `target_link_libraries`. No other build changes needed — `llvm_libs` already maps the `core` component, which covers `DIBuilder`.

## Verification

1. Reconfigure/build: `cmake --build --preset macos --target llair-test` (adjust target/preset name if `llair-test` isn't currently in the default build graph — confirm via the existing preset).
2. Run `./build-macos/.../llair-test` (path depends on where the example binary lands) and confirm it exits cleanly (asserts pass).
3. Sanity-check the test itself: temporarily revert the `RF_None` change locally, rebuild, confirm the new assertions actually fail (proving the test catches the bug), then reapply the fix. This is a local check during development, not a committed step.
4. Confirm the rest of `extsrc/llair` still builds (`llair-link`, `llair-metallib`, `llair-dump`, `metalc`, `make-library`) since `Linker.cpp` is shared across those tools.

This closes out A1 only. A2 (ODR dedup) and later milestones are explicitly out of scope for this change and can be picked up next once A1's gate is confirmed passing.
