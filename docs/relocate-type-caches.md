# A3 — Relocate type caches to `LLAIRContextImpl`

## Context

A1 and A2 landed and their gates passed. Per `docs/linker-permutation-plan.md`, A3 is next: `Linker::TypeMapper` currently owns its type-remap cache (`d_type_map`) and struct-canonicalization map (`d_opaque_struct_type_map`) as instance state, but a fresh `TypeMapper` is constructed (and its maps discarded) on every single `linkModules()` call — confirmed by reading the only three call sites in this repo (`llair-link.cpp`, `llair-metallib.cpp`, `finalizeInterfaces`'s inner link): each always builds `dst` fresh and links sources into it one at a time via separate `linkModules()` calls, so every call pays for a brand-new, empty cache. Both maps are keyed on `llvm::Type *`/struct identifier, which are context-owned and immortal — nothing about them is specific to which `dst` is being filled. The fix is to relocate them to `LLAIRContextImpl`, which outlives every `Linker` for the life of the permutation-building session.

Per your answer: `dst` is always freshly built in real usage, never loaded from bitcode, so `updateIdentifiedOpaqueStructTypes`'s eager `TypeFinder` sweep over `dst` (currently re-run on every `linkModule` call, O(N²) over N sources) can be dropped entirely once the canonicalization map is context-scoped — every opaque struct type that can ever appear in `dst` arrives via this same `TypeMapper::remapType`, which already records it into the (now-persistent) canonical map the first time it's seen, from any source, in any prior link.

## Code changes

### `lib/IR/LLAIRContextImpl.h` — new context-owned type caches

Add alongside the existing `NameMap`/`ModuleMapType`/etc. accessor blocks:

```cpp
using TypeMapType            = llvm::DenseMap<llvm::Type *, llvm::Type *>;
using CanonicalStructMapType = llvm::StringMap<llvm::StructType *>;

TypeMapType&            remapped_types()    { return d_remapped_types; }
CanonicalStructMapType& canonical_structs() { return d_canonical_structs; }
```

with `d_remapped_types`/`d_canonical_structs` as new private members. Add `llvm::StructType`/`llvm::Type` to the existing forward-declare block, and `#include <llvm/ADT/DenseMap.h>` + `<llvm/ADT/StringMap.h>`.

### `lib/Linker/CMakeLists.txt` — cross-library include

`Linker.cpp` needs `LLAIRContextImpl.h` (private to `lib/IR`) to reach `LLAIRContextImpl::Get()`. Add `${CMAKE_SOURCE_DIR}/lib/IR` to `LLAIRLinker`'s `PRIVATE` include dirs. This is an intentional layering exception the doc's own target-file list calls for (it names `lib/IR/LLAIRContextImpl.h` as one of A3's touched files), not an accident.

### `lib/Linker/Linker.cpp`

**`Linker::TypeMapper`** — `d_type_map`/`d_opaque_struct_type_map` become references, not storage; constructor takes the owning `LLAIRContextImpl`:

```cpp
TypeMapper(llvm::LLVMContext &context, LLAIRContextImpl &context_impl)
    : d_context(context)
    , d_remapped_types(context_impl.remapped_types())
    , d_canonical_structs(context_impl.canonical_structs()) {
}
```

Member types become `LLAIRContextImpl::TypeMapType&` / `LLAIRContextImpl::CanonicalStructMapType&`.

**Drop `updateIdentifiedOpaqueStructTypes` entirely** — delete the method and its call site at the top of `linkModule` (`TMap->updateIdentifiedOpaqueStructTypes(New);`). Per your confirmation, nothing populates a `dst`'s opaque struct types except this same `TypeMapper`, so the map never needs re-seeding from a `TypeFinder` sweep.

**`Linker::Linker`** constructor now resolves the context impl:

```cpp
Linker::Linker(Module &dst)
: TMap(new TypeMapper(dst.getLLContext(), LLAIRContextImpl::Get(dst.getContext())))
, d_dst(dst) {
}
```

**Retire the regex** (doc: "one per `LLAIRContext`, retiring it as a shared-mutable global" → superseded by the micro-work item "prefix/suffix parse instead of regex", so the end state has no regex at all). Delete `cxx_identifier_regex()`, `test_cxx_identifier_regex` (already flagged unused in the doc's correctness backlog, and it's a direct wrapper around the regex being removed), and the `#include <llvm/Support/Regex.h>`. Replace `match_cxx_identifier_regex` with a manual parse (rename to `canonicalStructIdentifier` since "regex" is no longer accurate):

```cpp
llvm::Optional<llvm::StringRef>
canonicalStructIdentifier(llvm::StringRef name) {
    llvm::StringRef rest = name;

    if (!rest.consume_front("struct.") && !rest.consume_front("class.")) {
        return llvm::None;
    }

    // LLVM appends a numeric ".N" suffix to disambiguate an identified
    // struct name colliding with one already in the context; strip it to
    // recover the shared identity.
    auto dot = rest.rfind('.');
    if (dot != llvm::StringRef::npos) {
        auto suffix = rest.substr(dot + 1);
        if (!suffix.empty() && suffix.find_first_not_of("0123456789") == llvm::StringRef::npos) {
            rest = rest.substr(0, dot);
        }
    }

    if (rest.empty() || !(std::isalpha((unsigned char)rest[0]) || rest[0] == '_')) {
        return llvm::None;
    }

    return rest;
}
```

**Fix the self-referential-struct infinite recursion** (doc's "Risk"): `remapType` must insert a placeholder before recursing into contained types, so a re-entrant lookup for the same `SrcTy` (reached while remapping its own fields) resolves instead of looping forever. Also do the micro-work in the same pass: `SmallVector<Type *, 8>` instead of `std::vector`, an early-out for true leaf types (zero contained types *and* not a named struct — a named opaque struct also reports zero contained types but must still go through canonicalization), and `try_emplace` instead of find-then-insert. Net shape (branch semantics unchanged from today, verified by tracing each case):

```cpp
llvm::Type *remapType(llvm::Type *SrcTy) override {
    auto it = d_remapped_types.find(SrcTy);
    if (it != d_remapped_types.end()) {
        return it->second;
    }

    auto SrcStructTy     = llvm::dyn_cast<llvm::StructType>(SrcTy);
    bool is_named_struct = SrcStructTy && SrcStructTy->hasName();

    if (!is_named_struct && SrcTy->getNumContainedTypes() == 0) {
        return d_remapped_types[SrcTy] = SrcTy;
    }

    // Placeholder breaks cycles in self-referential types.
    d_remapped_types[SrcTy] = SrcTy;

    llvm::Type *RemappedTy = SrcTy;

    if (is_named_struct && SrcStructTy->isOpaque()) {
        auto identifier = canonicalStructIdentifier(SrcStructTy->getName());
        if (identifier) {
            RemappedTy = d_canonical_structs.try_emplace(*identifier, SrcStructTy).first->second;
        }
    }
    else {
        llvm::SmallVector<llvm::Type *, 8> RemappedContainedTys(SrcTy->getNumContainedTypes(), nullptr);
        std::transform(SrcTy->subtype_begin(), SrcTy->subtype_end(), RemappedContainedTys.begin(),
                      [&](auto ContainedTy) { return remapType(ContainedTy); });

        if (is_named_struct) {
            RemappedTy = llvm::StructType::get(d_context, RemappedContainedTys, SrcStructTy->isPacked());
        }
        else if (!std::equal(SrcTy->subtype_begin(), SrcTy->subtype_end(), RemappedContainedTys.begin())) {
            switch (SrcTy->getTypeID()) { /* unchanged FunctionTyID/PointerTyID/StructTyID/ArrayTyID cases */ }
        }
    }

    d_remapped_types[SrcTy] = RemappedTy;
    return RemappedTy;
}
```

## Regression tests — `examples/command-line/test.cpp`

**`testLinkerCanonicalizesStructsAcrossSeparateLinks`** — the core A3 win. Manually create two opaque struct types in one `LLVMContext` named `struct.Foo` and `struct.Foo.1` (simulating what LLVM's own struct-type uniquing produces when two separately-parsed bitcode files collide on a name), each referenced by a pointer-typed global in its own source module. Link both sources into one `dst` via two *separate* `linkModules()` calls (so two independently-constructed `Linker`/`TypeMapper` objects). Assert both globals end up with the identical pointer type in `dst` — i.e., canonicalization survives across `Linker` instances with no `TypeFinder` re-sweep. I'll verify this actually discriminates (fails if the maps are per-instance instead of context-scoped) by temporarily reverting just the storage-vs-reference change and re-running.

**`testLinkerHandlesSelfReferentialStructTypes`** — build a source module with a genuinely self-referential struct (`%struct.Node = type { %struct.Node*, i32 }`) referenced by a global, link it, and assert the call returns (doesn't hang/stack-overflow) and the result passes `verifyModule`. I'll verify this test hangs/crashes without the placeholder fix.

Also rerun the existing A1/A2 tests unchanged — they're the closest available proxy for "linked IR bit-identical to the A2 baseline" without the real interactive-renderer corpus.

## Verification

1. `cmake --build --preset macos --target llair-test`, run it, confirm all five tests pass.
2. Sanity-check each new test's discriminating power by temporarily reverting its corresponding fix, rebuilding, and confirming failure, then restoring.
3. Full `cmake --build --preset macos` to confirm `llair-link`, `llair-metallib`, `llair-dump`, `metalc`, `make-library`, `llair-triangle` still build clean (the new cross-library include and the deleted `updateIdentifiedOpaqueStructTypes`/regex machinery touch shared code).
4. Update `docs/linker-permutation-plan.md`'s A3 section with a "DONE" marker and gate result: note that the second gate ("permutation *k* performs strictly fewer `TypeFinder` runs than *k−1*") is satisfied unconditionally and permanently — the function that ran `TypeFinder` no longer exists, so the count is zero from the first permutation onward, confirmed by your call that `dst` is never bitcode-loaded in real usage.
