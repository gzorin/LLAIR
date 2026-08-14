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

## A2 — ODR dedup for already-defined symbols

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

## A3 — Relocate type caches to `LLAIRContextImpl`

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

## A4 — Symbol index on `llair::Module`

Pure addition; nothing consumes it in this milestone.

```cpp
// [new] llair::Module
const llvm::GlobalValue *lookupDefinition(llvm::StringRef) const;   // lazily built
void                     invalidateSymbolIndex();
```

One pass over `global_values()`, no instruction walking. It belongs on `llair::Module` rather than `Linker` because a source permutation module is indexed once and reused by every `dst` that pulls from it. `syncMetadata()` is the existing seam where the wrapper reconciles with a mutated `llvm::Module`; hang invalidation there.

**Gate.** For every module in the corpus, `lookupDefinition` agrees with a brute-force `global_values()` scan on every name present, and returns null for a name known absent.

## A5 — Reachability-driven pull

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

## A6 — Explicit variation points and entry-level permutation key

Inferring the frontier from `isDeclarationForLinker()` conflates symbols intended to bind per permutation with symbols merely not yet pulled. A declared set gives validation (every point bound before handing IR downstream, rather than discovering an unresolved call in the driver) and the key defined under *Shared components*.

**Amended from v1:** the key is computed per entry point over that entry's reachable subgraph, not per library. B3 makes this the natural granularity.

The payoff is upstream of everything else here — the cheapest link is the one skipped because the key already maps to a compiled `MTLLibrary`.

**Gate.** Two independent constructions of the same binding set produce equal keys; any single binding change produces a different key; a binding change affecting only the fragment subgraph leaves the vertex key unchanged.

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

- **Opaque-pointer migration.** A0's harvested note and A3's context-scoped canonical struct table are the enablers: receiver types come from a name lookup, not `getElementType()`.
- **LLVM 17+ upgrade.** `PassManagerBuilder`, the legacy pass manager, `llvm::Optional`, and `PointerType::getElementType()` all block it. B2's outcome determines how much of `finalizeLibrary` survives the port.
- **Threading.** `LLVMContext` is not thread-safe. If permutation builds move to workers, each owns a context, and neither source modules nor the A3 caches cross worker boundaries — the ThinLTO shape is bitcode buffers parsed lazily per context with per-context copies of both tables. Cheaper to decide before A3 lands. The `contexts::llvm_to_llair()` global `std::map` is the one unguarded static either way; make it a `DenseMap` regardless.
- **`makeLibraryWithLLD` path.** Untouched by this plan. It already does reachability properly via internalize-to-entry-points plus `GlobalDCE`, at the cost of a subprocess.

## Correctness backlog

Independent, cheap, no ordering constraints:

- `llvm.module.flags` is dropped from `src` entirely when `dst` has any, rather than merged under the flags' own behavior semantics.
- Non-"once" named metadata operands are appended unconditionally, so linking the same source twice duplicates them.
- `s_once_metadata_names` is a `std::set<StringRef>` of six entries — `StringSwitch` or a sorted array.
- `test_cxx_identifier_regex` is unused.
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
- **A3 widens the scope of mutable state.** A stale entry in a context-scoped `d_type_map` corrupts every permutation rather than one. Types are immortal so it should be safe by construction — assert that a remapped type's context matches rather than trusting the argument.
- **A5 and A6 may be redundant with each other.** High cache hit rate on the permutation key sharply reduces A5's marginal value. B4's gate answers this; run it early.
- **B3's win scales with E.** If a typical permutation library has one or two entry points, the clone reduction is small and B2's outcome matters far more.
- **Upstream floor_llvm sync is now a standing dependency.** The version tables and `llvm.ident` strings need updating per macOS release. If a sync ever changes `WriteMetalLibToFile`'s entry-point handling, B3's assumption that a single-entry module produces one well-formed container needs rechecking.

## Deletion

This document is scaffolding. Lasting invariants — source immutability, ODR dedup semantics, the A5 source-lifetime requirement, the no-fork constraint and its reasoning — get harvested into code comments or `lib/Linker/CLAUDE.md` and `lib/Tools/CLAUDE.md` before it goes.
