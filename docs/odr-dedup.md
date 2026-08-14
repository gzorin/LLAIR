# A2 — ODR dedup for already-defined symbols

## Context

A1 (source-module immutability) landed and its gate passed. Per `docs/linker-permutation-plan.md`, A2 is next: `Linker::linkModule` currently creates a brand-new `Function`/`GlobalVariable` for every definition in the source module, with no check for whether `dst` already holds one under that name. When it does — e.g. a Metal header's `linkonce_odr` inline function or constant table, which every source module that includes the header re-defines — LLVM's module symbol table silently renames the new one to `name.1` on insertion. Every source module relinked into the same `dst` duplicates that body again (`name.2`, `name.3`, ...). This is dead weight carried through the rest of the pipeline (and, per B3, cloned again per entry point). The doc gives a concrete code sketch for the function case and says globals get "the same treatment."

## Code changes — `lib/Linker/Linker.cpp`

Add a free function next to the existing `copyComdat` helper in the anonymous namespace:

```cpp
constexpr bool
isODR(llvm::GlobalValue::LinkageTypes L) {
    return L == llvm::GlobalValue::LinkOnceODRLinkage ||
           L == llvm::GlobalValue::WeakODRLinkage;
}
```

**Function definitions loop** (currently ~339-354). Replace with the doc's sketch, extended with the decided `available_externally` exception:

```cpp
SmallVector<std::pair<const Function *, Function *>, 32> pending_bodies;

for (const Function &I : *M) {
    if (I.isDeclaration()) {
        continue;
    }

    Function *NF = New->getFunction(I.getName());

    if (NF && !NF->isDeclaration()) {
        if (NF->getLinkage() == GlobalValue::AvailableExternallyLinkage) {
            // dst's body is a stand-in; src's real definition wins.
            NF->deleteBody();
        }
        else {
            // ODR: bodies are equivalent, keep dst's. Anything src's body
            // reached is already reachable through dst's.
            assert(isODR(I.getLinkage()) && isODR(NF->getLinkage()));
            VMap[&I] = NF;
            continue;                       // no copyAttributesFrom: would stomp dst's
        }
    }

    if (!NF) {
        NF = Function::Create(cast<FunctionType>(TMap->remapType(I.getValueType())),
                              I.getLinkage(), I.getName(), New);
    }

    NF->copyAttributesFrom(&I);
    VMap[&I] = NF;
    pending_bodies.emplace_back(&I, NF);
}
```

A non-ODR, non-`available_externally` collision hits the `assert` — a genuine duplicate-symbol error, not silently renamed. (Matches the doc's "implementer's choice": assert, consistent with this file having no `Expected`-style error path anywhere today.)

**Function body-copy loop** (currently ~396-423). Iterate `pending_bodies` instead of re-scanning `*M` and re-doing `VMap[&I]`:

```cpp
for (auto &[I, F] : pending_bodies) {
    Function::arg_iterator DestI = F->arg_begin();
    for (auto J = I->arg_begin(); J != I->arg_end(); ++J) {
        DestI->setName(J->getName());
        VMap[&*J] = &*DestI++;
    }

    SmallVector<ReturnInst *, 8> Returns;
    CloneFunctionInto(F, I, VMap, CloneFunctionChangeType::DifferentModule, Returns, "", nullptr, TMap.get());
    // (pre-13 branch unchanged in shape, just sourced from I/F instead of &I/VMap[&I])

    if (I->hasPersonalityFn())
        F->setPersonalityFn(MapValue(I->getPersonalityFn(), VMap, RF_None, TMap.get()));

    copyComdat(F, I);

    if (F->hasSection() && F->getSection() == "air.static_init") {
        appendToGlobalCtors(*New, F, 65535);
    }
}
```

**Global-variable skeleton loop** (currently ~288-312) — same treatment, applied only to the non-declaration (definition) case, leaving the existing declaration-resolution branch (`src_to_dst_global_value_map`) untouched:

```cpp
SmallVector<std::pair<const GlobalVariable *, GlobalVariable *>, 32> pending_globals;

for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I) {
    if (I->getName() == "llvm.global_ctors") continue;

    if (I->isDeclaration()) {
        // unchanged: existing src_to_dst_global_value_map lookup, then
        // create-if-absent, then `VMap[&*I] = GV; continue;`
    }

    GlobalVariable *GV = New->getGlobalVariable(I->getName());

    if (GV && !GV->isDeclaration()) {
        if (GV->getLinkage() == GlobalValue::AvailableExternallyLinkage) {
            GV->setInitializer(nullptr);        // src's real definition wins
        }
        else {
            assert(isODR(I->getLinkage()) && isODR(GV->getLinkage()));
            VMap[&*I] = GV;
            continue;                            // no copyAttributesFrom: would stomp dst's
        }
    }
    else {
        GV = new GlobalVariable(*New, TMap->remapType(I->getValueType()), I->isConstant(),
                                I->getLinkage(), (Constant *)nullptr, I->getName(),
                                (GlobalVariable *)nullptr, I->getThreadLocalMode(),
                                I->getType()->getAddressSpace());
    }

    GV->copyAttributesFrom(&*I);
    VMap[&*I] = GV;
    pending_globals.emplace_back(&*I, GV);
}
```

`Module::getGlobalVariable(name)` (default `AllowInternal=false`) is the global analogue of `Module::getFunction(name)` already used for functions — it only matches non-internal-linkage globals, which is exactly the ODR/`available_externally` population.

**Global initializer/metadata loop** (currently ~368-392): iterate `pending_globals` instead of re-scanning `*M` (the `llvm.global_ctors`/declaration skips fall away — the pending list only contains real definitions that need it).

## Regression test — `examples/command-line/test.cpp`

Same harness as A1 (`assert()`-based, no gtest here). Two new functions:

**`testLinkerDedupesODRDefinitions`** — the doc's literal gate. Build two source modules that both define a `linkonce_odr` function `helper` (simulating a shared header) plus one module-unique function each (`unique1`, `unique2`); do the same with a `linkonce_odr` global to cover "same treatment for globals". Link both into one `dst`, then assert:
- `dst->getFunction("helper.1") == nullptr` (no `.N` rename),
- function count in `dst` equals the union of distinct names (3: `helper`, `unique1`, `unique2`),
- same two checks for the global,
- `llvm::verifyModule(*dst.getLLModule())` returns clean.

**`testLinkerPrefersRealDefinitionOverAvailableExternally`** — covers the decided exception not in the doc's code sketch. Link a module defining `f` as `available_externally` into `dst` first, then a module defining `f` as a real (`external`) definition. Assert `dst`'s `f` ends up with `ExternalLinkage` (not renamed, not left `available_externally`) and is non-empty.

## Verification

1. `cmake --build --preset macos --target llair-test` from `extsrc/llair`, run `./build-macos/examples/command-line/llair-test`.
2. Sanity-check discriminating power the same way as A1: temporarily revert the dedup branch to unconditional `Function::Create`/`new GlobalVariable`, confirm the new asserts fail, then restore.
3. Full `cmake --build --preset macos` to confirm `llair-link`, `llair-metallib`, `llair-dump`, `metalc`, `make-library`, `llair-triangle` still build clean.
4. Update `docs/linker-permutation-plan.md`'s A2 section with a "DONE" marker and gate result, matching the A1/B1 precedent.
