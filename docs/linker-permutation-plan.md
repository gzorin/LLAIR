# llair permutation pipeline: linker and metallib writer

Target: `lib/Linker/Linker.cpp`, `include/llair/Linker/Linker.h`, `lib/IR/LLAIRContextImpl.h`, `lib/IR/Module.{h,cpp}`, `lib/Tools/MakeLibrary.cpp`.

Out of scope by decision: `MetalLibWriterPass.cpp` and the `Writer50` / `Writer140` vendored bitcode writers. See *Constraints*.

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

## B2 — Calibrate the optimization pipeline

`finalizeLibrary` runs `PassManagerBuilder` at `OptLevel 3` / `SizeLevel 1` over the whole module. The output is AIR bitcode that Apple's driver then compiles to AGX ISA with its own full pipeline. Much of this is work the driver redoes.

It is not obviously all wasted: inlining before the per-entry split shrinks each split module, which compounds into B3 and B4. But the burden of proof is on keeping it.

**Decided.** Measure before deciding — the same discipline as calibrating light units before converting them. Build the corpus at `OptLevel 0` and `3`; compare `makeLibrary` wall time, `newLibraryWithData` wall time, and frame time of the resulting pipeline state. If frame time is unchanged, delete the pipeline.

Note regardless of outcome: `PassManagerBuilder` and the legacy pass manager are removed in LLVM 17, so this code needs rewriting at the next upgrade. The separate `fpm`-over-all-functions loop followed by `mpm` is the standard clang idiom, not an accident — it is not the redundancy worth chasing.

**Gate.** A recorded decision with the three numbers attached, not a code change.

## B3 — One entry point per metallib

Currently `WriteMetalLibToFile` performs a full `CloneModule` **per entry point**, each followed by `createInternalizePass` down to a single symbol plus `createGlobalOptimizerPass` to prune it back. With `finalizeLibrary`'s own clone that is `1 + E` deep copies, and each per-entry clone is clone-everything-then-delete-almost-all.

llair instead applies the shared reachability walk per entry point, produces a single-entry pruned module for each, and calls the writer once per entry. The writer's internal clone then copies a module that is already minimal, and internalize/GlobalOpt find nothing to do. No third-party changes.

**Decided.** Emit one `.metallib` per entry point rather than merging. Merging would require reimplementing container assembly — offsets, tag tables, header control — which is exactly the fork the constraints forbid. Metal permits `vertexFunction` and `fragmentFunction` to come from different `MTLLibrary` objects, so co-residency is not required for ordinary render pipeline states.

**Risk, resolve before building.** Verify Bourbon has no case needing functions co-resident in one library. Linked functions and visible function tables (`MTLLinkedFunctions`, `MTLVisibleFunctionTable`) have their own residency rules; if any current or planned path uses them, this decision needs revisiting and merging comes back into scope.

**Gate.** Byte-identical bitcode blobs versus the pre-split path for the same entry point, or a documented explanation of every difference. Second gate: total clone count per permutation drops from `1 + E` to `E`, instrumented.

## B4 — Entry-level blob caching

Each container entry holds an independently SHA-256-hashed bitcode blob. Keyed on A6's entry-level permutation key, an unchanged entry's blob, hash, and extended-metadata block are all reusable — editing one material invalidates the fragment entry and leaves the vertex entry untouched.

With B3's decision, this falls out almost for free: one metallib per entry means the cache unit is a whole file buffer, and a cache hit skips `makeLibrary` entirely rather than reassembling a container around reused pieces.

**Gate.** Cache hit rate over a recorded interactive editing session. If the rate is low, A5 loses most of its value too — measure this before building A5, not after.

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

## Validation

One corpus of real permutations, captured once, reused as the fixture for every gate. Each milestone compares against the immediately preceding milestone, not a frozen original — A1, A5, B1 and B3 all change output legitimately, and a single baseline would produce false failures at exactly the points where scrutiny matters most.

Rendered-image comparison is the backstop and the weakest signal: a duplicated `linkonce_odr` body renders identically to a deduplicated one. IR-level and instrumentation-level assertions are what catch these.

## Ordering

B1 first, before anything else in either series — it is one call, and its outcome determines whether Series B or Series A dominates. **Done (2026-08-14): predicate was false on Bourbon's real IR, so B1 did not resolve that question — move to B2 next.** B2 and B4's gates are measurements that should also precede the milestones they inform. A0 is retracted (see above); A1–A4 are independent of both series' outcomes and can proceed in parallel.

## Risks

- **A1 is a cost increase.** Cloning distinct metadata is real work the current code avoids by mutating sources. Accept it; the property it buys is the premise of the design.
- **A5 is the only milestone that can be wrong quietly.** A missed edge produces IR that links, verifies, and then fails in the driver or renders subtly wrong. The `GlobalDCE` differential gate is load-bearing.
- **A3 widens the scope of mutable state.** A stale entry in a shared `LinkerTypeCache` corrupts every link in that batch rather than one. Caller-owned scoping bounds the blast radius to the batch the caller chose to share, and types are immortal so it should be safe by construction — assert that a remapped type's context matches rather than trusting the argument.
- **A5 and A6 may be redundant with each other.** High cache hit rate on the permutation key sharply reduces A5's marginal value. B4's gate answers this; run it early.
- **B3's win scales with E.** If a typical permutation library has one or two entry points, the clone reduction is small and B2's outcome matters far more.
- **Upstream floor_llvm sync is now a standing dependency.** The version tables and `llvm.ident` strings need updating per macOS release. If a sync ever changes `WriteMetalLibToFile`'s entry-point handling, B3's assumption that a single-entry module produces one well-formed container needs rechecking.

## Deletion

This document is scaffolding. Lasting invariants — source immutability, ODR dedup semantics, the A5 source-lifetime requirement, the no-fork constraint and its reasoning — get harvested into code comments or `lib/Linker/CLAUDE.md` and `lib/Tools/CLAUDE.md` before it goes.
