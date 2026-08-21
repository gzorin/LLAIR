#include <llair/IR/Module.h>
#include <llair/IR/Class.h>
#include <llair/IR/Dispatcher.h>
#include <llair/IR/EntryPoint.h>
#include <llair/IR/Interface.h>
#include <llair/Linker/Linker.h>
#include <llair/IR/Module.h>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
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

} // End namespace llair
