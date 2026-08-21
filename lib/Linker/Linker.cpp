#include <llair/IR/Module.h>
#include <llair/IR/Class.h>
#include <llair/IR/Dispatcher.h>
#include <llair/IR/EntryPoint.h>
#include <llair/IR/Interface.h>
#include <llair/Linker/Linker.h>
#include <llair/IR/Module.h>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalObject.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/xxhash.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <algorithm>
#include <cctype>

using namespace llvm;

namespace llair {

namespace {

// The canonical identity of a C++ struct/class type as encoded in an LLVM
// identified-struct name: strip the `struct.`/`class.` prefix and the numeric
// `.N` suffix LLVM appends to disambiguate a name colliding with one already in
// the context. Returns `None` for names that don't fit the pattern.
llvm::Optional<llvm::StringRef>
canonicalStructIdentifier(llvm::StringRef name) {
    llvm::StringRef rest = name;

    if (!rest.consume_front("struct.") && !rest.consume_front("class.")) {
        return llvm::None;
    }

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

void
copyComdat(GlobalObject *Dst, const GlobalObject *Src) {
    const Comdat *SC = Src->getComdat();
    if (!SC)
        return;
    Comdat *DC = Dst->getParent()->getOrInsertComdat(SC->getName());
    DC->setSelectionKind(SC->getSelectionKind());
    Dst->setComdat(DC);
}

constexpr bool
isODR(llvm::GlobalValue::LinkageTypes L) {
    return L == llvm::GlobalValue::LinkOnceODRLinkage ||
           L == llvm::GlobalValue::WeakODRLinkage;
}

// Descend a use operand to the GlobalValue leaves it reaches: a GlobalValue is
// a leaf; a MetadataAsValue roots no reachability (metadata must never root the
// walk); any other Constant is descended through its operands, which carries the
// walk through a ConstantExpr to the GlobalValue it names. This is the forward
// (over operands, not users) analogue of GlobalDCE's dependency computation.
void
visitReferencedOperand(const llvm::Value *operand,
                       llvm::function_ref<void(const llvm::GlobalValue &)> visit) {
    if (auto *gv = llvm::dyn_cast<llvm::GlobalValue>(operand)) {
        visit(*gv);
        return;
    }
    if (llvm::isa<llvm::MetadataAsValue>(operand)) {
        return;
    }
    if (auto *c = llvm::dyn_cast<llvm::Constant>(operand)) {
        for (const auto &sub : c->operands()) {
            visitReferencedOperand(sub.get(), visit);
        }
    }
}

// Every GlobalValue directly referenced by `root`'s definition: a function's
// instruction operands, a global's initializer, an alias's aliasee. Attached
// metadata and debug locations are deliberately never walked.
void
forEachReferencedGlobalValue(const llvm::GlobalValue &root,
                             llvm::function_ref<void(const llvm::GlobalValue &)> visit) {
    if (auto *f = llvm::dyn_cast<llvm::Function>(&root)) {
        for (const auto &bb : *f) {
            for (const auto &inst : bb) {
                for (const auto &operand : inst.operands()) {
                    visitReferencedOperand(operand.get(), visit);
                }
            }
        }
    }
    else if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(&root)) {
        if (gv->hasInitializer()) {
            visitReferencedOperand(gv->getInitializer(), visit);
        }
    }
    else if (auto *ga = llvm::dyn_cast<llvm::GlobalAlias>(&root)) {
        if (const llvm::Constant *aliasee = ga->getAliasee()) {
            visitReferencedOperand(aliasee, visit);
        }
    }
}

// Transitive closure of the GlobalValues reachable from `root` over the same
// forward reference edges the pull follows (operands, initializer, aliasee;
// metadata excluded). The reachability walk the plan shares between the pull and
// the permutation key.
llvm::DenseSet<const llvm::GlobalValue *>
reachableClosure(const llvm::GlobalValue &root) {
    llvm::DenseSet<const llvm::GlobalValue *>        seen;
    llvm::SmallVector<const llvm::GlobalValue *, 32> worklist;

    seen.insert(&root);
    worklist.push_back(&root);

    while (!worklist.empty()) {
        const llvm::GlobalValue *gv = worklist.pop_back_val();
        forEachReferencedGlobalValue(*gv, [&](const llvm::GlobalValue &ref) {
            if (seen.insert(&ref).second) {
                worklist.push_back(&ref);
            }
        });
    }

    return seen;
}

// Content identity of a chosen definition: a stable hash of its printed IR
// (signature, attributes, and body/initializer/aliasee). Two definitions with
// identical IR -- which would compile to identical bitcode -- collide by design.
uint64_t
hashDefinition(const llvm::GlobalValue &gv) {
    std::string           buffer;
    llvm::raw_string_ostream os(buffer);
    gv.print(os);
    return llvm::xxHash64(os.str());
}

} // namespace

void
linkModules(llair::Module *dst, const llair::Module *src, LinkerTypeCache &type_cache) {
    Linker linker(*dst, type_cache);
    linker.linkModule(src);

    dst->syncMetadata();
}

void
linkModules(llair::Module *dst, const llair::Module *src) {
    LinkerTypeCache type_cache;
    linkModules(dst, src, type_cache);
}

void
finalizeInterfaces(Module *module, llvm::ArrayRef<Interface *> interfaces, std::function<uint32_t(const Class*)> getKindForClass) {
    auto dispatcher_module = std::make_unique<Module>("", module->getContext());

    llvm::StringMap<llvm::DenseSet<Interface *>> interface_index;

    std::for_each(
        interfaces.begin(), interfaces.end(),
        [&interface_index](auto interface) -> void {
            std::for_each(
                interface->method_begin(), interface->method_end(),
                [&interface_index, interface](const auto& method) -> void {
                    interface_index[method.getName()].insert(interface);
                });
        });

    llvm::DenseMap<llvm::StructType *, Interface *> interfaces_by_type;

    std::for_each(
        module->class_begin(), module->class_end(),
        [getKindForClass, &dispatcher_module, &interface_index, &interfaces_by_type](const auto& klass) -> void {
            // Find all interfaces that match `klass`:
            llvm::DenseMap<Interface *, std::size_t> implemented_method_count;

            std::for_each(
                klass.method_begin(), klass.method_end(),
                [&interface_index, &implemented_method_count](const auto& method) {
                    auto it = interface_index.find(method.getName());
                    if (it == interface_index.end()) {
                        return;
                    }

                    std::for_each(
                        it->second.begin(), it->second.end(),
                        [&implemented_method_count](auto interface) {
                            implemented_method_count[interface]++;
                        });
                });

            std::for_each(
                implemented_method_count.begin(), implemented_method_count.end(),
                [getKindForClass, &dispatcher_module, &interfaces_by_type, &klass](auto tmp) {
                    auto [ interface, implemented_method_count ] = tmp;
                    if (implemented_method_count != interface->method_size()) {
                        return;
                    }

                    auto r_dispatchers = dispatcher_module->getOrInsertDispatchers(interface);
                    assert(r_dispatchers.first != r_dispatchers.second);

                    auto dispatcher = *r_dispatchers.first;
                    dispatcher->insertImplementation(getKindForClass(&klass), &klass);

                    interfaces_by_type.insert({ interface->getType(), interface });
                });
        });

    linkModules(module, dispatcher_module.get());
}

class Linker::TypeMapper : public llvm::ValueMapTypeRemapper {
public:
    TypeMapper(llvm::LLVMContext &context, LinkerTypeCache &type_cache)
        : d_context(context)
        , d_type_map(type_cache.remapped_types())
        , d_opaque_struct_type_map(type_cache.canonical_structs()) {
    }

    // llvm::ValueMapTypeRemapper overrides:
    llvm::Type *remapType(llvm::Type *SrcTy) override {
        auto it = d_type_map.find(SrcTy);
        if (it != d_type_map.end()) {
            return it->second;
        }

        auto SrcStructTy     = llvm::dyn_cast<llvm::StructType>(SrcTy);
        bool is_named_struct = SrcStructTy && SrcStructTy->hasName();

        // A named opaque struct reports zero contained types but must still be
        // canonicalized, so exclude it from the leaf early-out.
        if (!is_named_struct && SrcTy->getNumContainedTypes() == 0) {
            return d_type_map[SrcTy] = SrcTy;
        }

        // Placeholder breaks cycles in self-referential types: a re-entrant
        // lookup for SrcTy reached while remapping its own fields resolves here
        // instead of recursing forever.
        d_type_map[SrcTy] = SrcTy;

        llvm::Type *RemappedTy = SrcTy;

        if (is_named_struct && SrcStructTy->isOpaque()) {
            auto identifier = canonicalStructIdentifier(SrcStructTy->getName());
            if (identifier) {
                RemappedTy = d_opaque_struct_type_map.try_emplace(*identifier, SrcStructTy).first->second;
            }
        }
        else {
            llvm::SmallVector<llvm::Type *, 8> RemappedContainedTys(SrcTy->getNumContainedTypes(), nullptr);
            std::transform(
                SrcTy->subtype_begin(), SrcTy->subtype_end(),
                RemappedContainedTys.begin(),
                [&](auto ContainedTy) -> llvm::Type * {
                    return remapType(ContainedTy);
                });

            if (is_named_struct) {
                RemappedTy = llvm::StructType::get(d_context, RemappedContainedTys,
                                                    SrcStructTy->isPacked());
            }
            else if (!std::equal(SrcTy->subtype_begin(), SrcTy->subtype_end(),
                                 RemappedContainedTys.begin())) {
                switch (SrcTy->getTypeID()) {
                case Type::FunctionTyID: {
                    RemappedTy = llvm::FunctionType::get(
                        RemappedContainedTys[0],
                        ArrayRef(&RemappedContainedTys[1], SrcTy->getNumContainedTypes() - 1),
                        cast<llvm::FunctionType>(SrcTy)->isVarArg());
                } break;
                case Type::PointerTyID: {
                    RemappedTy = llvm::PointerType::get(
                        RemappedContainedTys[0], cast<llvm::PointerType>(SrcTy)->getAddressSpace());
                } break;
                case Type::StructTyID: {
                    RemappedTy = llvm::StructType::get(d_context, RemappedContainedTys,
                                                    cast<StructType>(SrcTy)->isPacked());
                } break;
                case Type::ArrayTyID: {
                    RemappedTy = llvm::ArrayType::get(RemappedContainedTys[0],
                                                        cast<ArrayType>(SrcTy)->getNumElements());
                } break;
                default:
                    break;
                }
            }
        }

        d_type_map[SrcTy] = RemappedTy;
        return RemappedTy;
    }

private:
    llvm::LLVMContext &                       d_context;
    LinkerTypeCache::RemappedTypeMap &        d_type_map;
    LinkerTypeCache::CanonicalStructMap &     d_opaque_struct_type_map;
};

Linker::Linker(Module &dst, LinkerTypeCache &type_cache)
: TMap(new TypeMapper(dst.getLLContext(), type_cache)), d_dst(dst) {
}

Linker::~Linker() {
}

// Performs a task similar to LLVM's link `linkModules()`, except that it
// modifies neither the source module nor any of the non-literal
// `StructTypes` used by either module (this `linkModules()` is also
// probably naive compared to LLVM's, sufficient to link small Metal
// shaders, but, not, say, Chromium).
void
Linker::linkModule(const Module *src) {
    auto New = d_dst.getLLModule();
    auto M   = src->getLLModule();

    // Map global values declared in 'src' to global values defined in 'dst':
    llvm::DenseMap<const llvm::GlobalValue *, llvm::GlobalValue *> src_to_dst_global_value_map;

    for (const auto& src_global_value : M->global_values()) {
        if (!src_global_value.isDeclarationForLinker()) {
            continue;
        }

        auto dst_global_value = New->getNamedValue(src_global_value.getName());

        if (!dst_global_value ||
            (!dst_global_value->isStrongDefinitionForLinker() && !dst_global_value->isDeclarationForLinker())) {
            continue;
        }

        src_to_dst_global_value_map[&src_global_value] = dst_global_value;
    }

    // Now clone 'src' into 'dst':
    ValueToValueMapTy VMap;

    // Loop over all of the global variables, making corresponding globals in the
    // new module.  Here we add them to the VMap and to the new Module.  We
    // don't worry about attributes or initializers, they will come later.
    //
    SmallVector<std::pair<const GlobalVariable *, GlobalVariable *>, 32> pending_globals;

    for (llvm::Module::const_global_iterator I = M->global_begin(), E = M->global_end(); I != E;
         ++I) {
        if (I->getName() == "llvm.global_ctors") {
            continue;
        }

        if (I->isDeclaration()) {
            GlobalVariable *GV = nullptr;

            auto it = src_to_dst_global_value_map.find(&*I);
            if (it != src_to_dst_global_value_map.end()) {
                GV = llvm::cast<GlobalVariable>(it->second);
            }

            if (!GV) {
                GV = new GlobalVariable(*New, TMap->remapType(I->getValueType()), I->isConstant(),
                                        I->getLinkage(), (Constant *)nullptr, I->getName(),
                                        (GlobalVariable *)nullptr, I->getThreadLocalMode(),
                                        I->getType()->getAddressSpace());
                GV->copyAttributesFrom(&*I);
            }

            VMap[&*I] = GV;
            continue;
        }

        GlobalVariable *GV = New->getGlobalVariable(I->getName());

        if (GV && !GV->isDeclaration()) {
            if (GV->getLinkage() == GlobalValue::AvailableExternallyLinkage) {
                // dst's initializer is a stand-in; src's real definition wins.
                GV->setInitializer(nullptr);
            }
            else {
                // ODR: initializers are equivalent, keep dst's. Anything src's
                // initializer reached is already reachable through dst's.
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

    // Loop over the function declarations:
    for (const Function &I : *M) {
        if (!I.isDeclaration()) {
            continue;
        }

        Function *NF = nullptr;

        // If calling a member of 'src_to_dst_global_value_map', rewrite the declaration:
        if (!NF) {
            auto it = src_to_dst_global_value_map.find(&I);
            if (it != src_to_dst_global_value_map.end()) {
                NF = llvm::cast<Function>(it->second);
            }
        }

        if (!NF) {
            NF = Function::Create(cast<FunctionType>(TMap->remapType(I.getValueType())),
                                  I.getLinkage(), I.getName(), New);
            NF->copyAttributesFrom(&I);
        }

        VMap[&I] = NF;
    }

    // Loop over function definitions:
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

    // Loop over the aliases in the module
    for (llvm::Module::const_alias_iterator I = M->alias_begin(), E = M->alias_end(); I != E; ++I) {
        auto *GA = GlobalAlias::create(TMap->remapType(I->getValueType()), I->getType()->getPointerAddressSpace(),
                                       I->getLinkage(), I->getName(), New);
        GA->copyAttributesFrom(&*I);
        VMap[&*I] = GA;
    }

    // Now that all of the things that global variable initializer can refer to
    // have been created, loop through and copy the global variable referrers
    // over...  We also set the attributes on the global now.
    //
    for (auto &[I, GV] : pending_globals) {
        if (I->hasInitializer()) {
            GV->setInitializer(MapValue(I->getInitializer(), VMap, RF_None, TMap.get()));
        }

        SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
        I->getAllMetadata(MDs);
        for (auto MD : MDs)
#if LLVM_VERSION_MAJOR >= 13
            GV->addMetadata(MD.first, *MapMetadata(MD.second, VMap, RF_None, TMap.get()));
#else
            GV->addMetadata(MD.first, *MapMetadata(MD.second, VMap, RF_None, &TMap));
#endif

        copyComdat(GV, I);
    }

    // Similarly, copy over function bodies now...
    //
    for (auto &[I, F] : pending_bodies) {
        Function::arg_iterator DestI = F->arg_begin();
        for (Function::const_arg_iterator J = I->arg_begin(); J != I->arg_end(); ++J) {
            DestI->setName(J->getName());
            VMap[&*J] = &*DestI++;
        }

        SmallVector<ReturnInst *, 8> Returns; // Ignore returns cloned.
#if LLVM_VERSION_MAJOR >= 13
        CloneFunctionInto(F, I, VMap, CloneFunctionChangeType::DifferentModule, Returns, "", nullptr, TMap.get());
#else
        CloneFunctionInto(F, I, VMap, /*ModuleLevelChanges=*/true, Returns, "", nullptr, &TMap);
#endif

        if (I->hasPersonalityFn())
            F->setPersonalityFn(MapValue(I->getPersonalityFn(), VMap, RF_None, TMap.get()));

        copyComdat(F, I);

        if (F->hasSection() && F->getSection() == "air.static_init") {
            appendToGlobalCtors(*New, F, 65535);
        }
    }

    // And aliases
    for (llvm::Module::const_alias_iterator I = M->alias_begin(), E = M->alias_end(); I != E; ++I) {
        GlobalAlias *GA = cast<GlobalAlias>(VMap[&*I]);
        if (const Constant *C = I->getAliasee())
            GA->setAliasee(MapValue(C, VMap, RF_None, TMap.get()));
    }

    // And named metadata....
    static const std::set<llvm::StringRef> s_once_metadata_names = {
        "air.version", "air.language_version", "air.compile_options", "air.source_file_name", "llvm.ident",
        "llvm.module.flags"};

    for (llvm::Module::const_named_metadata_iterator I = M->named_metadata_begin(),
                                                     E = M->named_metadata_end();
         I != E; ++I) {
        const NamedMDNode &NMD    = *I;
        NamedMDNode *      NewNMD = New->getOrInsertNamedMetadata(NMD.getName());

        if (s_once_metadata_names.count(NMD.getName()) > 0 && NewNMD->getNumOperands() > 0)
            continue;

        for (unsigned i = 0, e = NMD.getNumOperands(); i != e; ++i)
            NewNMD->addOperand(MapMetadata(NMD.getOperand(i), VMap, RF_None, TMap.get()));
    }
}

void
Linker::syncMetadata() {
    d_dst.syncMetadata();
}

void
Linker::addModule(const Module *src) {
    d_modules.push_back(src);
}

void
Linker::require(llvm::StringRef name) {
    if (name.empty()) {
        return;
    }
    if (d_enqueued.insert(name).second) {
        d_worklist.push_back(name.str());
    }
}

const llvm::GlobalValue *
Linker::findDefinition(llvm::StringRef name) const {
    for (auto *m : d_modules) {
        if (auto *def = m->lookupDefinition(name)) {
            return def;
        }
    }
    return nullptr;
}

llvm::GlobalValue *
Linker::declare(const llvm::GlobalValue *src) {
    {
        auto it = d_vmap.find(src);
        if (it != d_vmap.end()) {
            return cast<GlobalValue>(it->second);
        }
    }

    auto New = d_dst.getLLModule();

    // A src symbol already present in dst (a prior link, or a decl this pull
    // created) is the join target; multiple src objects for one name share it.
    if (auto *existing = New->getNamedValue(src->getName())) {
        d_vmap[src] = existing;
        return existing;
    }

    GlobalValue *dst = nullptr;

    if (auto *gv = dyn_cast<GlobalVariable>(src)) {
        auto *NGV = new GlobalVariable(*New, TMap->remapType(gv->getValueType()), gv->isConstant(),
                                       gv->getLinkage(), (Constant *)nullptr, gv->getName(),
                                       (GlobalVariable *)nullptr, gv->getThreadLocalMode(),
                                       gv->getType()->getAddressSpace());
        NGV->copyAttributesFrom(gv);
        dst = NGV;
    }
    else if (auto *f = dyn_cast<Function>(src)) {
        auto *NF = Function::Create(cast<FunctionType>(TMap->remapType(f->getValueType())),
                                    f->getLinkage(), f->getName(), New);
        NF->copyAttributesFrom(f);
        dst = NF;
    }
    else {
        auto *ga  = cast<GlobalAlias>(src);
        auto *NGA = GlobalAlias::create(TMap->remapType(ga->getValueType()),
                                        ga->getType()->getPointerAddressSpace(), ga->getLinkage(),
                                        ga->getName(), New);
        NGA->copyAttributesFrom(ga);
        dst = NGA;
    }

    d_vmap[src] = dst;
    return dst;
}

void
Linker::define(const llvm::GlobalValue *src) {
    auto New  = d_dst.getLLModule();
    auto name = src->getName();

    // A2 dedup, mirroring the eager path's decision for an already-defined name.
    if (auto *existing = New->getNamedValue(name); existing && !existing->isDeclaration()) {
        if (existing->getLinkage() == GlobalValue::AvailableExternallyLinkage) {
            // dst's definition is a stand-in; src's real definition wins.
            if (auto *egv = dyn_cast<GlobalVariable>(existing)) {
                egv->setInitializer(nullptr);
            }
            else if (auto *ef = dyn_cast<Function>(existing)) {
                ef->deleteBody();
            }
        }
        else {
            // ODR: definitions are equivalent, keep dst's. Anything src's reached
            // is already reachable through dst's.
            assert(isODR(src->getLinkage()) && isODR(existing->getLinkage()));
            d_vmap[src] = existing;
            d_resolved.insert(name);
            return;
        }
    }

    auto *dst = declare(src);

    // Comdat is a source-module property, kept out of the reachability walk: pull
    // every sibling in src's group so the comdat stays a unit.
    if (auto *go = dyn_cast<GlobalObject>(src)) {
        if (auto *comdat = go->getComdat()) {
            for (const auto &sibling : go->getParent()->global_values()) {
                if (auto *sgo = dyn_cast<GlobalObject>(&sibling); sgo && sgo->getComdat() == comdat) {
                    require(sibling.getName());
                }
            }
        }
    }

    // Declare every referenced GlobalValue before the clone (so RF_None resolves
    // it in d_vmap rather than leaving the source object), and enqueue its real
    // definition to be pulled later.
    forEachReferencedGlobalValue(*src, [this](const llvm::GlobalValue &ref) {
        declare(&ref);
        require(ref.getName());
    });

    if (auto *gv = dyn_cast<GlobalVariable>(src)) {
        auto *GV = cast<GlobalVariable>(dst);
        GV->copyAttributesFrom(gv);
        if (gv->hasInitializer()) {
            GV->setInitializer(MapValue(gv->getInitializer(), d_vmap, RF_None, TMap.get()));
        }

        SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
        gv->getAllMetadata(MDs);
        for (auto MD : MDs) {
            GV->addMetadata(MD.first, *MapMetadata(MD.second, d_vmap, RF_None, TMap.get()));
        }

        copyComdat(GV, gv);
    }
    else if (auto *f = dyn_cast<Function>(src)) {
        auto *F = cast<Function>(dst);
        F->copyAttributesFrom(f);

        Function::arg_iterator DestI = F->arg_begin();
        for (Function::const_arg_iterator J = f->arg_begin(); J != f->arg_end(); ++J) {
            DestI->setName(J->getName());
            d_vmap[&*J] = &*DestI++;
        }

        SmallVector<ReturnInst *, 8> Returns;
        CloneFunctionInto(F, f, d_vmap, CloneFunctionChangeType::DifferentModule, Returns, "",
                          nullptr, TMap.get());

        if (f->hasPersonalityFn()) {
            F->setPersonalityFn(MapValue(f->getPersonalityFn(), d_vmap, RF_None, TMap.get()));
        }

        copyComdat(F, f);

        // Batched so llvm.global_ctors is built once in resolve(), not per ctor.
        if (F->hasSection() && F->getSection() == "air.static_init") {
            d_pending_ctors.push_back(F);
        }
    }
    else {
        auto *ga = cast<GlobalAlias>(src);
        auto *GA = cast<GlobalAlias>(dst);
        GA->copyAttributesFrom(ga);
        if (const Constant *C = ga->getAliasee()) {
            GA->setAliasee(MapValue(C, d_vmap, RF_None, TMap.get()));
        }
    }

    d_resolved.insert(name);
}

void
Linker::copyNamedMetadata() {
    auto New = d_dst.getLLModule();

    static const std::set<llvm::StringRef> s_once_metadata_names = {
        "air.version", "air.language_version", "air.compile_options", "air.source_file_name",
        "llvm.ident", "llvm.module.flags"};

    // Copied wholesale under RF_None. air.vertex/fragment/kernel is safe because
    // every entry point is a root and so resolves in d_vmap. This would assert if
    // some other named-MD node referenced an un-pulled GlobalValue -- in practice
    // only debug info (llvm.dbg.cu -> DISubprogram -> un-pulled fn), which no
    // Bourbon link input carries. If debug info ever appears at link time, the
    // non-entry/non-once nodes need a missing-tolerant map or a post-
    // StripDebugInfo ordering; the eager path never hits this because it pulls
    // everything.
    for (auto *m : d_modules) {
        auto M = m->getLLModule();
        for (llvm::Module::const_named_metadata_iterator I = M->named_metadata_begin(),
                                                         E = M->named_metadata_end();
             I != E; ++I) {
            const NamedMDNode &NMD    = *I;
            NamedMDNode *      NewNMD = New->getOrInsertNamedMetadata(NMD.getName());

            if (s_once_metadata_names.count(NMD.getName()) > 0 && NewNMD->getNumOperands() > 0) {
                continue;
            }

            for (unsigned i = 0, e = NMD.getNumOperands(); i != e; ++i) {
                NewNMD->addOperand(MapMetadata(NMD.getOperand(i), d_vmap, RF_None, TMap.get()));
            }
        }
    }
}

void
Linker::resolve() {
    auto New = d_dst.getLLModule();

    // Seed roots from every registered module: entry points, llvm.used /
    // llvm.compiler.used, and air.static_init constructors. Explicit require()s
    // are already queued.
    for (auto *m : d_modules) {
        auto M = m->getLLModule();

        for (auto I = m->entry_point_begin(), E = m->entry_point_end(); I != E; ++I) {
            require(I->getFunction()->getName());
        }

        SmallVector<GlobalValue *, 8> used;
        collectUsedGlobalVariables(*M, used, /*CompilerUsed=*/false);
        collectUsedGlobalVariables(*M, used, /*CompilerUsed=*/true);
        for (auto *gv : used) {
            require(gv->getName());
        }

        for (const auto &f : M->functions()) {
            if (f.hasSection() && f.getSection() == "air.static_init") {
                require(f.getName());
            }
        }
    }

    // Drain: an unresolved name with no definition stays an external declaration,
    // exactly as the eager path leaves an unresolved decl. Checking d_resolved at
    // pop keeps resolve() re-enterable across require()+resolve() calls.
    while (!d_worklist.empty()) {
        std::string name = std::string(d_worklist.pop_back_val());
        if (d_resolved.count(name) > 0) {
            continue;
        }
        if (auto *def = findDefinition(name)) {
            define(def);
        }
    }

    for (auto *F : d_pending_ctors) {
        appendToGlobalCtors(*New, F, 65535);
    }
    d_pending_ctors.clear();

    copyNamedMetadata();
}

void
Linker::addVariationPoint(llvm::StringRef name) {
    d_variation_points.insert(name);
}

bool
Linker::variationPointsBound() const {
    auto New = d_dst.getLLModule();

    for (const auto &entry : d_variation_points) {
        auto *gv = New->getNamedValue(entry.getKey());
        if (!gv || gv->isDeclaration()) {
            return false;
        }
    }

    return true;
}

uint64_t
Linker::permutationKey(const llvm::Function *entry_point) const {
    auto reachable = reachableClosure(*entry_point);

    // (name, chosen-definition hash) for every variation point the entry reaches.
    llvm::SmallVector<std::pair<llvm::StringRef, uint64_t>, 8> bindings;
    for (const auto *gv : reachable) {
        if (d_variation_points.count(gv->getName()) == 0) {
            continue;
        }
        bindings.emplace_back(gv->getName(),
                              gv->isDeclaration() ? 0 : hashDefinition(*gv));
    }

    // Sort by name so the key is independent of the walk's traversal order.
    std::sort(bindings.begin(), bindings.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    // Fold the sorted bindings into one stable value. The NUL after each name
    // delimits the fields so distinct (name, definition) sets cannot alias by
    // concatenation.
    std::string              buffer;
    llvm::raw_string_ostream os(buffer);
    for (const auto &[name, hash] : bindings) {
        os << name << '\0';
        os.write(reinterpret_cast<const char *>(&hash), sizeof(hash));
    }

    return llvm::xxHash64(os.str());
}

} // End namespace llair
