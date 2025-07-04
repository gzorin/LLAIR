#include <llair/IR/Module.h>
#include <llair/IR/Class.h>
#include <llair/IR/Dispatcher.h>
#include <llair/IR/EntryPoint.h>
#include <llair/IR/Interface.h>
#include <llair/Linker/Linker.h>
#include <llair/IR/Module.h>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Regex.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <algorithm>

using namespace llvm;

namespace llair {

namespace {

llvm::Regex&
cxx_identifier_regex() {
    static llvm::Regex s_cxx_identifier_regex("(struct|class)\\.([a-zA-Z_][a-zA-Z0-9_:]*(\\.[a-zA-Z_:]+)*)(\\.[0-9]+)*");
    return s_cxx_identifier_regex;
}

bool
test_cxx_identifier_regex(llvm::StringRef identifier) {
    return cxx_identifier_regex().match(identifier);
}

llvm::Optional<llvm::StringRef>
match_cxx_identifier_regex(llvm::StringRef identifier) {
    llvm::SmallVector<llvm::StringRef, 3> matches;
    cxx_identifier_regex().match(identifier, &matches);

    if (matches.empty()) {
        return {};
    }

    return { matches[2] };
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

} // namespace

void
linkModules(llair::Module *dst, const llair::Module *src) {
    Linker linker(*dst);
    linker.linkModule(src);

    dst->syncMetadata();
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
    TypeMapper(llvm::LLVMContext &context)
        : d_context(context) {
    }

    void updateIdentifiedOpaqueStructTypes(const llvm::Module *M) {
        auto identified_struct_types = M->getIdentifiedStructTypes();

        std::for_each(
            identified_struct_types.begin(), identified_struct_types.end(),
            [this](auto *identified_struct_type) -> void {
                if (!identified_struct_type->isOpaque()) {
                    return;
                }

                auto identifier = match_cxx_identifier_regex(identified_struct_type->getName());

                if (!identifier) {
                    return;
                }

                if (d_opaque_struct_type_map.count(*identifier) != 0) {
                    return;
                }

                d_opaque_struct_type_map.insert({ *identifier, identified_struct_type });
            });
    }

    // llvm::ValueMapTypeRemapper overrides:
    llvm::Type *remapType(llvm::Type *SrcTy) override {
        // Search the type map:
        auto it = d_type_map.find(SrcTy);
        if (it != d_type_map.end()) {
            return it->second;
        }

        llvm::Type *RemappedTy = SrcTy;

        // Remap contained types:
        std::vector<llvm::Type *> RemappedContainedTys(SrcTy->getNumContainedTypes(), nullptr);

        std::transform(
            SrcTy->subtype_begin(), SrcTy->subtype_end(),
            RemappedContainedTys.begin(),
            [&](auto ContainedTy) -> llvm::Type * {
                return remapType(ContainedTy);
            });

        if (auto SrcStructTy = llvm::dyn_cast<llvm::StructType>(SrcTy); SrcStructTy && SrcStructTy->hasName()) {
            if (SrcStructTy->isOpaque()) {
                auto identifier = match_cxx_identifier_regex(SrcStructTy->getName());

                if (identifier) {
                    auto it = d_opaque_struct_type_map.find(*identifier);
                    if (it == d_opaque_struct_type_map.end()) {
                        it = d_opaque_struct_type_map.insert({ *identifier, SrcStructTy }).first;
                    }

                    RemappedTy = it->second;
                }
            }
            else {
                RemappedTy = llvm::StructType::get(d_context, RemappedContainedTys,
                                                    SrcStructTy->isPacked());
            }
        }
        else {
            // Are any contained types remapped?
            if (!std::equal(SrcTy->subtype_begin(), SrcTy->subtype_end(),
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
                    auto SrcStructTy = llvm::cast<StructType>(SrcTy);

                    RemappedTy = llvm::StructType::get(d_context, RemappedContainedTys,
                                                    SrcStructTy->isPacked());
                } break;
                case Type::ArrayTyID: {
                    auto SrcArrayTy = llvm::cast<ArrayType>(SrcTy);

                    RemappedTy = llvm::ArrayType::get(RemappedContainedTys[0],
                                                        SrcArrayTy->getNumElements());
                } break;
                default:
                    break;
                }
            }
        }

        // Cache it no matter what:
        d_type_map[SrcTy] = RemappedTy;

        return RemappedTy;
    }

private:
    llvm::LLVMContext &                         d_context;
    llvm::DenseMap<llvm::Type *, llvm::Type *>  d_type_map;
    llvm::StringMap<llvm::StructType *>         d_opaque_struct_type_map;
};

Linker::Linker(Module &dst)
: TMap(new TypeMapper(dst.getLLContext())), d_dst(dst) {
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
    TMap->updateIdentifiedOpaqueStructTypes(New);

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
    for (llvm::Module::const_global_iterator I = M->global_begin(), E = M->global_end(); I != E;
         ++I) {
        if (I->getName() == "llvm.global_ctors") {
            continue;
        }

        GlobalVariable *GV = nullptr;

        if (I->isDeclaration()) {
            auto it = src_to_dst_global_value_map.find(&*I);
            if (it != src_to_dst_global_value_map.end()) {
                GV = llvm::cast<GlobalVariable>(it->second);
            }
        }

        if (!GV) {
            GV = new GlobalVariable(*New, TMap->remapType(I->getValueType()), I->isConstant(),
                                    I->getLinkage(), (Constant *)nullptr, I->getName(),
                                    (GlobalVariable *)nullptr, I->getThreadLocalMode(),
                                    I->getType()->getAddressSpace());
            GV->copyAttributesFrom(&*I);
        }

        VMap[&*I] = GV;
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
    for (const Function &I : *M) {
        if (I.isDeclaration()) {
            continue;
        }

        Function *NF = New->getFunction(I.getName());

        if (!NF || !NF->isDeclaration()) {
            NF = Function::Create(cast<FunctionType>(TMap->remapType(I.getValueType())),
                                  I.getLinkage(), I.getName(), New);
        }

        NF->copyAttributesFrom(&I);
        VMap[&I] = NF;
    }

    // Loop over the aliases in the module
    for (llvm::Module::const_alias_iterator I = M->alias_begin(), E = M->alias_end(); I != E; ++I) {
        auto *GA = GlobalAlias::create(I->getValueType(), I->getType()->getPointerAddressSpace(),
                                       I->getLinkage(), I->getName(), New);
        GA->copyAttributesFrom(&*I);
        VMap[&*I] = GA;
    }

    // Now that all of the things that global variable initializer can refer to
    // have been created, loop through and copy the global variable referrers
    // over...  We also set the attributes on the global now.
    //
    for (llvm::Module::const_global_iterator I = M->global_begin(), E = M->global_end(); I != E;
         ++I) {
        if (I->getName() == "llvm.global_ctors") {
            continue;
        }

        if (I->isDeclaration())
            continue;

        GlobalVariable *GV = cast<GlobalVariable>(VMap[&*I]);
        if (I->hasInitializer()) {
            GV->setInitializer(MapValue(I->getInitializer(), VMap, RF_None, TMap.get()));
        }

        SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
        I->getAllMetadata(MDs);
        for (auto MD : MDs)
#if LLVM_VERSION_MAJOR >= 13
            GV->addMetadata(MD.first, *MapMetadata(MD.second, VMap, RF_ReuseAndMutateDistinctMDs, TMap.get()));
#else
            GV->addMetadata(MD.first, *MapMetadata(MD.second, VMap, RF_MoveDistinctMDs, &TMap));
#endif

        copyComdat(GV, &*I);
    }

    // Similarly, copy over function bodies now...
    //
    for (const Function &I : *M) {
        if (I.isDeclaration())
            continue;

        Function *F = cast<Function>(VMap[&I]);

        Function::arg_iterator DestI = F->arg_begin();
        for (Function::const_arg_iterator J = I.arg_begin(); J != I.arg_end(); ++J) {
            DestI->setName(J->getName());
            VMap[&*J] = &*DestI++;
        }

        SmallVector<ReturnInst *, 8> Returns; // Ignore returns cloned.
#if LLVM_VERSION_MAJOR >= 13
        CloneFunctionInto(F, &I, VMap, CloneFunctionChangeType::DifferentModule, Returns, "", nullptr, TMap.get());
#else
        CloneFunctionInto(F, &I, VMap, /*ModuleLevelChanges=*/true, Returns, "", nullptr, &TMap);
#endif

        if (I.hasPersonalityFn())
            F->setPersonalityFn(MapValue(I.getPersonalityFn(), VMap));

        copyComdat(F, &I);

        if (F->hasSection() && F->getSection() == "air.static_init") {
            appendToGlobalCtors(*New, F, 65535);
        }
    }

    // And aliases
    for (llvm::Module::const_alias_iterator I = M->alias_begin(), E = M->alias_end(); I != E; ++I) {
        GlobalAlias *GA = cast<GlobalAlias>(VMap[&*I]);
        if (const Constant *C = I->getAliasee())
            GA->setAliasee(MapValue(C, VMap));
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
