#include <llair/IR/EntryPoint.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Linker/Linker.h>

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/MetalLibWriter.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace {

// Regression test for A1 (docs/linker-permutation-plan.md): linkModule() must
// not mutate distinct metadata nodes in the source module. A DIGlobalVariable
// attached to a global variable is exactly this shape -- a distinct node with
// a scope pointer -- and goes through the code path that used to reuse the
// source's node by identity (RF_ReuseAndMutateDistinctMDs) instead of cloning
// it (RF_None). Reuse-by-identity is what corrupts a source relinked into
// many destinations: two permutations would end up sharing the same node.
llvm::DIGlobalVariableExpression *
getDebugInfoForGlobal(llair::Module &module, llvm::StringRef name) {
    auto *gv = llvm::cast<llvm::GlobalVariable>(module.getLLModule()->getNamedValue(name));

    llvm::SmallVector<llvm::DIGlobalVariableExpression *, 1> dbg;
    gv->getDebugInfo(dbg);
    assert(dbg.size() == 1);

    return dbg[0];
}

void
testLinkerPreservesSourceModuleDebugInfo() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    llair::Module src("src", context);
    auto         *src_module = src.getLLModule();

    llvm::DIBuilder builder(*src_module);

    auto *file = builder.createFile("src.metal", "/tmp");
    auto *cu   = builder.createCompileUnit(llvm::dwarf::DW_LANG_C99, file, "llair-test",
                                            false, "", 0);

    auto *int_ty = llvm::Type::getInt32Ty(llcontext);
    auto *gv     = new llvm::GlobalVariable(*src_module, int_ty, true,
                                            llvm::GlobalValue::ExternalLinkage,
                                            llvm::ConstantInt::get(int_ty, 0), "g");

    auto *basic_ty = builder.createBasicType("int", 32, llvm::dwarf::DW_ATE_signed);
    auto *gv_expr  = builder.createGlobalVariableExpression(cu, "g", "g", file, 1, basic_ty,
                                                             /*IsLocalToUnit=*/false);
    gv->addDebugInfo(gv_expr);

    builder.finalize();

    auto *original_scope = gv_expr->getVariable()->getScope();

    auto *dbg_cu_md = src_module->getNamedMetadata("llvm.dbg.cu");
    assert(dbg_cu_md && dbg_cu_md->getNumOperands() == 1);
    auto *original_cu_operand = dbg_cu_md->getOperand(0);

    llair::Module dst1("dst1", context);
    llair::linkModules(&dst1, &src);

    // The source must not be touched by linking it:
    assert(gv_expr->getVariable()->getScope() == original_scope);
    assert(dbg_cu_md->getOperand(0) == original_cu_operand);

    // The destination must get its own clone, not a shared reference to the
    // source's node -- this is the part RF_ReuseAndMutateDistinctMDs violated.
    auto *dst1_gv_expr = getDebugInfoForGlobal(dst1, "g");
    assert(dst1_gv_expr != gv_expr);
    assert(dst1_gv_expr->getVariable()->getScope() != original_scope);

    llair::Module dst2("dst2", context);
    llair::linkModules(&dst2, &src);

    assert(gv_expr->getVariable()->getScope() == original_scope);
    assert(dbg_cu_md->getOperand(0) == original_cu_operand);

    // Two independent permutations must not end up sharing the same clone:
    auto *dst2_gv_expr = getDebugInfoForGlobal(dst2, "g");
    assert(dst2_gv_expr != gv_expr);
    assert(dst2_gv_expr->getVariable()->getScope() != original_scope);
    assert(dst2_gv_expr != dst1_gv_expr);

    std::cerr << "testLinkerPreservesSourceModuleDebugInfo: OK" << std::endl;
}

llvm::Function *
createSimpleFunction(llvm::Module &module, llvm::StringRef name,
                     llvm::GlobalValue::LinkageTypes linkage) {
    auto *fn_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(module.getContext()), false);
    auto *fn    = llvm::Function::Create(fn_ty, linkage, name, module);

    auto *bb = llvm::BasicBlock::Create(module.getContext(), "entry", fn);
    llvm::ReturnInst::Create(module.getContext(), bb);

    return fn;
}

// Regression test for A2 (docs/linker-permutation-plan.md): linkModule() must
// deduplicate an already-defined ODR symbol instead of blindly creating a new
// GlobalValue and letting the module's symbol table rename it to "name.1".
// Metal header-inlined linkonce_odr functions and constant tables are
// redefined by every source module that includes the header, so relinking
// several such modules into one destination hits this path every time.
void
testLinkerDedupesODRDefinitions() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    auto build_source = [&](llvm::StringRef module_name, llvm::StringRef unique_name) {
        auto module = std::make_unique<llair::Module>(module_name, context);
        auto *m     = module->getLLModule();

        // Shared, header-inlined helper -- redefined by every source module:
        createSimpleFunction(*m, "helper", llvm::GlobalValue::LinkOnceODRLinkage);

        // Module-unique function:
        createSimpleFunction(*m, unique_name, llvm::GlobalValue::ExternalLinkage);

        // Shared, header-inlined constant table -- same shape, for globals:
        auto *i32_ty = llvm::Type::getInt32Ty(llcontext);
        new llvm::GlobalVariable(*m, i32_ty, true, llvm::GlobalValue::LinkOnceODRLinkage,
                                 llvm::ConstantInt::get(i32_ty, 42), "table");

        return module;
    };

    auto src1 = build_source("src1", "unique1");
    auto src2 = build_source("src2", "unique2");

    llair::Module dst("dst_odr", context);
    llair::linkModules(&dst, src1.get());
    llair::linkModules(&dst, src2.get());

    auto *dst_module = dst.getLLModule();

    assert(dst_module->getFunction("helper"));
    assert(!dst_module->getFunction("helper.1"));
    assert(std::distance(dst_module->begin(), dst_module->end()) == 3); // helper, unique1, unique2

    assert(dst_module->getGlobalVariable("table"));
    assert(!dst_module->getNamedValue("table.1"));
    assert(std::distance(dst_module->global_begin(), dst_module->global_end()) == 1);

    assert(!llvm::verifyModule(*dst_module));

    std::cerr << "testLinkerDedupesODRDefinitions: OK" << std::endl;
}

// Regression test for A2's decided exception: an available_externally
// definition in dst is a stand-in, not a real one, so a later source
// module's real definition must replace it rather than being deduped away
// or renamed alongside it.
void
testLinkerPrefersRealDefinitionOverAvailableExternally() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    llair::Module stand_in_src("stand_in", context);
    createSimpleFunction(*stand_in_src.getLLModule(), "f", llvm::GlobalValue::AvailableExternallyLinkage);

    llair::Module real_src("real", context);
    createSimpleFunction(*real_src.getLLModule(), "f", llvm::GlobalValue::ExternalLinkage);

    llair::Module dst("dst_ae", context);
    llair::linkModules(&dst, &stand_in_src);
    llair::linkModules(&dst, &real_src);

    auto *dst_module = dst.getLLModule();
    auto *f          = dst_module->getFunction("f");

    assert(f);
    assert(!dst_module->getFunction("f.1"));
    assert(f->getLinkage() == llvm::GlobalValue::ExternalLinkage);
    assert(!f->isDeclaration());

    assert(!llvm::verifyModule(*dst_module));

    std::cerr << "testLinkerPrefersRealDefinitionOverAvailableExternally: OK" << std::endl;
}

// Regression test for A3 (docs/relocate-type-caches.md): a LinkerTypeCache
// shared across separate linkModules() calls must canonicalize identified
// structs that LLVM's own uniquing split into "struct.Foo"/"struct.Foo.1",
// even though each link constructs an independent Linker/TypeMapper.
void
testLinkerCanonicalizesStructsAcrossSeparateLinks() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    // What two separately-parsed bitcode files colliding on a name produce:
    auto *foo0 = llvm::StructType::create(llcontext, "struct.Foo");
    auto *foo1 = llvm::StructType::create(llcontext, "struct.Foo");
    assert(foo0->getName() == "struct.Foo");
    // LLVM disambiguates the colliding name with a numeric suffix; the integer it
    // starts from is version-dependent (.0 or .1), so assert only that it renamed.
    assert(foo1->getName() != foo0->getName());
    assert(foo1->getName().startswith("struct.Foo."));

    auto build_source = [&](llvm::StringRef module_name, llvm::StructType *foo,
                            llvm::StringRef global_name) {
        auto module = std::make_unique<llair::Module>(module_name, context);
        new llvm::GlobalVariable(*module->getLLModule(), llvm::PointerType::get(foo, 0), false,
                                 llvm::GlobalValue::ExternalLinkage, nullptr, global_name);
        return module;
    };

    auto src0 = build_source("src0", foo0, "g0");
    auto src1 = build_source("src1", foo1, "g1");

    llair::Module dst("dst_canon", context);

    llair::LinkerTypeCache type_cache;
    llair::linkModules(&dst, src0.get(), type_cache);
    llair::linkModules(&dst, src1.get(), type_cache);

    auto *dst_module = dst.getLLModule();
    auto *g0         = dst_module->getNamedGlobal("g0");
    auto *g1         = dst_module->getNamedGlobal("g1");

    assert(g0 && g1);
    assert(g0->getValueType() == g1->getValueType());

    std::cerr << "testLinkerCanonicalizesStructsAcrossSeparateLinks: OK" << std::endl;
}

// Regression test for A3's placeholder fix: remapType() must terminate on a
// self-referential struct instead of recursing forever into its own fields.
void
testLinkerHandlesSelfReferentialStructTypes() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    llair::Module src("src_selfref", context);

    // %struct.Node = type { %struct.Node*, i32 }
    auto *node_ty = llvm::StructType::create(llcontext, "struct.Node");
    node_ty->setBody({ llvm::PointerType::get(node_ty, 0),
                       llvm::Type::getInt32Ty(llcontext) });

    new llvm::GlobalVariable(*src.getLLModule(), llvm::PointerType::get(node_ty, 0), false,
                             llvm::GlobalValue::ExternalLinkage, nullptr, "head");

    llair::Module dst("dst_selfref", context);
    llair::linkModules(&dst, &src); // must return, not hang or stack-overflow

    assert(!llvm::verifyModule(*dst.getLLModule()));

    std::cerr << "testLinkerHandlesSelfReferentialStructTypes: OK" << std::endl;
}

// Gate for A4 (docs/linker-permutation-plan.md): Module::lookupDefinition()
// must agree with a brute-force global_values() scan -- returning the defining
// GlobalValue for a name and null for a name that is only declared or absent --
// and its lazily-built index must be dropped by invalidateSymbolIndex() and by
// syncMetadata(), the seam where the wrapper reconciles with a mutated module.
void
testModuleSymbolIndexMatchesBruteForceScan() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    llair::Module module("src_symidx", context);
    auto         *m = module.getLLModule();

    // A defining function and a declaration-only function:
    createSimpleFunction(*m, "defined_fn", llvm::GlobalValue::ExternalLinkage);
    auto *decl_fn_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(llcontext), false);
    llvm::Function::Create(decl_fn_ty, llvm::GlobalValue::ExternalLinkage, "declared_fn", m);

    // A defining global (has an initializer) and a declaration-only global:
    auto *i32_ty = llvm::Type::getInt32Ty(llcontext);
    new llvm::GlobalVariable(*m, i32_ty, true, llvm::GlobalValue::ExternalLinkage,
                             llvm::ConstantInt::get(i32_ty, 7), "defined_gv");
    new llvm::GlobalVariable(*m, i32_ty, false, llvm::GlobalValue::ExternalLinkage,
                             nullptr, "declared_gv");

    auto brute_force = [&](llvm::StringRef name) -> const llvm::GlobalValue * {
        for (const auto &gv : m->global_values()) {
            if (!gv.isDeclaration() && gv.getName() == name) {
                return &gv;
            }
        }
        return nullptr;
    };

    for (llvm::StringRef name : {"defined_fn", "declared_fn", "defined_gv", "declared_gv"}) {
        assert(module.lookupDefinition(name) == brute_force(name));
    }

    assert(module.lookupDefinition("defined_fn"));
    assert(module.lookupDefinition("defined_gv"));
    assert(!module.lookupDefinition("declared_fn")); // declared, not defined
    assert(!module.lookupDefinition("declared_gv"));
    assert(!module.lookupDefinition("absent"));

    // A definition added after the index was built is invisible until the index
    // is dropped -- proving both lazy rebuild and invalidateSymbolIndex().
    createSimpleFunction(*m, "late_fn", llvm::GlobalValue::ExternalLinkage);
    assert(!module.lookupDefinition("late_fn"));
    module.invalidateSymbolIndex();
    assert(module.lookupDefinition("late_fn") == brute_force("late_fn"));

    // syncMetadata() is the production seam and must also drop the index.
    createSimpleFunction(*m, "later_fn", llvm::GlobalValue::ExternalLinkage);
    assert(!module.lookupDefinition("later_fn"));
    module.syncMetadata();
    assert(module.lookupDefinition("later_fn"));

    std::cerr << "testModuleSymbolIndexMatchesBruteForceScan: OK" << std::endl;
}

// Corpus shared by the A5 pull tests. Module A defines `entry` (which calls
// `helper` and references global `g` through a ConstantExpr) but only declares
// `helper`/`g`; it also defines a linkonce_odr `shared` and an air.static_init
// `ctor` that calls `shared`. Module B defines `helper`, `g`, a `dead_fn` that
// nothing references, and its own linkonce_odr `shared`.
struct PullCorpus {
    std::unique_ptr<llair::Module> a, b;
};

PullCorpus
buildPullCorpus(llair::LLAIRContext &context) {
    auto &llctx = context.getLLContext();

    auto A  = std::make_unique<llair::Module>("pull_A", context);
    auto B  = std::make_unique<llair::Module>("pull_B", context);
    auto *mA = A->getLLModule();
    auto *mB = B->getLLModule();

    auto *i32      = llvm::Type::getInt32Ty(llctx);
    auto *i64      = llvm::Type::getInt64Ty(llctx);
    auto *void_fn  = llvm::FunctionType::get(llvm::Type::getVoidTy(llctx), false);

    // Module A: declarations of B's definitions, plus entry/shared/ctor.
    auto *g_decl      = new llvm::GlobalVariable(*mA, i32, false,
                                                 llvm::GlobalValue::ExternalLinkage, nullptr, "g");
    auto *helper_decl = llvm::Function::Create(void_fn, llvm::GlobalValue::ExternalLinkage,
                                               "helper", mA);

    auto *entry = llvm::Function::Create(void_fn, llvm::GlobalValue::ExternalLinkage, "entry", mA);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(llctx, "entry", entry));
        auto *slot = b.CreateAlloca(i64);
        // Reference g only through a ConstantExpr, exercising the walk's descent.
        b.CreateStore(llvm::ConstantExpr::getPtrToInt(g_decl, i64), slot);
        b.CreateCall(helper_decl);
        b.CreateRetVoid();
    }

    createSimpleFunction(*mA, "shared", llvm::GlobalValue::LinkOnceODRLinkage);

    auto *ctor = llvm::Function::Create(void_fn, llvm::GlobalValue::InternalLinkage, "ctor", mA);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(llctx, "entry", ctor));
        b.CreateCall(mA->getFunction("shared"));
        b.CreateRetVoid();
    }
    ctor->setSection("air.static_init");

    // Module B: real definitions.
    createSimpleFunction(*mB, "helper", llvm::GlobalValue::ExternalLinkage);
    new llvm::GlobalVariable(*mB, i32, false, llvm::GlobalValue::ExternalLinkage,
                             llvm::ConstantInt::get(i32, 7), "g");
    createSimpleFunction(*mB, "dead_fn", llvm::GlobalValue::ExternalLinkage);
    createSimpleFunction(*mB, "shared", llvm::GlobalValue::LinkOnceODRLinkage);

    return {std::move(A), std::move(B)};
}

// Register `entry` as a compute entry point on `module` and mirror it into
// air.kernel named metadata, the form a bitcode load would carry.
void
materializeComputeEntryPoint(llair::Module &module) {
    auto *entry = module.getLLModule()->getFunction("entry");
    auto *ep    = llair::ComputeEntryPoint::Create(entry, &module);
    module.getLLModule()->getOrInsertNamedMetadata("air.kernel")->addOperand(ep->metadata());
}

// A1 (mechanism): the pull path clones exactly the transitive closure of the
// roots -- reaching a callee, a ConstantExpr-referenced global, an ODR helper
// pulled through a ctor, and a static_init ctor -- while leaving dead code out.
void
testLinkerPullReachesRequiredClosure() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    auto corpus = buildPullCorpus(context);

    llair::Module          dst("dst_pull_closure", context);
    llair::LinkerTypeCache cache;
    llair::Linker          linker(dst, cache);
    linker.addModule(corpus.a.get());
    linker.addModule(corpus.b.get());
    linker.require("entry");
    linker.resolve();
    dst.syncMetadata();

    auto *m = dst.getLLModule();

    assert(m->getFunction("entry") && !m->getFunction("entry")->isDeclaration());
    assert(m->getFunction("helper") && !m->getFunction("helper")->isDeclaration());
    assert(m->getFunction("shared") && !m->getFunction("shared")->isDeclaration());
    assert(m->getFunction("ctor") && !m->getFunction("ctor")->isDeclaration());

    // Cross-module join: A declared g, B defined it; the pull wires B's initializer.
    auto *g = m->getNamedGlobal("g");
    assert(g && g->hasInitializer());

    assert(!m->getFunction("dead_fn")); // unreachable -- never pulled

    // A definition is pulled once and joined by name, so no ".1" rename occurs.
    assert(!m->getNamedValue("shared.1"));
    assert(!m->getNamedValue("helper.1"));

    auto *ctors = m->getNamedGlobal("llvm.global_ctors");
    assert(ctors && ctors->hasInitializer());
    assert(llvm::cast<llvm::ConstantArray>(ctors->getInitializer())->getNumOperands() == 1);

    assert(!llvm::verifyModule(*m));

    std::cerr << "testLinkerPullReachesRequiredClosure: OK" << std::endl;
}

void
pruneToRoots(llvm::Module &m, const std::set<std::string> &roots) {
    llvm::legacy::PassManager mpm;
    mpm.add(llvm::createInternalizePass([roots](const llvm::GlobalValue &gv) -> bool {
        return roots.count(gv.getName().str()) == 1;
    }));
    mpm.add(llvm::createGlobalDCEPass());
    mpm.run(m);
}

// Two modules compare equal modulo symbol ordering: same set of *defined* names
// and the same function/global/alias counts. Linkage and metadata are ignored --
// internalize rewrites the former and GlobalDCE nulls references in the latter.
void
assertSameShape(const llvm::Module *lhs, const llvm::Module *rhs) {
    auto shape = [](const llvm::Module *m) {
        std::vector<std::string> defined;
        std::size_t              n_func = 0, n_global = 0, n_alias = 0;
        for (const auto &f : *m) {
            ++n_func;
            if (!f.isDeclaration()) {
                defined.push_back(f.getName().str());
            }
        }
        for (const auto &g : m->globals()) {
            ++n_global;
            if (g.hasInitializer()) {
                defined.push_back(g.getName().str());
            }
        }
        for (const auto &a : m->aliases()) {
            ++n_alias;
            defined.push_back(a.getName().str());
        }
        std::sort(defined.begin(), defined.end());
        return std::make_tuple(defined, n_func, n_global, n_alias);
    };

    assert(shape(lhs) == shape(rhs));
    assert(!llvm::verifyModule(*lhs));
    assert(!llvm::verifyModule(*rhs));
}

// A5's load-bearing differential gate: the pull must produce the same defined
// symbols as the eager path followed by internalize(full root set)+GlobalDCE.
void
testLinkerPullMatchesEagerThenPrune() {
    // The full pull root set for this corpus: the explicit require plus the
    // auto-seeded air.static_init ctor. (No llvm.used here.)
    const std::set<std::string> roots = {"entry", "ctor"};

    // Variant 1: closure driven by an explicit require().
    {
        llvm::LLVMContext   llcontext;
        llair::LLAIRContext context(llcontext);

        // Link the definer (B) before the module that only declares its globals
        // (A): the eager global loop joins a definition onto an existing dst
        // declaration only in that order, so this is its correct-baseline order.
        auto                   eager = buildPullCorpus(context);
        llair::Module          dst_eager("dst_eager", context);
        llair::LinkerTypeCache eager_cache;
        llair::linkModules(&dst_eager, eager.b.get(), eager_cache);
        llair::linkModules(&dst_eager, eager.a.get(), eager_cache);
        pruneToRoots(*dst_eager.getLLModule(), roots);

        auto                   pull = buildPullCorpus(context);
        llair::Module          dst_pull("dst_pull", context);
        llair::LinkerTypeCache pull_cache;
        llair::Linker          linker(dst_pull, pull_cache);
        linker.addModule(pull.a.get());
        linker.addModule(pull.b.get());
        linker.require("entry");
        linker.resolve();
        dst_pull.syncMetadata();

        assertSameShape(dst_eager.getLLModule(), dst_pull.getLLModule());
    }

    // Variant 2: no explicit require -- the closure is driven by an auto-seeded
    // entry-point root, and the copied air.kernel metadata rematerializes it.
    {
        llvm::LLVMContext   llcontext;
        llair::LLAIRContext context(llcontext);

        auto eager = buildPullCorpus(context);
        materializeComputeEntryPoint(*eager.a);
        llair::Module          dst_eager("dst_eager2", context);
        llair::LinkerTypeCache eager_cache;
        llair::linkModules(&dst_eager, eager.b.get(), eager_cache);
        llair::linkModules(&dst_eager, eager.a.get(), eager_cache);
        pruneToRoots(*dst_eager.getLLModule(), roots);

        auto pull = buildPullCorpus(context);
        materializeComputeEntryPoint(*pull.a);
        llair::Module          dst_pull("dst_pull2", context);
        llair::LinkerTypeCache pull_cache;
        llair::Linker          linker(dst_pull, pull_cache);
        linker.addModule(pull.a.get());
        linker.addModule(pull.b.get());
        linker.resolve();
        dst_pull.syncMetadata();

        assert(dst_pull.entry_point_begin() != dst_pull.entry_point_end());
        assert(dst_pull.entry_point_begin()->getName() == "entry");

        assertSameShape(dst_eager.getLLModule(), dst_pull.getLLModule());
    }

    std::cerr << "testLinkerPullMatchesEagerThenPrune: OK" << std::endl;
}

// An i32()-returning function whose body is a single `ret <value>`, so distinct
// `value`s give it distinct IR -- a stand-in for a variation point bound to
// different definitions across permutations.
llvm::Function *
createReturningFunction(llvm::Module &module, llvm::StringRef name,
                        llvm::GlobalValue::LinkageTypes linkage, int value) {
    auto *i32   = llvm::Type::getInt32Ty(module.getContext());
    auto *fn_ty = llvm::FunctionType::get(i32, false);
    auto *fn    = llvm::Function::Create(fn_ty, linkage, name, module);

    llvm::IRBuilder<> b(llvm::BasicBlock::Create(module.getContext(), "entry", fn));
    b.CreateRet(llvm::ConstantInt::get(i32, value));

    return fn;
}

// One permutation for the A6 key tests. A shape module declares two variation
// points (`vbxdf`, `fbxdf`) and defines two entries plus a shared helper:
// `vmain` reaches `vbxdf` and `common`; `fmain` reaches `fbxdf` and `common`.
// Neither entry reaches the other's variation point. A defs module supplies the
// chosen definitions, whose bodies are controlled by `vbxdf_value`/`fbxdf_value`.
// `with_defs == false` leaves the frontier unbound, for the validation test.
struct PermutationKeys {
    uint64_t vmain = 0, fmain = 0;
    bool     bound = false;
};

PermutationKeys
buildPermutationKeys(int vbxdf_value, int fbxdf_value, bool with_defs = true) {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    auto  shape  = std::make_unique<llair::Module>("a6_shape", context);
    auto *mShape = shape->getLLModule();

    auto *i32     = llvm::Type::getInt32Ty(llcontext);
    auto *i32_fn  = llvm::FunctionType::get(i32, false);
    auto *void_fn = llvm::FunctionType::get(llvm::Type::getVoidTy(llcontext), false);

    auto *vbxdf_decl =
        llvm::Function::Create(i32_fn, llvm::GlobalValue::ExternalLinkage, "vbxdf", mShape);
    auto *fbxdf_decl =
        llvm::Function::Create(i32_fn, llvm::GlobalValue::ExternalLinkage, "fbxdf", mShape);

    // Shared, non-varying code reachable from both entries: its content must not
    // enter either key, since it is not a variation point.
    auto *common = createReturningFunction(*mShape, "common",
                                           llvm::GlobalValue::LinkOnceODRLinkage, 99);

    auto build_entry = [&](llvm::StringRef name, llvm::Function *bxdf) {
        auto *entry = llvm::Function::Create(void_fn, llvm::GlobalValue::ExternalLinkage, name,
                                             mShape);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(llcontext, "entry", entry));
        b.CreateCall(common);
        b.CreateCall(bxdf);
        b.CreateRetVoid();
        return entry;
    };
    build_entry("vmain", vbxdf_decl);
    build_entry("fmain", fbxdf_decl);

    auto  defs  = std::make_unique<llair::Module>("a6_defs", context);
    auto *mDefs = defs->getLLModule();
    if (with_defs) {
        createReturningFunction(*mDefs, "vbxdf", llvm::GlobalValue::ExternalLinkage, vbxdf_value);
        createReturningFunction(*mDefs, "fbxdf", llvm::GlobalValue::ExternalLinkage, fbxdf_value);
    }

    llair::Module          dst("a6_dst", context);
    llair::LinkerTypeCache cache;
    llair::Linker          linker(dst, cache);
    linker.addModule(shape.get());
    linker.addModule(defs.get());
    linker.addVariationPoint("vbxdf");
    linker.addVariationPoint("fbxdf");
    linker.require("vmain");
    linker.require("fmain");
    linker.resolve();
    dst.syncMetadata();

    auto *m = dst.getLLModule();
    return {linker.permutationKey(m->getFunction("vmain")),
            linker.permutationKey(m->getFunction("fmain")),
            linker.variationPointsBound()};
}

// A6 gate (docs/linker-permutation-plan.md): the entry-level permutation key is
// stable for equal bindings, changes when any reached binding changes, and is
// isolated to the entry's reachable subgraph -- a fragment-only binding change
// leaves the vertex key untouched and vice versa.
void
testPermutationKeyDiscriminatesBindings() {
    // Gate 1: two independent constructions of the same binding set agree.
    auto base  = buildPermutationKeys(1, 2);
    auto again = buildPermutationKeys(1, 2);
    assert(base.bound && again.bound);
    assert(base.vmain == again.vmain);
    assert(base.fmain == again.fmain);

    // Gate 3: a change reaching only the fragment subgraph moves the fragment
    // key but leaves the vertex key unchanged.
    auto frag_changed = buildPermutationKeys(1, 3);
    assert(frag_changed.fmain != base.fmain);
    assert(frag_changed.vmain == base.vmain);

    // Gate 2 (and the symmetric case of gate 3): a change reaching only the
    // vertex subgraph moves the vertex key but leaves the fragment key unchanged.
    auto vert_changed = buildPermutationKeys(4, 2);
    assert(vert_changed.vmain != base.vmain);
    assert(vert_changed.fmain == base.fmain);

    // Distinct variation points make the two entries' keys differ even at equal
    // bound values -- the folded name disambiguates them.
    auto equal_values = buildPermutationKeys(5, 5);
    assert(equal_values.vmain != equal_values.fmain);

    std::cerr << "testPermutationKeyDiscriminatesBindings: OK" << std::endl;
}

// A6 validation: variationPointsBound() is true only once every declared point
// has resolved to a definition; an unbound point (no defs module) reports false,
// and its key contribution is well-defined (the point is still reachable).
void
testVariationPointBindingValidation() {
    auto bound = buildPermutationKeys(1, 2, /*with_defs=*/true);
    assert(bound.bound);

    auto unbound = buildPermutationKeys(1, 2, /*with_defs=*/false);
    assert(!unbound.bound);

    std::cerr << "testVariationPointBindingValidation: OK" << std::endl;
}

// Serialize a module the same way WriteMetalLibToFile's per-entry write does.
std::string
serializeAsBitcode140(llvm::Module &module) {
    std::string           buffer;
    llvm::raw_string_ostream os(buffer);
    llvm::WriteBitcodeToFile140(module, os);
    os.flush();
    return buffer;
}

// Gate 1 for B3 (docs/linker-permutation-plan.md): filtering CloneModule's
// definitions to an entry's reachable set must not change the final pruned
// module -- internalize+GlobalOpt+GlobalDCE already discard everything a
// filtered clone would have skipped, so cloning full bodies for code that
// gets discarded anyway is pure waste, not a correctness dependency. Two
// entries share `common`; each also has a private helper/global nothing else
// reaches.
void
testBuildEntryModuleByteIdenticalToUnfilteredPath() {
    llvm::LLVMContext llcontext;
    llvm::Module       module("b3_gate1", llcontext);
    auto              *i32 = llvm::Type::getInt32Ty(llcontext);

    auto *common = createReturningFunction(module, "common", llvm::GlobalValue::ExternalLinkage, 1);

    auto *priv_a = new llvm::GlobalVariable(module, i32, true, llvm::GlobalValue::InternalLinkage,
                                            llvm::ConstantInt::get(i32, 11), "priv_a");
    auto *only_a = llvm::Function::Create(llvm::FunctionType::get(i32, false),
                                          llvm::GlobalValue::InternalLinkage, "only_a", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(llcontext, "entry", only_a));
        b.CreateRet(b.CreateLoad(i32, priv_a));
    }

    auto *priv_b = new llvm::GlobalVariable(module, i32, true, llvm::GlobalValue::InternalLinkage,
                                            llvm::ConstantInt::get(i32, 22), "priv_b");
    auto *only_b = llvm::Function::Create(llvm::FunctionType::get(i32, false),
                                          llvm::GlobalValue::InternalLinkage, "only_b", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(llcontext, "entry", only_b));
        b.CreateRet(b.CreateLoad(i32, priv_b));
    }

    auto build_entry = [&](llvm::StringRef name, llvm::Function *only) {
        auto *entry = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(llcontext), false),
            llvm::GlobalValue::ExternalLinkage, name, module);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(llcontext, "entry", entry));
        b.CreateCall(common);
        b.CreateCall(only);
        b.CreateRetVoid();
        return entry;
    };
    auto *entry_a = build_entry("entry_a", only_a);
    auto *entry_b = build_entry("entry_b", only_b);

    auto always_true = [](const llvm::GlobalValue *) { return true; };

    auto check_entry = [&](llvm::Function                                    *entry,
                           const std::set<const llvm::GlobalValue *> &reachable) {
        auto unfiltered = llvm::buildEntryModule(module, entry, always_true, 250);
        auto filtered   = llvm::buildEntryModule(
            module, entry,
            [&reachable](const llvm::GlobalValue *gv) { return reachable.count(gv) != 0; }, 250);

        assert(serializeAsBitcode140(*unfiltered) == serializeAsBitcode140(*filtered));
    };

    check_entry(entry_a, {entry_a, common, only_a, priv_a});
    check_entry(entry_b, {entry_b, common, only_b, priv_b});

    std::cerr << "testBuildEntryModuleByteIdenticalToUnfilteredPath: OK" << std::endl;
}

// Gate 2 for B3: buildEntryModule's cost under a real reachability predicate
// must track the entry's reachable set, not the whole module -- an
// accidentally-ignored ShouldCloneDefinition (still cloning every body) would
// scale with total module size instead.
void
testBuildEntryModuleCostScalesWithReachableSet() {
    auto build_and_time = [](unsigned pad_count) -> double {
        llvm::LLVMContext llcontext;
        llvm::Module       module("b3_gate2", llcontext);
        auto              *i32 = llvm::Type::getInt32Ty(llcontext);

        auto *entry = llvm::Function::Create(llvm::FunctionType::get(i32, false),
                                             llvm::GlobalValue::ExternalLinkage, "tiny_entry", module);
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(llcontext, "entry", entry));
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
        }

        // Unrelated padding: none of this is reachable from `entry`.
        for (unsigned i = 0; i < pad_count; ++i) {
            createReturningFunction(module, "pad" + std::to_string(i),
                                    llvm::GlobalValue::InternalLinkage, int(i));
        }

        std::unordered_set<const llvm::GlobalValue *> reachable{entry};
        auto predicate = [&reachable](const llvm::GlobalValue *gv) {
            return reachable.count(gv) != 0;
        };

        auto t0     = std::chrono::steady_clock::now();
        auto cloned = llvm::buildEntryModule(module, entry, predicate, 250);
        auto t1     = std::chrono::steady_clock::now();
        (void)cloned;

        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };

    auto best_of = [&](unsigned pad_count) {
        double best = std::numeric_limits<double>::infinity();
        for (int rep = 0; rep < 3; ++rep) {
            best = std::min(best, build_and_time(pad_count));
        }
        return best;
    };

    constexpr unsigned kSmall = 500, kLarge = 5000; // 10x the unrelated module size
    auto               small_ms = best_of(kSmall);
    auto               large_ms = best_of(kLarge);

    std::cerr << "testBuildEntryModuleCostScalesWithReachableSet: " << kSmall
              << " unrelated fns = " << small_ms << "ms, " << kLarge
              << " unrelated fns = " << large_ms << "ms" << std::endl;

    // Not a strict scaling proof -- a generous bound that still catches an
    // accidentally-still-O(module size) implementation (which would come much
    // closer to a 10x blowup than this).
    assert(large_ms < small_ms * 5.0 + 5.0 /* floor for sub-ms noise */);

    std::cerr << "testBuildEntryModuleCostScalesWithReachableSet: OK" << std::endl;
}

// Gate 3 for B3, the correction's load-bearing regression test: two entries
// with disjoint function constants must each retain only their own constant
// in air.function_constants after buildEntryModule. A filtered-out constant's
// global survives CloneModule as an external declaration -- only the GlobalDCE
// added alongside the filtered clone actually erases it, which is what nulls
// the metadata operand the writer's null-operand idiom checks. Verified this
// fails without that GlobalDCE call (temporarily commented out) before landing.
void
testBuildEntryModuleDisjointFunctionConstants() {
    llvm::LLVMContext llcontext;
    llvm::Module       module("b3_gate3", llcontext);
    auto              *i32 = llvm::Type::getInt32Ty(llcontext);

    auto make_constant_global = [&](llvm::StringRef name) {
        return new llvm::GlobalVariable(module, i32, false, llvm::GlobalValue::ExternalLinkage,
                                        nullptr, name);
    };
    auto *gv_a = make_constant_global("fc_a_gv");
    auto *gv_b = make_constant_global("fc_b_gv");

    auto build_entry = [&](llvm::StringRef name, llvm::GlobalVariable *fc_gv) {
        auto *entry = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(llcontext), false),
            llvm::GlobalValue::ExternalLinkage, name, module);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(llcontext, "entry", entry));
        b.CreateLoad(i32, fc_gv);
        b.CreateRetVoid();
        return entry;
    };
    auto *entry_a = build_entry("fc_entry_a", gv_a);
    auto *entry_b = build_entry("fc_entry_b", gv_b);

    auto make_fc_node = [&](llvm::GlobalVariable *gv, llvm::StringRef type_name,
                           llvm::StringRef cnst_name, uint32_t index) {
        llvm::Metadata *ops[] = {
            llvm::ConstantAsMetadata::get(gv),
            llvm::MDString::get(llcontext, type_name),
            llvm::MDString::get(llcontext, cnst_name),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, index)),
        };
        return llvm::MDNode::get(llcontext, ops);
    };

    auto *fc_md = module.getOrInsertNamedMetadata("air.function_constants");
    fc_md->addOperand(make_fc_node(gv_a, "int", "fc_a", 0));
    fc_md->addOperand(make_fc_node(gv_b, "int", "fc_b", 1));

    auto reachable_for = [](llvm::Function *entry, llvm::GlobalVariable *gv) {
        return [entry, gv](const llvm::GlobalValue *g) { return g == entry || g == gv; };
    };

    auto cloned_a = llvm::buildEntryModule(module, entry_a, reachable_for(entry_a, gv_a), 250);
    auto cloned_b = llvm::buildEntryModule(module, entry_b, reachable_for(entry_b, gv_b), 250);

    auto surviving_constants = [](llvm::Module &m) {
        std::set<std::string> names;
        if (auto *md = m.getNamedMetadata("air.function_constants")) {
            for (auto *node : md->operands()) {
                if (node->getOperand(0).get() == nullptr) {
                    continue; // erased by GlobalDCE -- not this entry's constant
                }
                names.insert(llvm::cast<llvm::MDString>(node->getOperand(2))->getString().str());
            }
        }
        return names;
    };

    assert((surviving_constants(*cloned_a) == std::set<std::string>{"fc_a"}));
    assert((surviving_constants(*cloned_b) == std::set<std::string>{"fc_b"}));

    std::cerr << "testBuildEntryModuleDisjointFunctionConstants: OK" << std::endl;
}

// Regression test for a correctness bug found after B3 first landed, on a real
// corpus (examples/sg/triangle), not this synthetic one: an "air.static_init"
// function -- Metal's mechanism for copying a function-constant value into a
// global before any entry point executes -- has no call edge from any entry,
// so a per-entry reachability walk rooted only at the entry can never find it.
// Worse, the ctor's only real (non-metadata) use is usually the pointer to it
// in @llvm.global_ctors's initializer; without that array also surviving,
// GlobalOpt sees the ctor as unused and deletes it outright even once the
// ctor itself is marked reachable. Both gaps must be closed, or the shader
// that reads the global the ctor writes silently gets an uninitialized value.
void
testBuildEntryModulePreservesStaticInitCtor() {
    llvm::LLVMContext llcontext;
    llvm::Module       module("b3_static_init", llcontext);
    auto              *i32     = llvm::Type::getInt32Ty(llcontext);
    auto              *i8_ptr  = llvm::Type::getInt8PtrTy(llcontext);

    // A real Metal function-constant placeholder: an external declaration the
    // driver fills in at specialization time. Unlike a compile-time constant,
    // GlobalOpt's ctor evaluator cannot fold a load from this away, so the
    // ctor genuinely has to survive and run -- exactly what made the bug
    // observable on the real corpus and invisible with a foldable store.
    auto *fc_init = new llvm::GlobalVariable(
        module, i32, false, llvm::GlobalValue::ExternalLinkage, nullptr, "fc_init_placeholder");

    auto *fc_global = new llvm::GlobalVariable(
        module, i32, false, llvm::GlobalValue::InternalLinkage,
        llvm::UndefValue::get(i32), "fc_global");

    auto *ctor = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(llcontext), false),
        llvm::GlobalValue::InternalLinkage, "ctor", module);
    ctor->setSection("air.static_init");
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(llcontext, "entry", ctor));
        b.CreateStore(b.CreateLoad(i32, fc_init), fc_global);
        b.CreateRetVoid();
    }

    // @llvm.global_ctors = appending global [1 x {i32, void()*, i8*}] [...]
    auto *ctor_entry_ty = llvm::StructType::get(i32, ctor->getType(), i8_ptr);
    auto *ctors_ty      = llvm::ArrayType::get(ctor_entry_ty, 1);
    auto *ctor_entry    = llvm::ConstantStruct::get(
        ctor_entry_ty, { llvm::ConstantInt::get(i32, 65535), ctor,
                         llvm::ConstantPointerNull::get(i8_ptr) });
    new llvm::GlobalVariable(module, ctors_ty, false, llvm::GlobalValue::AppendingLinkage,
                             llvm::ConstantArray::get(ctors_ty, { ctor_entry }),
                             "llvm.global_ctors");

    auto *entry = llvm::Function::Create(llvm::FunctionType::get(i32, false),
                                         llvm::GlobalValue::ExternalLinkage, "entry", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(llcontext, "entry", entry));
        b.CreateRet(b.CreateLoad(i32, fc_global));
    }

    // Reproduce the bug: reachability from `entry` alone never reaches `ctor`
    // -- nothing in `entry`'s own body references it.
    std::set<const llvm::GlobalValue *> entry_only{ entry, fc_global };
    auto buggy = llvm::buildEntryModule(
        module, entry,
        [&entry_only](const llvm::GlobalValue *gv) { return entry_only.count(gv) != 0; }, 250);
    assert(!buggy->getFunction("ctor") || buggy->getFunction("ctor")->isDeclaration());

    // The fix: fold computeStaticInitRoots()'s closure into the predicate.
    auto static_init_roots = llvm::computeStaticInitRoots(module);
    auto fixed             = llvm::buildEntryModule(
        module, entry,
        [&entry_only, &static_init_roots](const llvm::GlobalValue *gv) {
            return entry_only.count(gv) != 0 || static_init_roots.count(gv) != 0;
        },
        250);

    auto *fixed_ctor = fixed->getFunction("ctor");
    assert(fixed_ctor && !fixed_ctor->isDeclaration());
    auto *fixed_ctors_gv = fixed->getNamedGlobal("llvm.global_ctors");
    assert(fixed_ctors_gv && fixed_ctors_gv->hasInitializer());

    std::cerr << "testBuildEntryModulePreservesStaticInitCtor: OK" << std::endl;
}

} // namespace

int
main(int argc, const char **argv) {
    std::cerr << "Hello, llair" << std::endl;

    std::unique_ptr<llvm::LLVMContext>   llcontext(new llvm::LLVMContext());
    std::unique_ptr<llair::LLAIRContext> context(new llair::LLAIRContext(*llcontext));

    std::unique_ptr<llair::Module> module(new llair::Module("test", *context));

#if defined(LLVM_ENABLE_DUMP)
    module->getLLModule()->dump();
#endif

    testLinkerPreservesSourceModuleDebugInfo();
    testLinkerDedupesODRDefinitions();
    testLinkerPrefersRealDefinitionOverAvailableExternally();
    testLinkerCanonicalizesStructsAcrossSeparateLinks();
    testLinkerHandlesSelfReferentialStructTypes();
    testModuleSymbolIndexMatchesBruteForceScan();
    testLinkerPullReachesRequiredClosure();
    testLinkerPullMatchesEagerThenPrune();
    testPermutationKeyDiscriminatesBindings();
    testVariationPointBindingValidation();
    testBuildEntryModuleByteIdenticalToUnfilteredPath();
    testBuildEntryModuleCostScalesWithReachableSet();
    testBuildEntryModuleDisjointFunctionConstants();
    testBuildEntryModulePreservesStaticInitCtor();
}
