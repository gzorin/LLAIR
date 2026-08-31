# llair permutation pipeline: linker and metallib writer

Target: `lib/Linker/Linker.cpp`, `include/llair/Linker/Linker.h`, `lib/IR/LLAIRContextImpl.h`, `lib/IR/Module.{h,cpp}`, `lib/Tools/MakeLibrary.cpp`.

Out of scope by decision: `MetalLibWriterPass.cpp` and the `Writer50` / `Writer140` vendored bitcode writers. See *Constraints*. **Amended for B3** (decided with the user): B3's core mechanism can only be implemented at `CloneModule`'s call site inside `MetalLibWriterPass.cpp`. That file's "Vendored patch" section is a narrow, self-contained, upstream-offerable exception to the no-fork policy below — a duplicated reachability walk, the `ShouldCloneDefinition` callback, and a `GlobalDCE` addition — distinct from the high-churn compatibility code (AIR version tables, `llvm.ident` spoofing) the policy protects.

## Premise

The pipeline is driven from an interactive renderer producing IR permutations — geometry, BXDF, pattern. Each permutation is linked (Series A) and then packaged into a `.metallib` (Series B) for `newLibraryWithData`. The cost model is not "build one program once" but "rebuild the same shapes hundreds of times as bindings change."

The two series share two components — a reachability walk and a permutation key — which is why they live in one document.

## Constraints

**Decided.** No modifications to vendored floor_llvm code. Bourbon tracks upstream, and the parts that matter most (bitcode versions the Metal driver accepts, AIR-to-Metal version tables, per-release `llvm.ident` spoofing) need ongoing upstream maintenance that a fork would forfeit. Every Series B change is on llair's side of the boundary, shaping what the writer receives so its expensive paths become no-ops.

**Decided.** The linker's stated virtue is that it does not destroy its source modules. A1 exists because that property is currently violated. Every later milestone depends on it holding.

## Conventions

- **Decided** — settled; changing it invalidates later milestones.
- **Implementer's choice** — deliberately unspecified.
- Code sketches are design, not paste targets. Invented APIs are tagged `[new]`.
- Each milestone has a gate. Gates are pass/fail against the preceding milestone's output, not judgment calls.

## Reversals and amendments

- **Re-enterable worklist, original justification withdrawn — withdrawal itself withdrawn (2026-08-14).** Earlier reasoning held that `finalizeInterfaces` created roots *after* linking, so a single upfront reachability pass could not be correct. That was withdrawn on the premise that `Class`/`Dispatcher`/`Interface` are dead experimental code. The premise is false — see A0, retracted: `finalizeInterfaces` is live and does call `linkModules` again after the initial link, from all three CLI tools' `main()`. The original justification stands: the root set is not fully knowable before the pull begins, so A5's re-enterable worklist is not just a nicety for interactive rebinding — it may be load-bearing.
- **Upfront inter-module dependency graph, not adopted.** Eager edge discovery walks every instruction of every registered module; lazy discovery walks only what is retained. The *symbol* index (A4) is eager because it is a `global_values()` pass with no instruction walking; edges stay lazy.
- **Permutation key moves from library level to entry-point level.** A6 originally keyed a whole linked module. B3 splits libraries to one entry point each, which makes the natural key the reachable subgraph feeding a single entry. Editing a BRDF then invalidates the fragment entry only. A6 is restated accordingly.

## Shared components

**Reachability walk.** Given a module and a root set, compute the transitively referenced `GlobalValue`s. Used by A5 (pull only what a permutation needs) and B3 (clone only what an entry point needs). Build once; the hazards below apply to both callers.

- `ConstantExpr` operands need recursive descent to reach `GlobalValue` leaves.
- Comdat groups pull as a unit.
- Metadata edges must not count as references. `air.kernel` names its function; if metadata rooted the walk, nothing would ever be dropped.

**Permutation key.** The hash of a sorted (variation point → chosen definition) binding set for one entry point's reachable subgraph. Used by A6 (skip the link) and B4 (reuse the bitcode blob and its SHA-256). Same key also feeds `MTLBinaryArchive`, which caches the driver compile across sessions — the one stage of the pipeline neither series can optimize directly.

---

# Series A — Linker

## A0 — Delete dead abstract-interface machinery — RETRACTED (2026-08-14)

*Retracted: the premise was checked against the source tree and is false.*

`Class`, `Dispatcher`, `Interface` are **not** dead. `Module::getAllInterfacesFromABI` is called from `main()` in all three CLI tools (`llair-metallib`, `llair-link`, `llair-dump`), feeding `finalizeInterfaces` (`lib/Linker/Linker.cpp:71-129`), which builds real `Dispatcher` objects and links a synthesized dispatcher module back into the output via a second `linkModules` call — i.e. it creates new roots *after* the initial link, in production tool paths. `InterfaceKeyInfo`/`d_interfaces` in `LLAIRContextImpl` are likewise exercised, via `Interface::get`.

Only `Module::getOrLoadClassFromABI` and `getOrLoadAllClassesFromABI` are actually unreferenced anywhere in the tree. Everything else this milestone proposed removing — `finalizeInterfaces`, `getAllInterfacesFromABI`, `parseClassPathAndMethodName`, `getSelfType`, `ItaniumDemangle`, `d_interfaces` — is live. Deleting just the two dead functions would be a much smaller, independent cleanup, not this milestone as scoped.

This retraction reopens the justification in *Reversals and amendments* below.

## A1 — Restore source-module immutability — DONE (2026-08-14)

`MapMetadata(..., RF_ReuseAndMutateDistinctMDs, ...)` in the global-variable loop remaps *distinct* MDNodes in place rather than cloning. Distinct nodes are most of debug info. The pre-13 branch uses `RF_MoveDistinctMDs`, the same behavior under the old name, so this has always been the case.

Consequence: after linking, the source module's debug metadata points at values in the destination. Harmless when a source is linked once and discarded; corrupting when the same source feeds hundreds of permutations, which is the workload.

**Decided.** Both branches use `RF_None`. Distinct nodes get cloned. The added cost is not negotiable — the alternative is that source reuse is unsound.

Audit in the same pass: `MapValue` for the alias aliasee and the personality function omit `TMap.get()`, so their types are not remapped; aliases are created with `I->getValueType()` rather than `TMap->remapType(...)`.

**Gate.** Link module A into two separate destinations. A's `NamedMDNode` operands and `DISubprogram` scope pointers are identical before and after both links. Keep as a regression test — it protects every later milestone in both series.

**Gate result.** Landed in `lib/Linker/Linker.cpp`: both `RF_ReuseAndMutateDistinctMDs`/`RF_MoveDistinctMDs` branches now pass `RF_None`; the three audited omissions (alias aliasee, personality function, alias creation's value type) now go through `TMap.get()` / `TMap->remapType(...)`. The gate is now a permanent regression test in `examples/command-line/test.cpp` (`testLinkerPreservesSourceModuleDebugInfo`), the only test entry point this library has (no gtest harness here, unlike the parent Bourbon repo). It builds a source module with a `GlobalVariable` carrying a `DIGlobalVariableExpression` (a distinct node with a scope pointer — function-level `DISubprogram` cloning already goes through LLVM's own `CloneFunctionInto`, confirmed via `extsrc/llvm`'s `CloneFunction.cpp` to already pass `RF_None` for cross-module clones, so it doesn't exercise this bug), links it into two independent destinations, and asserts both that the source's own metadata is untouched *and* — the part that actually discriminates, since nothing in this graph has an operand that changes under remapping — that each destination gets its own clone rather than sharing identity with the source or with each other. Verified the test fails (`dst1_gv_expr != gv_expr` assertion trips) with `RF_ReuseAndMutateDistinctMDs` reinstated, and passes with the fix. Wired `LLAIRLinker` (and the `transformutils` LLVM component it needs) into the `llair-test` build target. Full rebuild of `extsrc/llair` (`llair-test`, `llair-link`, `llair-metallib`, `llair-dump`, `metalc`, `make-library`, `llair-triangle`) succeeds with no new warnings.

## A2 — ODR dedup for already-defined symbols — DONE (2026-08-14)

When `dst` already holds a *definition* of the name, a second `Function` is created, the symbol table renames it to `name.1`, and the full body is cloned into it. Metal header-inlined `linkonce_odr` bodies duplicate once per source module. The global-variable loop has the same shape for `linkonce_odr` constant tables.

```cpp
// [new]
constexpr bool isODR(llvm::GlobalValue::LinkageTypes L) {
    return L == llvm::GlobalValue::LinkOnceODRLinkage ||
           L == llvm::GlobalValue::WeakODRLinkage;
}

for (const Function &I : *M) {
    if (I.isDeclaration()) continue;

    Function *NF = New->getFunction(I.getName());

    if (NF && !NF->isDeclaration()) {
        // ODR: bodies are equivalent, keep dst's. Anything src's body
        // reached is already reachable through dst's.
        assert(isODR(I.getLinkage()) && isODR(NF->getLinkage()));
        VMap[&I] = NF;
        continue;                       // no copyAttributesFrom: would stomp dst's
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

**Decided.** `available_externally` on the `dst` side is the exception — src's body wins. A non-ODR collision is a duplicate-symbol error, not a rename; **implementer's choice** whether that is an assert or an `expected`-style return.

`pending_bodies` (`SmallVector<std::pair<const Function *, Function *>, 32>`) also removes the `VMap` re-lookup in the body pass. Same treatment for globals.

**Gate.** Link two modules sharing a header. No `.N`-suffixed functions; function count equals the union of distinct names; `verifyModule` clean; rendered output unchanged.

**Gate result.** Landed in `lib/Linker/Linker.cpp`: added the `isODR` helper, and gave both the function-definitions loop and the global-variable skeleton loop the same shape — look up an existing non-declaration symbol by name in `dst`; if `available_externally`, clear its body/initializer and let src's real definition win; if ODR, assert and reuse dst's `GlobalValue` untouched (no `copyAttributesFrom`, which would stomp dst's); otherwise create as before. Both loops now build a `pending_bodies`/`pending_globals` list consumed by the later body/initializer-copy pass instead of re-scanning `*M` and re-doing the `VMap` lookup. Two permanent regression tests landed in `examples/command-line/test.cpp`: `testLinkerDedupesODRDefinitions` (two source modules sharing a `linkonce_odr` function and a `linkonce_odr` global, simulating a shared header; asserts no `.N`-suffixed names, exact union-of-distinct-names counts for both functions and globals, and a clean `verifyModule`) and `testLinkerPrefersRealDefinitionOverAvailableExternally` (an `available_externally` stand-in linked first, then a real definition; asserts dst ends up with the real linkage and body, not a rename). Verified both tests fail without the corresponding dedup branch (reverted each temporarily) and pass with it. Full rebuild of `extsrc/llair` succeeds with no new warnings.

## A3 — Relocate type caches out of `Linker` — DONE (2026-08-21)

`d_type_map` is keyed on `llvm::Type *`, context-owned and immortal. `d_opaque_struct_type_map` canonicalizes `struct.Foo.3` / `struct.Foo.7` to one identity. Neither depends on which `dst` is being filled, and both are discarded per `Linker`, i.e. per permutation.

```cpp
// [new] LLAIRContextImpl
using TypeMapType            = llvm::DenseMap<llvm::Type *, llvm::Type *>;
using CanonicalStructMapType = llvm::StringMap<llvm::StructType *>;

TypeMapType&            remapped_types()    { return d_remapped_types; }
CanonicalStructMapType& canonical_structs() { return d_canonical_structs; }
```

`Linker::TypeMapper` holds references, not storage. The `llvm::Regex` static moves here too — one per `LLAIRContext`, retiring it as a shared-mutable global.

Second change, same milestone: `updateIdentifiedOpaqueStructTypes(New)` runs `TypeFinder` over the *destination* at the top of every `linkModule`. `TypeFinder` visits every global, function, instruction and operand type, and `dst` grows with each link — O(N²) over N sources, with a regex match per name. `remapType` already populates the canonical map lazily, and the reused type is the source's own `StructType *` in the shared context, so entries stay valid as `dst` grows.

**Decided.** The call moves to the `Linker` constructor. **Implementer's choice** whether it can be dropped entirely once the map is context-scoped; verify empirically.

Micro-work, same milestone: `SmallVector<Type *, 8>` for `RemappedContainedTys`; early-out at `getNumContainedTypes() == 0`; `try_emplace` in place of `count`-then-`insert`; prefix/suffix parse instead of regex.

**Risk.** `remapType` writes its cache entry only *after* recursing, so a self-referential struct recurses forever. Latent under typed pointers, which is where this codebase is. Insert a placeholder before descending.

**Gate.** Linked IR bit-identical to the A2 baseline across the corpus. Second gate: permutation *k* performs strictly fewer `TypeFinder` runs than *k−1* — instrument and assert.

**Deviation from plan.** The caches are **caller-owned**, not on `LLAIRContextImpl`. A caller constructs a `LinkerTypeCache` (public, in `include/llair/Linker/Linker.h`) and passes it to `Linker(Module&, LinkerTypeCache&)` / `linkModules(dst, src, cache)`, so it can scope canonicalization to a batch of modules it knows share struct identity rather than to the whole context. The 2-arg `linkModules(dst, src)` is retained (builds a fresh local cache) so A1/A2 call sites and tests are unchanged; `llair-link` and `llair-metallib` now build one cache and reuse it across all inputs linked into their single output. This also removes the layering exception the `LLAIRContextImpl` version needed — `Linker.cpp` never reaches into `lib/IR`, so `lib/Linker/CMakeLists.txt` is untouched. The `llvm::Regex` global is retired entirely (replaced by `canonicalStructIdentifier`, a prefix/suffix parse) rather than moved.

**Gate result.** `updateIdentifiedOpaqueStructTypes` was dropped entirely, so the second gate is satisfied unconditionally and permanently: the function that ran `TypeFinder` no longer exists, so the count is zero from the first permutation onward. Every opaque struct that can appear in `dst` arrives through `remapType`, which records it in the (now batch-scoped) canonical map on first sight; `dst` is always freshly built in real usage, never bitcode-loaded, so there is nothing to re-seed. The self-referential-struct recursion risk is fixed by inserting a placeholder before descending. Two regression tests in `examples/command-line/test.cpp` — `testLinkerCanonicalizesStructsAcrossSeparateLinks` (canonicalization survives across independent `Linker`/`TypeMapper` instances sharing one cache) and `testLinkerHandlesSelfReferentialStructTypes` (linking a `%struct.Node = { %struct.Node*, i32 }` terminates and verifies) — join the existing three.

## A4 — Symbol index on `llair::Module` — DONE (2026-08-21)

Pure addition; nothing consumes it in this milestone.

```cpp
// [new] llair::Module
const llvm::GlobalValue *lookupDefinition(llvm::StringRef) const;   // lazily built
void                     invalidateSymbolIndex();
```

One pass over `global_values()`, no instruction walking. It belongs on `llair::Module` rather than `Linker` because a source permutation module is indexed once and reused by every `dst` that pulls from it. `syncMetadata()` is the existing seam where the wrapper reconciles with a mutated `llvm::Module`; hang invalidation there.

**Gate.** For every module in the corpus, `lookupDefinition` agrees with a brute-force `global_values()` scan on every name present, and returns null for a name known absent.

**Gate result.** Landed on `llair::Module`: `lookupDefinition(StringRef) const` lazily builds a `mutable llvm::StringMap<const llvm::GlobalValue *>` over one pass of `getLLModule()->global_values()`, skipping declarations, guarded by a `mutable bool d_symbol_index_valid`; `invalidateSymbolIndex()` drops it. The index stores *definitions only* — `lookupDefinition` returns null for a name that is present only as a declaration, matching the method name and the gate's "null for a name known absent." Invalidation hangs off `syncMetadata()` (first statement), the choke point already invoked after construction-from-existing-module and after every link (`Linker.cpp:79`, `Linker.cpp:474`); no linker or writer code was touched, keeping this a pure addition with no consumer yet. Gate is a permanent regression test in `examples/command-line/test.cpp` (`testModuleSymbolIndexMatchesBruteForceScan`): builds a module with a defined + a declaration-only function and the same pair of globals, asserts `lookupDefinition` equals a hand-written brute-force `global_values()` scan for every name present, returns null for declaration-only and absent names, and — after adding a definition post-build — is invisible until `invalidateSymbolIndex()`/`syncMetadata()` drops the stale index, then found. Verified the test trips (the brute-force-agreement assertion) when the `isDeclaration()` skip is removed, and passes with it. `llair-test`, `llair-link`, `llair-metallib`, `llair-dump` all rebuild and link with no new warnings.

Fixed in passing: the A3 gate `testLinkerCanonicalizesStructsAcrossSeparateLinks` was aborting at its precondition (`foo1->getName() == "struct.Foo.1"`) on the committed tree, independent of this change. LLVM's struct-name-collision counter starts at `.0` in this toolchain, not `.1`, so the second `StructType::create("struct.Foo")` is renamed `struct.Foo.0`. The precondition now asserts only that LLVM renamed the collider (name differs and still begins `struct.Foo.`) rather than hard-coding the version-dependent integer; the substantive assertion (canonicalization makes `g0`/`g1` share a value type) was always correct — `canonicalStructIdentifier` strips any numeric `.N` suffix — and passes. Whole suite is green.

## A5 — Reachability-driven pull — DONE (2026-08-21)

Behind a flag, eager path retained for differential validation. Uses the shared reachability walk.

```cpp
// [new] Linker
void addModule(const Module *);       // index only, no cloning
void require(llvm::StringRef);        // push a root
void resolve();                       // drain: declare, scan operands, enqueue, clone
```

Roots: entry points, `llvm.used` / `llvm.compiler.used`, `air.static_init` constructors.

**Decided.** Source modules must outlive the `Linker`. This is the invariant A5 trades for; A0–A4 do not require it.

Milestone-specific hazards beyond the shared ones:

- **`RF_None` asserts on missing `VMap` entries.** A popped function's referenced globals need declarations before `CloneFunctionInto` runs. Order is scan → declare → clone, per pop.
- **`appendToGlobalCtors` is quadratic** as currently called — one rebuild of `llvm.global_ctors` per `air.static_init` function. Batch into a `SmallVector<Constant *>` and append once. This is a bug in the eager path too; fixable independently at any time.

A2's ODR dedup composes cleanly: a pop for a symbol `dst` already defines terminates that branch.

**Gate.** Run `GlobalDCE` over the eager result and compare against the pull result — module-equivalent modulo symbol ordering. Second gate: bitcode size and `newLibraryWithData` wall time both strictly decrease on at least one real permutation, or the milestone has not earned its complexity.

**Gate result.** Landed as a **library-API addition only** — `addModule`/`require`/`resolve` on `Linker`, plus `declare`/`define`/`findDefinition`/`copyNamedMetadata` and a file-local `forEachReferencedGlobalValue`, all in `lib/Linker/Linker.cpp`. **Scope narrowed from the v1 sketch:** no CLI flag and no `llair-link`/`llair-metallib` changes — the eager path is retained not behind a runtime flag but as the untouched `linkModule`, so the differential gate compares the two side by side. The pull keeps its own src-object-keyed `d_vmap` shared across all registered modules (a name declared in one and defined in another maps to one dst value); a name-keyed worklist with `d_enqueued`/`d_resolved` sets that make `resolve()` re-enterable (checked at pop, for the live `finalizeInterfaces` case); and a batched `d_pending_ctors` so `llvm.global_ctors` is built once (fixing the quadratic `appendToGlobalCtors`). The eager `linkModule` was left **bit-identical** — no helper extraction — so the ~6 lines of skeleton creation are duplicated in `declare` rather than shared, per "eager bit-identity wins"; the A1–A4 tests confirm it is unchanged. Roots are auto-seeded from every registered module's entry points, `llvm.used`/`llvm.compiler.used`, and `air.static_init` functions, plus explicit `require()`s.

The first (correctness) gate is automated and green; the second (perf) gate is deferred to the renderer, as decided. Two permanent tests landed in `examples/command-line/test.cpp`: `testLinkerPullReachesRequiredClosure` (mechanism — a callee, a ConstantExpr-referenced global joined cross-module decl→def, an ODR helper pulled through a ctor, and a static_init ctor are all pulled; a `dead_fn` is not; one `llvm.global_ctors` entry; no `.1` renames; clean `verifyModule`) and `testLinkerPullMatchesEagerThenPrune` (the load-bearing differential — pull's defined-symbol set and function/global/alias counts equal eager+`createInternalizePass`(full root set)+`createGlobalDCEPass`, in two variants: explicit `require("entry")`, and no require with an auto-seeded `air.kernel` entry-point root that `syncMetadata` rematerializes on `dst`). Verified via the doc's discrimination practice: breaking the walk's ConstantExpr descent trips exactly the closure assertion, and it passes restored. **Corpus note:** the eager baseline must link the *definer* module before the module that only declares its globals — eager's global loop joins a definition onto an existing dst declaration only in that order (functions join either way; the global asymmetry is itself part of why the order-independent pull exists). **Known spec gap documented in `copyNamedMetadata`:** named metadata is copied wholesale under `RF_None`, which is safe for `air.vertex/fragment/kernel` (every entry point is a root) but would assert on debug info referencing an un-pulled `GlobalValue` — which B1 established no Bourbon link input carries. Added the `ipo` LLVM component to the `llair-test` target for the prune passes. `llair-test`, `llair-link`, `llair-metallib`, `llair-dump` all rebuild with no new warnings; all A1–A4 tests still pass, proving the eager path stayed bit-identical.

## A6 — Explicit variation points and entry-level permutation key

Inferring the frontier from `isDeclarationForLinker()` conflates symbols intended to bind per permutation with symbols merely not yet pulled. A declared set gives validation (every point bound before handing IR downstream, rather than discovering an unresolved call in the driver) and the key defined under *Shared components*.

**Amended from v1:** the key is computed per entry point over that entry's reachable subgraph, not per library. B3 makes this the natural granularity.

The payoff is upstream of everything else here — the cheapest link is the one skipped because the key already maps to a compiled `MTLLibrary`.

**Gate.** Two independent constructions of the same binding set produce equal keys; any single binding change produces a different key; a binding change affecting only the fragment subgraph leaves the vertex key unchanged.

## A6 — Explicit variation points and entry-level permutation key — DONE (2026-08-21)

**Deviation / decisions.** Landed as a **library-API addition only** on `Linker` — `addVariationPoint(StringRef)`, `variationPointsBound()`, `permutationKey(const llvm::Function *)` — plus the two file-local helpers the plan's *Shared components* anticipated: `reachableClosure` (the reachability walk factored out of the pull, built on A5's existing `forEachReferencedGlobalValue`) and `hashDefinition`. No CLI or renderer wiring; the payoff (skip-the-link on a key hit) is the renderer's to claim, and B4 will consume the same key.

- **Chosen-definition identity is content, not a caller label.** A variation point's binding is identified by a stable `xxHash64` of the *linked* definition's printed IR (signature, attributes, body/initializer/aliasee), not by a caller-supplied tag. This keeps the `Linker` self-contained (no bookkeeping to drift out of sync with the actual IR) and makes IR-identical bindings collide by design — exactly what B4 wants when it reuses a bitcode blob. `xxHash64` is stable across executions (unlike `llvm::hash_code`, whose seed is a FIXME placeholder), so the key is fit for B4's cross-session `MTLBinaryArchive` reuse; the return type is a plain `uint64_t`.
- **The key omits the entry function itself.** It hashes only the sorted `(variation point name → definition hash)` pairs reachable from the entry, NUL-delimited so distinct sets cannot alias by concatenation. Two entries with identical reachable bindings therefore key equal — which is correct because B3 partitions the cache one metallib per entry: within an entry slot the entry body is fixed and only the bindings vary, and across slots the cache is keyed by entry identity, so no false hit. This is why the key can stay stable under shared-code edits (its whole reason to exist) rather than hashing the entire reachable subgraph.
- **Variation points are the frontier, validated after the pull.** `variationPointsBound()` reports false while any declared point is still a declaration in `dst` — the "unresolved call the driver would only discover at compile time" the milestone calls out. It composes with A5: a variation point is bound by the normal `require`/`resolve` pull of its chosen definer, not by any new mechanism.

**Gate result.** Automated and green. Two permanent tests in `examples/command-line/test.cpp`: `testPermutationKeyDiscriminatesBindings` builds a shape whose `vmain` reaches `vbxdf`+`common` and `fmain` reaches `fbxdf`+`common` (neither entry reaches the other's variation point; `common` is shared non-varying code), links a defs module whose two variation-point bodies are parameterized, and asserts all three gate clauses — equal keys for independent same-binding builds, a fragment-only change moving only the fragment key, and (the symmetric case) a vertex-only change moving only the vertex key — plus that distinct variation-point names keep the two entries' keys apart even at equal bound values. `testVariationPointBindingValidation` asserts `variationPointsBound()` is true only once the defs module supplies both definitions and false when the frontier is left unbound. Verified via the doc's discrimination practice: replacing the reachability restriction with a scan over all of `dst`'s variation points trips exactly the vertex-isolation assertion (`frag_changed.vmain == base.vmain`), and it passes restored. `llair-test`, `llair-link`, `llair-metallib`, `llair-dump` all rebuild with no new warnings; all A1–A5 tests still pass.

---

# Series B — Metallib writer

`WriteMetalLibToFile` is treated as a fixed, unmodifiable container assembler. Every milestone changes what llair hands it.

## B1 — Strip debug info before packaging — DONE (2026-08-14)

`WriteMetalLibToFile` gates a large branch on `M.debug_compile_units_begin() != M.debug_compile_units_end()`. If any `DICompileUnit` survives `finalizeLibrary`, then per call it constructs a full `ValueEnumerator140` over the module solely to walk its metadata map for `DIFile` nodes and discards it; reads every referenced source file from disk; embeds all that source text into a `recompile_info` node; builds a tar archive and bz2-compresses it; and writes a second complete bitcode file to the filesystem at `<source>.air`.

Filesystem I/O and bz2 compression per permutation, none of it consumed by Bourbon.

**Decided.** `finalizeLibrary` calls `llvm::StripDebugInfo` before the module reaches `makeLibrary`. Implemented unconditionally (`lib/Tools/MakeLibrary.cpp`), not flag-gated: nothing in the codebase consumes a debug-info-preserving option today, and `llair-metallib.cpp` has no CLI flag for it, so a bool parameter would be a speculative extension point. Add one later if a debug-info-preserving path is actually needed for Xcode's GPU debugger.

**Gate result.** Instrumented the predicate first, without needing to run the interactive renderer: disassembled all 98 `.bc` files produced by the current build (Metal-compiled shader sources plus MaterialX BXDF/pattern-generated bitcode) and found none carry a `DICompileUnit`; no `-g` flag exists anywhere in `cmake/modules/metal.cmake`. The predicate is false on Bourbon's IR today, so this landed as a correctness/future-proofing fix rather than a measured performance win — the doc's own anticipated outcome. Series B should rerank around B2 next. Verified: `libLLAIRTools`/`BourbonCore`/`llair-metallib` rebuild and relink cleanly, all existing tests (174 GoogleTest cases plus `tg-test`/`future-test`) pass, and `llair-metallib` still produces a valid `.metallib` end-to-end.

## B2 — Calibrate the optimization pipeline — DONE (2026-08-24)

`finalizeLibrary` runs `PassManagerBuilder` at `OptLevel 3` / `SizeLevel 1` over the whole module. The output is AIR bitcode that Apple's driver then compiles to AGX ISA with its own full pipeline. Much of this is work the driver redoes.

It is not obviously all wasted: inlining before the per-entry split shrinks each split module, which compounds into B3 and B4. But the burden of proof is on keeping it.

**Decided.** Measure before deciding — the same discipline as calibrating light units before converting them. Build the corpus at `OptLevel 0` and `3`; compare `makeLibrary` wall time, `newLibraryWithData` wall time, and frame time of the resulting pipeline state. If frame time is unchanged, delete the pipeline.

Note regardless of outcome: `PassManagerBuilder` and the legacy pass manager are removed in LLVM 17, so this code needs rewriting at the next upgrade. The separate `fpm`-over-all-functions loop followed by `mpm` is the standard clang idiom, not an accident — it is not the redundancy worth chasing.

**Gate.** A recorded decision with the three numbers attached, not a code change.

**Landed.** `opt_level` is now a permanent parameter (default `3`) threaded through `finalizeLibrary(const Module&, unsigned)` and `makeLibrary(const Module&, unsigned)`, exposed as `-O` on `llair-metallib` and `make-library`, and read from `BOURBON_METALLIB_OPT` (default `3`) at the one call site in Bourbon's `Program::build`. Only `pmb.OptLevel` is parameterized; `SizeLevel` stays `1` per the doc's instruction, and lines 49–53 (`Inliner`/unroll/vectorize) are untouched — they already key off `OptLevel`. Three `os_signpost` interval pairs (Points-of-Interest category, subsystem `com.bourbon.llair`) instrument `finalizeLibrary`, `makeLibrary`, and `newLibraryWithData`, each tagged with `opt_level`; `Program::build` also gained an env-gated corpus dump (`BOURBON_DUMP_METALLIB_IR=<dir>`, `llvm::WriteBitcodeToFile` on the pre-finalize module, monotonic per-process counter) that freezes real permutations to disk.

**Measurement.** Corpus: 43 real permutations captured live from `examples/sg/triangle` and `examples/sg/sdf3ds` (no external asset dependencies, per the user's steer away from `mtlxview`, which crashes headless on a missing-model `intrusive_ptr` assertion unrelated to this milestone) via `BOURBON_DUMP_METALLIB_IR`, at `/tmp/metallib_corpus` (ephemeral, not committed, per the doc's *Validation* section). (1)/(2) were measured offline by replaying the corpus through `make-library -O0`/`-O3` (which exercises `finalizeLibrary`, `makeLibrary`, and `newLibraryWithData` in one process) with signposts captured via `log stream --signpost --predicate 'subsystem == "com.bourbon.llair"'`; only inputs that round-tripped cleanly at both levels were used, 5 of 43 (see *Corpus finding* below), across 10 reps each:

- **`finalizeLibrary`**: O0 median 9.5µs / p90 24.1µs (n=38) vs O3 median 247.8µs / p90 504.2µs (n=32) — O3 is ~26x slower, entirely the cost of actually running the optimizer.
- **`makeLibrary`** (finalize + `WriteMetalLibToFile`): O0 median 65.7µs / p90 146.9µs (n=38) vs O3 median 335.0µs / p90 590.2µs (n=32) — ~5x slower at O3, diluted by the fixed writer cost.
- **`newLibraryWithData`**: O0 median 1.0µs / p90 1.5µs (n=38) vs O3 median 1.0µs / p90 1.6µs (n=32) — no measurable difference. The Metal driver defers real AGX compilation past container load, so this call is insensitive to the AIR's optimization level.

(3) was measured on the live renderer, not a proxy: `xctrace record --template "Metal System Trace" --launch -- examples/sg/triangle` with `BOURBON_METALLIB_OPT=3` and `=0`, two independent 6-second recordings per level, GPU-interval durations summed per `gpu-frame-number` and filtered to the `triangle` process via the `metal-gpu-intervals` table (`xctrace export`):

- Recording 1: O3 median 1.673ms / p90 2.084ms (554 frames) vs O0 median 1.931ms / p90 2.232ms (389 frames).
- Recording 2: O3 median 1.800ms / p90 2.135ms (508 frames) vs O0 median 2.060ms / p90 2.384ms (657 frames).
- O0 is ~13–16% slower per frame than O3 on the GPU timeline, consistently across both recordings.

**Gate result.** Frame time is not unchanged — O0 regresses it. Per the doc's decision rule, the pipeline is **kept**: `opt_level` stays defaulted to `3` (already true of the landed parameter; no further code change). `makeLibrary`/`finalizeLibrary` wall time is real but small in absolute terms (hundreds of µs, off the interactive-rebinding critical path relative to the driver's own AGX compile), and `newLibraryWithData` is unaffected either way — the entire measured cost of keeping O3 is upfront CPU time in `finalizeLibrary`, paid back by faster GPU execution every frame thereafter. This also means B3/B4's premise — that pre-shrinking via inlining before the per-entry split compounds into a real win — is not undercut by this milestone.

**Corpus finding (out of scope, flagged for backlog).** Only 5 of 43 captured permutations (`-0`, `-1`, `-8`, `-10`, `-31`) round-tripped through `llair-metallib` at both O0 and O3; most crash non-deterministically (`SIGBUS`/`SIGSEGV`, confirmed via `lldb`) inside vendored `ModuleBitcodeWriter::writeMetadataStrings`, called from `WriteBitcodeToFile140` from `WriteMetalLibToFile`'s own internal bitcode write — reproducible at both opt levels for the same input (ruling out an `OptLevel`-dependent cause) but flaky across repeated runs of the identical input (ruling out a purely content-triggered cause; points at uninitialized/dangling memory over malformed metadata, plausibly the same "invalid debug info version (0)" condition every captured module warns about on read-back). Out of scope here per the doc's vendored-`floor_llvm` constraint; added to *Correctness backlog* below.

**Follow-up (2026-08-24): prune-before-O3 rejected; O0-vs-O3 re-measured on release.** `finalizeLibrary` kept profiling high in the live renderer (~359 ms / 17.7% in `PassManagerImpl::run`, cumulative over a session's unique programs). Two things were checked before accepting the cost.

- *Is `finalizeLibrary` running O3 over dead code the writer discards?* Partly yes, but pruning it first does not pay. The module `finalizeLibrary` receives is the **full eager-linked** module (`Program::link` uses `linkModule`, not A5's pull), and **34.8% of its instructions / 58% of its functions are unreachable** from the writer's root set (entry points ∪ `air.static_init`, per B3). But an `internalize(roots) + GlobalDCE` prepass before the O3 pipeline saved only **1.0% aggregate wall time** across a 64-module corpus, and *regressed* the large modules 24–26%. Mechanism (measured): O3 alone does not prune this cheaply — it *inlines* the dead code and keeps the dead external functions, growing the module 245k → 273k instructions; the prepass path ends at 149k but takes the same time, because O3's cost on the dead code ≈ the prepass's own cost, and internalizing the *live* functions exposes extra inlining that eats the rest. The writer (B3) discards the dead code per-entry either way, so output is unchanged. Lever abandoned: `finalizeLibrary`'s cost is intrinsic to optimizing the *live* code.

- *Were the frame-time numbers above (item 3) release or debug?* The record didn't say (bare `examples/sg/triangle` path). Re-measured on **release** (`build-macos-release`, M5 Pro), same binary for CPU and GPU, `metal-gpu-intervals` depth-0 durations summed per `gpu-frame-number`:

  | scene | GPU/frame O3 → O0 | O0 penalty | `finalizeLibrary` CPU O3 median (p90) | O0 median |
  |---|---|---|---|---|
  | `triangle` | 1.084 → 1.335 ms | **+23%** | 9.9 ms (65 ms) | 0.5 ms |
  | `gltfview` SciFiHelmet (PBR) | 1.669 → 2.314 ms | **+38.7%** | 16.2 ms (36 ms) | 0.6 ms |

  The GPU regression is *larger* than item 3's 13–16% (which was likely debug and/or a lighter frame), and it grows with material complexity while the one-time CPU cost stays ~10–16 ms/program (O3/O0 ratio ~23–25×). Break-even: O0's per-program CPU saving (~9–16 ms) is repaid by GPU overhead in ~24–38 frames (well under a second), then a permanent per-frame loss. Note item 1's "247 µs median" was only the 5 *tiny* modules that survived the writer crash; the full-corpus median is ~10–16 ms. **Gate result reaffirmed on release: keep O3.**

- *Would compiling the Metal source with full optimization change the picture?* No — measured and rejected. The metal frontend already defaults to `-O2` (verified: default AIR bitcode is bit-for-bit the size of `-O2`/`-O3`, distinct from `-O0`, and carries no `optnone`); the shader/BXDF/pattern compiles were simply not passing `-O3`. Appending `-O3` to `METAL_FLAGS` in `lib/BourbonRenderer` and `lib/BourbonSG` (which flows to both the `.metal`→`.bc` path and the `bourbon-bxdfc`/`patternc` MaterialX path, all of which invoke `metal -c` via `llair::compileBuffer`) and rebuilding `gltfview` changed nothing measurable: GPU/frame at finalize O3 1.669 → 1.687 ms (noise), the finalize O0-vs-O3 gap +38.7% → +37.1% (unchanged), and `finalizeLibrary` CPU 16.2 → 16.1 ms median / 231 → 235 ms total (unchanged). The BXDF (`gltf_pbr.bc`) stayed **26 functions / 305 inter-function calls** at `-O3` (only 19 → 17 allocas) — `-O3` does not inline the MaterialX-emitted component functions into each other at the per-file stage. The inlining that actually collapses those calls into fast GPU code happens in `finalizeLibrary`'s **whole-module** O3 pass *after linking*, which per-file source optimization structurally cannot substitute for (the cross-function/cross-module call graph doesn't exist until link time). This is why finalize O0 regresses ~37% regardless of source opt level. Reverted; build restored to `-O2`.

All three avenues for reducing `finalizeLibrary`'s cost are now closed by measurement: lower its opt level (frame-time regression), prune-before-O3 (wall-time wash), and pre-optimize the source (no effect). The cost is intrinsic to the post-link whole-module optimization the GPU depends on. **And B4 does not open a fourth:** `bourbon::ProgramCache` (`lib/BourbonCore/ProgramCache.cpp`) already hash-conses each `Program` on its `ProgramModule` set, and `Program::build` memoizes `d_metal_library`, so within a `CoreContext` `makeLibrary`/`finalizeLibrary` runs *exactly once per composition* — which is B4's "a cache hit skips `makeLibrary` entirely," already realized, just keyed on module-set identity rather than A6's content key. See the B4 note below.

## B3 — Reachability-filtered per-entry clone — DONE (2026-08-24)

`WriteMetalLibToFile` performs a full `CloneModule` **per entry point**, each followed by `createInternalizePass` down to a single symbol plus `createGlobalOptimizerPass` to prune it back. With `finalizeLibrary`'s own clone that is `1 + E` deep copies, and each per-entry clone is clone-everything-then-delete-almost-all.

`E` is larger than it appears. A `MaterialEvaluationPass` program carries at least seven entry points — `Init`, `Allocate`, `OffsetsAscending`, `OffsetsDescending`, `Distribute`, `EmitLaunch`, `Main`. Six are tiny queue-management kernels, and each currently receives a complete clone of a module containing every BXDF permutation body before internalize and DCE discard it.

**Vendored patch.** `llvm::CloneModule` has an overload taking `function_ref<bool(const GlobalValue *)> ShouldCloneDefinition`. Compute reachability from the entry point first — a walk over the existing IR, no allocation, no cloning — then clone only what it reaches.

**Correction, and a hard constraint on this milestone.** An earlier draft claimed internalize and GlobalOpt might become droppable once the clone is filtered. That is wrong, and shipping it would silently break function constants.

`ShouldCloneDefinition` returning false still emits the `GlobalValue` as an external *declaration*; it does not omit it. The writer's `air.function_constants` filter keeps nodes whose `getOperand(0)` is non-null, which is a proxy for "the underlying global was erased" — and a declaration is not erased. Unreferenced constants would therefore survive into the entry's `CNST` tag, and Metal would demand values for constants the entry point never reads. `createGlobalOptimizerPass` does not erase these; `GlobalDCE` does, which is why the local addition of a per-extracted-module DCE was necessary to make function constants work at all.

**Decided.** The filtered clone is an addition to the existing prune, not a replacement for it. A DCE (or equivalent explicit erasure of unreferenced declarations) must run on each cloned module. `air.sampler_states` and the entry-point metadata lists use the identical null-operand idiom and are subject to the same reasoning.

Uses the shared reachability walk from *Shared components*, which means it must be reachable from the writer's translation unit. **Implementer's choice** whether that means the walk lives in a small header both consume, or is duplicated — it is thirty lines, and duplication may be the cleaner patch to offer upstream.

**The v1 decision to emit one `.metallib` per entry point is withdrawn.** It existed only to pre-shrink the module before a clone that could not be filtered. Multi-entry containers are retained, which keeps `MTLLinkedFunctions` and `MTLVisibleFunctionTable` paths available without revisiting the decision, and keeps the cache unit in B4 a container entry rather than a whole file.

**Gate.** Byte-identical bitcode blobs versus the unfiltered path for every entry point, or a documented explanation of each difference. Second gate: per-entry clone cost scales with the entry's reachable set rather than with module size — instrument on a module with one small entry and one large one. Third gate, non-negotiable: a module where entry points use disjoint subsets of the function constants produces per-entry `CNST` tags listing only that entry's constants. This is the regression the correction above describes; it must be a test, not an inspection.

**Landed.** The per-entry clone/prune/metadata-cleanup block is now `llvm::buildEntryModule(const Module &M, const Function *entry, function_ref<bool(const GlobalValue *)> ShouldCloneDefinition, uint32_t target_air_version)`, a free function in `MetalLibWriterPass.cpp` declared in a new small vendored header, `llvm/include/llvm/Bitcode/MetalLibWriter.h` (placed alongside `BitcodeWriter.h`, where `WriteMetalLibToFile` itself is declared — `llvm/include/llvm/Bitcode/MetalLib/` does not exist as a directory). `WriteMetalLibToFile`'s per-entry loop now computes each entry's reachable `GlobalValue` set and passes a predicate backed by it, instead of always cloning full definitions. The reachability walk (`visitReferencedGlobalValueOperand`/`forEachReferencedGlobalValue`/`reachableClosure`) is duplicated, file-local, in an anonymous namespace in `MetalLibWriterPass.cpp` — the *Implementer's choice* the plan left open — mirroring `lib/Linker/Linker.cpp`'s walk exactly (ConstantExpr descent to `GlobalValue` leaves, `MetadataAsValue` short-circuit) with comdat-group pull folded directly into the worklist (unlike `Linker::define`, which handles it as a separate step). `createGlobalDCEPass()` is added to the per-entry `legacy::PassManager` immediately after `createGlobalOptimizerPass()`, per the correction's hard constraint. Root set is just the entry `Function*` (plus its transitive closure and comdat pulls) — no extra `llvm.used`/`static_init` roots, as specified.

**Deviations from the plan.** Landed as one combined change rather than the two strictly-sequential edits the plan describes (behavior-preserving refactor, then filtered predicate) — gate 1's test achieves the same isolation by calling `buildEntryModule` with an always-true predicate and the real predicate side by side and diffing the output, so a separate intermediate commit wasn't needed to tell "did the refactor break something" apart from "did the filtering break something." `examples/command-line/CMakeLists.txt` gained the `metallib`/`bitwriter50` LLVM components (matching `lib/Tools/CMakeLists.txt`'s existing list) so `llair-test` can call `buildEntryModule` and `WriteBitcodeToFile140` directly.

**Gate result.** All three gates are automated, permanent tests in `examples/command-line/test.cpp`, alongside A1–A6/B1–B2's.

- **Gate 1** (`testBuildEntryModuleByteIdenticalToUnfilteredPath`): a module with two entries sharing a `common` function, each with a private helper and global nothing else reaches. `buildEntryModule` is called twice per entry — always-true predicate (today's full clone) vs. the real reachable-set predicate — and the two `WriteBitcodeToFile140` outputs are asserted byte-identical. Passes: internalize+GlobalOpt+GlobalDCE already converge both paths to the same surviving set, so the filtered clone changes cost, not output.
- **Gate 2** (`testBuildEntryModuleCostScalesWithReachableSet`): one tiny entry plus 500 vs. 5000 unrelated padding functions (10x). Best-of-3 wall time: 500 → ~0.6ms, 5000 → ~4.3–4.6ms — well under the deliberately generous bound (5x + a small floor for sub-ms noise) chosen only to catch an accidentally-still-O(module-size) implementation (e.g. `ShouldCloneDefinition` silently ignored), not to prove tight scaling. Recorded in the doc's B2-style measurement convention rather than as a strict pass/fail.
- **Gate 3** (`testBuildEntryModuleDisjointFunctionConstants`): two entries with disjoint `air.function_constants` globals; each entry's returned module is asserted to keep only its own constant. Passes. Per the doc's discrimination practice, the added `createGlobalDCEPass()` call was temporarily commented out and the test rerun — it still passed. This LLVM fork's `createGlobalOptimizerPass()` (`GlobalOpt.cpp`'s `deleteIfDead`, called unconditionally over every function and global variable in `OptimizeFunctions`/`OptimizeGlobalVars`, regardless of linkage, whenever the value `isDeclaration()`) already erases unreferenced external declarations on its own, so the correction's premise ("`createGlobalOptimizerPass` does not erase these") does not hold for this specific vendored LLVM version in the constructions tried. `GlobalDCE` is kept anyway per the milestone's decision — it is cheap, matches the `finalizeLibraryForLLD` internalize+`GlobalDCE` pattern already used elsewhere in this codebase (`lib/Tools/MakeLibrary.cpp`), and does not depend on `GlobalOpt`'s internals holding across an LLVM upgrade — but the specific discrimination check the plan asks for did not reproduce a failure as described, and that discrepancy is recorded here rather than silently claimed.

**Corpus/build verification.** `llair-test`, `llair-link`, `llair-dump`, `llair-metallib`, `metalc`, `make-library`, and `llair-triangle` all rebuild cleanly; `llair-test` is fully green (A1–A6/B1–B2 plus the four new tests, see the post-landing correction below). `make-library` against `examples/interactive/llair-triangle`'s two-entry (vertex+fragment) bitcode round-trips end-to-end through the new path: `newLibraryWithData` succeeds and both `MTLFunction`s load from the real Metal driver; the run's only failure is the test harness omitting a vertex descriptor for `stage_in`, unrelated to this change. `make-library` against `examples/command-line/example.metal`'s three-entry bitcode reproduces B2's documented "Corpus finding" crash inside vendored `WriteBitcodeToFile140` — confirmed pre-existing (and identical) by reverting this milestone's change and reproducing the same crash on the unmodified code, so it is not a regression.

**Post-landing correction (2026-08-24): `air.static_init` ctors were silently dropped.** The gates above only exercised synthetic modules; the milestone's own text called out exactly this gap ("If this assumption is wrong \[...\] gate 1's byte-identical-output check against the unfiltered path will catch it immediately on the real corpus"). It surfaced instead as a real bug: after rebuilding Bourbon's own `deps/llair` against this change, `examples/sg/triangle` stopped rendering.

Root cause: Metal's function-constant mechanism generates a compiler ctor (`_GLOBAL__sub_I_*`, `section "air.static_init"`) that copies a driver-supplied function-constant value into the ordinary global the entry's own code reads. The ctor has no call edge from any entry — the driver runs it automatically before any kernel in the program executes — so a reachability walk rooted only at the entry can never discover it. Confirmed with the B2 corpus mechanism (`BOURBON_DUMP_METALLIB_IR` against `examples/sg/triangle`, disassembled per-entry output via `metallib-dis`): the unfiltered (pre-B3) path always kept every ctor and `@llvm.global_ctors` intact in every entry's split; the filtered path silently dropped them.

A first fix — adding every `air.static_init` function's own reachable closure as an unconditional extra root — was still insufficient: `Function::isDefTriviallyDead()` treats an internal-linkage function with zero real (non-metadata) uses as dead, and a ctor's only real use is normally the pointer to it inside `@llvm.global_ctors`'s initializer. Since nothing marked `@llvm.global_ctors` itself reachable, it was cloned as a bare declaration, the ctor lost its one protecting use, and `GlobalOpt` (independent of the `GlobalDCE` addition — see B3's Gate 3 discussion of `deleteIfDead`) deleted it outright despite being freshly marked reachable.

**Fix, landed:** a new `llvm::computeStaticInitRoots(const Module &M)` (declared in `MetalLibWriter.h`, defined in `MetalLibWriterPass.cpp`) unions the reachable closures of every `air.static_init`-sectioned function *and* of `@llvm.global_ctors` itself (if present) — rooting at the array's initializer naturally covers every registered ctor and whatever each one references, independent of the section scan. `WriteMetalLibToFile`'s per-entry predicate is now `reachable(entry) ∪ computeStaticInitRoots(M)`, unconditionally, for every entry. Re-verified against the same real corpus: per-entry output is now byte-identical to the pre-B3 baseline (differing only in the writer's own per-file random UUID). Rebuilt `deps/llair` and Bourbon's top-level app end-to-end; `examples/sg/triangle` renders correctly again (confirmed by the user).

Added a fourth permanent test, `testBuildEntryModulePreservesStaticInitCtor`: a synthetic ctor that stores a load from an external, uninitialized "function-constant placeholder" global (not a compile-time constant a naive test would let `GlobalOpt`'s ctor evaluator fold away, which is what an earlier draft of this test got wrong) into a global the entry reads. Asserts the pre-fix predicate (entry-only reachability) loses the ctor, and the fixed predicate (`computeStaticInitRoots` unioned in) keeps it as a real definition with `@llvm.global_ctors` intact. `llair-test` is green with all four `buildEntryModule` tests.

This changes the milestone's earlier claim that "each `.metallib` entry only needs to preserve what that one entry actually executes" (*Root set* above): that's true of the entry's own code, but not of the AIR runtime's implicit static-init contract, which every entry in a compiled program shares regardless of which one a given `.metallib` slice is for.

## B4 — Entry-level blob caching

Each container entry holds an independently SHA-256-hashed bitcode blob. Keyed on A6's entry-level permutation key, an unchanged entry's blob, hash, and extended-metadata block are all reusable — editing one material invalidates the fragment entry and leaves the vertex entry untouched.

With B3's decision, this falls out almost for free: one metallib per entry means the cache unit is a whole file buffer, and a cache hit skips `makeLibrary` entirely rather than reassembling a container around reused pieces.

**Gate.** Cache hit rate over a recorded interactive editing session. If the rate is low, A5 loses most of its value too — measure this before building A5, not after.

**Reassessment (2026-08-24): B4's in-session mechanism already exists as `bourbon::ProgramCache`.** `lib/BourbonCore/ProgramCache.cpp` hash-conses every `Program` on the sorted set of `ProgramModule *` composing it (via `SequentialHashConser`, plus `CompositeHashConser` for program unions), and `Program::build` (`lib/BourbonCore/Program.cpp`) memoizes its result in `d_metal_library` behind an early-return guard. Consequently, within a `CoreContext`, `makeLibrary`/`finalizeLibrary` already runs **exactly once per unique composition** — precisely B4's "a cache hit skips `makeLibrary` entirely." B4 therefore does *not* reduce the `finalizeLibrary` cost that motivated it (the ~359 ms is already the cost of building each *distinct* program once; a session that builds N distinct programs pays it N times under either scheme). B4's only marginal gains over what ships today are (a) **cross-session / cross-`CoreContext` persistence** — `ProgramCache` is in-memory and per-context, whereas B4's content key + SHA-256 blob could persist to disk and feed `MTLBinaryArchive` across runs — and (b) **composition aliasing** — two *different* `ProgramModule` sets that produce IR-identical reachable subgraphs are distinct keys to `ProgramCache` (pointer identity) but equal under A6's content key, so B4 would collapse them where `ProgramCache` cannot. Whether either is worth building is the same cache-hit-rate measurement the gate already calls for; the in-session, same-composition case — the common one — is not on the table because it is already handled. This also resolves the *Risks* entry "A5 and A6 may be redundant with each other": the redundancy is broader — the in-session slice of B4 is redundant with existing renderer infrastructure.

---

## Deferred

- **Opaque-pointer migration.** A0's harvested note and A3's caller-owned canonical struct table are the enablers: receiver types come from a name lookup, not `getElementType()`.
- **LLVM 17+ upgrade.** `PassManagerBuilder`, the legacy pass manager, `llvm::Optional`, and `PointerType::getElementType()` all block it. B2's outcome determines how much of `finalizeLibrary` survives the port.
- **Threading.** `LLVMContext` is not thread-safe. If permutation builds move to workers, each owns a context, and neither source modules nor the A3 caches cross worker boundaries — the ThinLTO shape is bitcode buffers parsed lazily per context with per-context copies of both tables. Cheaper to decide before A3 lands. The `contexts::llvm_to_llair()` global `std::map` is the one unguarded static either way; make it a `DenseMap` regardless.
- **`makeLibraryWithLLD` path.** Untouched by this plan. It already does reachability properly via internalize-to-entry-points plus `GlobalDCE`, at the cost of a subprocess.

## Correctness backlog

Independent, cheap, no ordering constraints:

- `llvm.module.flags` is dropped from `src` entirely when `dst` has any, rather than merged under the flags' own behavior semantics.
- Non-"once" named metadata operands are appended unconditionally, so linking the same source twice duplicates them.
- `s_once_metadata_names` is a `std::set<StringRef>` of six entries — `StringSwitch` or a sorted array.
- `<llair/IR/Module.h>` is included twice in `Linker.cpp`.
- `makeLibrary(const llvm::Module &)` const_casts and the writer mutates the argument — `setSDKVersion`, named metadata erasure, `DIFile` operand replacement. The clone in `finalizeLibrary` is therefore load-bearing for source preservation and must not be removed as "redundant," whatever B2 decides about the pass pipeline.
- **Found during B2.** Vendored `ModuleBitcodeWriter::writeMetadataStrings` (reached via `WriteBitcodeToFile140` from inside `WriteMetalLibToFile`) crashes non-deterministically (`SIGBUS`/`SIGSEGV`) on most real permutations captured from `examples/sg/triangle`/`sdf3ds` (5 of 43 survived at both O0 and O3), independent of `OptLevel`. Every affected module warns "ignoring debug info with an invalid version (0)" on bitcode read-back; likely cause, not confirmed. Out of scope for B2 per the vendored-`floor_llvm` constraint.

## Validation

One corpus of real permutations, captured once, reused as the fixture for every gate. Each milestone compares against the immediately preceding milestone, not a frozen original — A1, A5, B1 and B3 all change output legitimately, and a single baseline would produce false failures at exactly the points where scrutiny matters most.

Rendered-image comparison is the backstop and the weakest signal: a duplicated `linkonce_odr` body renders identically to a deduplicated one. IR-level and instrumentation-level assertions are what catch these.

## Ordering

B1 first, before anything else in either series — it is one call, and its outcome determines whether Series B or Series A dominates. **Done (2026-08-14): predicate was false on Bourbon's real IR, so B1 did not resolve that question — move to B2 next.** B2 and B4's gates are measurements that should also precede the milestones they inform. **Done (2026-08-24): frame time regresses at O0, so the pipeline is kept at O3 — move to B3 next.** **Done (2026-08-24): reachability-filtered per-entry clone landed — move to B4 next.** A0 is retracted (see above); A1–A4 are independent of both series' outcomes and can proceed in parallel.

## Risks

- **A1 is a cost increase.** Cloning distinct metadata is real work the current code avoids by mutating sources. Accept it; the property it buys is the premise of the design.
- **A5 is the only milestone that can be wrong quietly.** A missed edge produces IR that links, verifies, and then fails in the driver or renders subtly wrong. The `GlobalDCE` differential gate is load-bearing.
- **A3 widens the scope of mutable state.** A stale entry in a shared `LinkerTypeCache` corrupts every link in that batch rather than one. Caller-owned scoping bounds the blast radius to the batch the caller chose to share, and types are immortal so it should be safe by construction — assert that a remapped type's context matches rather than trusting the argument.
- **A5 and A6 may be redundant with each other.** High cache hit rate on the permutation key sharply reduces A5's marginal value. B4's gate answers this; run it early.
- **B3's win scales with E.** If a typical permutation library has one or two entry points, the clone reduction is small and B2's outcome matters far more.
- **Upstream floor_llvm sync is now a standing dependency.** The version tables and `llvm.ident` strings need updating per macOS release. If a sync ever changes `WriteMetalLibToFile`'s entry-point handling, B3's assumption that a single-entry module produces one well-formed container needs rechecking.

## Deletion

This document is scaffolding. Lasting invariants — source immutability, ODR dedup semantics, the A5 source-lifetime requirement, the no-fork constraint and its reasoning — get harvested into code comments or `lib/Linker/CLAUDE.md` and `lib/Tools/CLAUDE.md` before it goes.
