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
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <algorithm>
#include <set>

using namespace llvm;

namespace llair {

namespace {

template <typename Input1, typename Input2, typename BinaryFunction, typename Compare>
void
for_each_intersection(Input1 first1, Input1 last1, Input2 first2, Input2 last2, BinaryFunction fn,
                      Compare comp) {
    while (first1 != last1 && first2 != last2) {
        if (comp(*first1, *first2)) {
            ++first1;
        }
        else {
            if (!comp(*first2, *first1)) {
                fn(*first1, *first2);
                ++first1;
            }
            ++first2;
        }
    }
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

// Performs a task similar to LLVM's link `linkModules()`, except that it
// modifies neither the source module nor any of the non-literal
// `StructTypes` used by either module (this `linkModules()` is also
// probably naive compared to LLVM's, sufficient to link small Metal
// shaders, but, not, say, Chromium).
void
linkModules(llair::Module *dst, const llair::Module *src) {
    llvm::DenseMap<llvm::Type *, llvm::Type *> type_map;

    // Collect global values in both 'src' and 'dst':
    std::vector<const llvm::GlobalValue *> src_global_values;

    std::transform(
        src->getLLModule()->global_values().begin(), src->getLLModule()->global_values().end(),
        std::back_inserter(src_global_values), [](const auto &value) -> auto { return &value; });
    std::sort(src_global_values.begin(), src_global_values.end(),
              [](auto lhs, auto rhs) -> bool { return lhs->getName() < rhs->getName(); });

    std::vector<llvm::GlobalValue *> dst_global_values;

    std::transform(
        dst->getLLModule()->global_values().begin(), dst->getLLModule()->global_values().end(),
        std::back_inserter(dst_global_values), [](auto &value) -> auto { return &value; });
    std::sort(dst_global_values.begin(), dst_global_values.end(),
              [](auto lhs, auto rhs) -> bool { return lhs->getName() < rhs->getName(); });

    struct CompareGlobalValues {
        bool operator()(const llvm::GlobalValue *src_value,
                        const llvm::GlobalValue *dst_value) const {
            return src_value->getName() < dst_value->getName();
        }
    };

    // Map global values declared in 'src' to global values defined in 'dst':
    llvm::DenseMap<const llvm::GlobalValue *, llvm::GlobalValue *> src_to_dst_global_value_map;

    for_each_intersection(
        src_global_values.begin(), src_global_values.end(), dst_global_values.begin(),
        dst_global_values.end(),
        [&src_to_dst_global_value_map](auto src_value, auto dst_value) -> void {
            if (src_value->isDeclarationForLinker() &&
                (dst_value->isStrongDefinitionForLinker() || dst_value->isDeclarationForLinker())) {
                src_to_dst_global_value_map[src_value] = dst_value;
            }
        },
        CompareGlobalValues());

    // Now clone 'src' into 'dst':
    class TypeMapper : public llvm::ValueMapTypeRemapper {
    public:
        TypeMapper(llvm::LLVMContext &context, llvm::DenseMap<llvm::Type *, llvm::Type *> &type_map)
            : d_context(context)
            , d_type_map(type_map) {}

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
                RemappedTy = llvm::StructType::get(d_context, RemappedContainedTys,
                                                   SrcStructTy->isPacked());
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
        llvm::DenseMap<llvm::Type *, llvm::Type *> &d_type_map;
    };

    TypeMapper TMap(dst->getLLModule()->getContext(), type_map);

    ValueToValueMapTy VMap;

    auto M   = src->getLLModule();
    auto New = dst->getLLModule();

    // Loop over all of the global variables, making corresponding globals in the
    // new module.  Here we add them to the VMap and to the new Module.  We
    // don't worry about attributes or initializers, they will come later.
    //
    for (llvm::Module::const_global_iterator I = M->global_begin(), E = M->global_end(); I != E;
         ++I) {
        GlobalVariable *GV = nullptr;

        if (I->isDeclaration()) {
            auto it = src_to_dst_global_value_map.find(&*I);
            if (it != src_to_dst_global_value_map.end()) {
                GV = llvm::cast<GlobalVariable>(it->second);
            }
        }

        if (!GV) {
            GV = new GlobalVariable(*New, TMap.remapType(I->getValueType()), I->isConstant(),
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
            NF = Function::Create(cast<FunctionType>(TMap.remapType(I.getValueType())),
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
            NF = Function::Create(cast<FunctionType>(TMap.remapType(I.getValueType())),
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
        if (I->isDeclaration())
            continue;

        GlobalVariable *GV = cast<GlobalVariable>(VMap[&*I]);
        if (I->hasInitializer()) {
            GV->setInitializer(MapValue(I->getInitializer(), VMap, RF_None, &TMap));
        }

        SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
        I->getAllMetadata(MDs);
        for (auto MD : MDs)
#if LLVM_VERSION_MAJOR >= 13
            GV->addMetadata(MD.first, *MapMetadata(MD.second, VMap, RF_ReuseAndMutateDistinctMDs, &TMap));
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
        CloneFunctionInto(F, &I, VMap, CloneFunctionChangeType::DifferentModule, Returns, "", nullptr, &TMap);
#else
        CloneFunctionInto(F, &I, VMap, /*ModuleLevelChanges=*/true, Returns, "", nullptr, &TMap);
#endif

        if (I.hasPersonalityFn())
            F->setPersonalityFn(MapValue(I.getPersonalityFn(), VMap));

        copyComdat(F, &I);
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
            NewNMD->addOperand(MapMetadata(NMD.getOperand(i), VMap, RF_None, &TMap));
    }

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

Linker::Linker(Delegate& delegate, llvm::StringRef name, LLAIRContext& context)
: d_delegate(delegate)
, d_module(std::make_unique<Module>(name, context)) {
}

void
Linker::addInterface(Interface *interface) {
    std::for_each(
        interface->method_begin(), interface->method_end(),
        [this, interface](const auto& method) -> void {
            d_interface_index[method.getName()].insert(interface);
        });
}

void
Linker::linkModule(const Module *module) {

}

std::unique_ptr<Module>
Linker::releaseModule() {
    return std::move(d_module);
}

Linker::Delegate::~Delegate() {
}

} // End namespace llair
