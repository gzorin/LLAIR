#include <llair/Linker/Linker.h>
#include <llair/IR/Module.h>

#include <llvm/IR/Constant.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Regex.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <set>

using namespace llvm;

namespace llair {

namespace {

using NamedTypes = std::multimap<std::string, llvm::StructType *>;

NamedTypes
GetNamedTypes(const llvm::Module *module) {
    static llvm::Regex named_type_regex(
	"([a-zA-Z_][a-zA-Z0-9_:]*(\\.[a-zA-Z_:][a-zA-Z0-9_:]*)*)(\\.[0-9]+)*");

    NamedTypes result;

    auto types = module->getIdentifiedStructTypes();

    std::for_each(
	types.begin(), types.end(),
	[&result](auto struct_type) -> void {
	    llvm::SmallVector<llvm::StringRef, 2> matches;
	    if (!named_type_regex.match(struct_type->getName(), &matches)) {
		return;
	    }

	    result.insert(std::make_pair(matches[1].str(), struct_type));
	});

    return result;
}

template<typename Input1, typename Input2, typename BinaryFunction, typename Compare>
void
for_each_intersection(Input1 first1, Input1 last1,
		      Input2 first2, Input2 last2,
		      BinaryFunction fn,
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

}

// Performs a task similar to LLVM's link `linkModules()`, except that it
// modifies neither the source module nor any of the non-literal
// `StructTypes` used by either module (this `linkModules()` is also
// probably naive compared to LLVM's, sufficient to link small Metal
// shaders, but, not, say, Chromium).
void
linkModules(llair::Module *dst, const llair::Module *src) {
    // Map types in 'src' to equivalent types in 'dst':
    auto dst_types = GetNamedTypes(dst->getLLModule());
    auto src_types = GetNamedTypes(src->getLLModule());

    llvm::DenseMap<llvm::Type *, llvm::Type *> type_map;

    struct CompareTypes {
	bool operator()(const NamedTypes::value_type& src_type, const NamedTypes::value_type& dst_type) const {
	    return src_type.first < dst_type.first;
	}
    };

    std::for_each(
	src_types.begin(), src_types.end(),
	[&dst_types, &type_map](const auto& src) -> void {
	    auto it = dst_types.find(src.first);
	    if (it != dst_types.end()) {
		type_map[src.second] = it->second;
		return;
	    }

	    dst_types.insert(src);
	});

    // Map global values declared in 'src' to global values defined in 'dst':
    std::vector<const llvm::GlobalValue *> src_global_values;

    std::transform(
	src->getLLModule()->global_value_begin(), src->getLLModule()->global_value_end(),
	std::back_inserter(src_global_values),
	[](const auto& value) -> auto {
	    return &value;
	});
    std::sort(
	src_global_values.begin(), src_global_values.end(),
	[](auto lhs, auto rhs) -> bool {
	    return lhs->getName() < rhs->getName();
	});

    std::vector<llvm::GlobalValue *> dst_global_values;

    std::transform(
	dst->getLLModule()->global_value_begin(), dst->getLLModule()->global_value_end(),
	std::back_inserter(dst_global_values),
	[](auto& value) -> auto {
	    return &value;
	});
    std::sort(
	dst_global_values.begin(), dst_global_values.end(),
	[](auto lhs, auto rhs) -> bool {
	    return lhs->getName() < rhs->getName();
	});

    llvm::DenseMap<const llvm::GlobalValue *, llvm::GlobalValue *> global_value_map;

    struct CompareGlobalValues {
	bool operator()(const llvm::GlobalValue *src_value, const llvm::GlobalValue *dst_value) const {
	    return src_value->getName() < dst_value->getName();
	}
    };

    for_each_intersection(
	src_global_values.begin(), src_global_values.end(),
	dst_global_values.begin(), dst_global_values.end(),
	[&global_value_map](auto src_value, auto dst_value) -> void {
	    if (src_value->isDeclarationForLinker() && dst_value->isStrongDefinitionForLinker()) {
		global_value_map[src_value] = dst_value;
	    }
	},
	CompareGlobalValues());

    // Now clone 'src' into 'dst':
    class TypeMapper : public llvm::ValueMapTypeRemapper {
    public:
	
	TypeMapper(llvm::LLVMContext& context, llvm::DenseMap<llvm::Type *, llvm::Type *>& type_map)
	    : d_context(context)
	    , d_type_map(type_map) {
	}
	
	// llvm::ValueMapTypeRemapper overrides:
	llvm::Type *remapType(llvm::Type *SrcTy) override {
	    // Search the type map:
	    {
		auto it = d_type_map.find(SrcTy);
		if (it != d_type_map.end()) {
		    return it->second;
		}
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
	    
	    // Are any contained types remapped?
	    if (!std::equal(
		    SrcTy->subtype_begin(), SrcTy->subtype_end(),
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
			RemappedContainedTys[0],
			cast<llvm::PointerType>(SrcTy)->getAddressSpace());
		} break;
		case Type::StructTyID: {
		    auto SrcStructTy = llvm::cast<StructType>(SrcTy);

		    if (SrcStructTy->isLiteral()) {
			RemappedTy = llvm::StructType::get(
			    d_context, RemappedContainedTys, SrcStructTy->isPacked());
		    }
		    else {
			RemappedTy = llvm::StructType::create(
			    d_context, RemappedContainedTys, SrcStructTy->getName(), SrcStructTy->isPacked());
		    }
		} break;
		default:
		    break;
		}
	    }
	    
	    // Cache it no matter what:
	    d_type_map[SrcTy] = RemappedTy;
	    
	    return RemappedTy;
	}
	
    private:

	llvm::LLVMContext& d_context;
	llvm::DenseMap<llvm::Type *, llvm::Type *>& d_type_map;
    };

    TypeMapper TMap(dst->getLLModule()->getContext(), type_map);

    ValueToValueMapTy VMap;

    auto M = src->getLLModule();
    auto New = dst->getLLModule();
   
    // Loop over all of the global variables, making corresponding globals in the
    // new module.  Here we add them to the VMap and to the new Module.  We
    // don't worry about attributes or initializers, they will come later.
    //
    for (llvm::Module::const_global_iterator I = M->global_begin(), E = M->global_end();
	 I != E; ++I) {
	GlobalVariable *GV = nullptr;

	if (I->isDeclaration()) {
	    auto it = global_value_map.find(&*I);
	    if (it != global_value_map.end()) {
		GV = llvm::cast<GlobalVariable>(it->second);
	    }
	}

	if (!GV) {
	    GV = new GlobalVariable(*New, 
				    TMap.remapType(I->getValueType()),
				    I->isConstant(), I->getLinkage(),
				    (Constant*) nullptr, I->getName(),
				    (GlobalVariable*) nullptr,
				    I->getThreadLocalMode(),
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
	
	// If calling a member of 'function_map', rewrite the declaration:
	if (!NF) {
	    auto it = global_value_map.find(&I);
	    if (it != global_value_map.end()) {
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
	
	Function *NF = nullptr;
	
	if (!NF) {
	    NF = Function::Create(cast<FunctionType>(TMap.remapType(I.getValueType())),
				  I.getLinkage(), I.getName(), New);
	}
	
	NF->copyAttributesFrom(&I);
	VMap[&I] = NF;
    }

    // Loop over the aliases in the module
    for (llvm::Module::const_alias_iterator I = M->alias_begin(), E = M->alias_end();
	 I != E; ++I) {
	auto *GA = GlobalAlias::create(I->getValueType(),
				       I->getType()->getPointerAddressSpace(),
				       I->getLinkage(), I->getName(), New);
	GA->copyAttributesFrom(&*I);
	VMap[&*I] = GA;
    }
    
    // Now that all of the things that global variable initializer can refer to
    // have been created, loop through and copy the global variable referrers
    // over...  We also set the attributes on the global now.
    //
    for (llvm::Module::const_global_iterator I = M->global_begin(), E = M->global_end();
	 I != E; ++I) {
	if (I->isDeclaration())
	    continue;
	
	GlobalVariable *GV = cast<GlobalVariable>(VMap[&*I]);
	if (I->hasInitializer())
	    GV->setInitializer(MapValue(I->getInitializer(), VMap));
	
	SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
	I->getAllMetadata(MDs);
	for (auto MD : MDs)
	    GV->addMetadata(MD.first,
			    *MapMetadata(MD.second, VMap, RF_MoveDistinctMDs));
	
	copyComdat(GV, &*I);
    }
    
    // Similarly, copy over function bodies now...
    //
    for (const Function &I : *M) {
	if (I.isDeclaration())
	    continue;
	
	Function *F = cast<Function>(VMap[&I]);
	
	Function::arg_iterator DestI = F->arg_begin();
	for (Function::const_arg_iterator J = I.arg_begin(); J != I.arg_end();
	     ++J) {
	    DestI->setName(J->getName());
	    VMap[&*J] = &*DestI++;
	}
	
	SmallVector<ReturnInst *, 8> Returns; // Ignore returns cloned.
	CloneFunctionInto(F, &I, VMap, /*ModuleLevelChanges=*/true, Returns,
			  "", nullptr, &TMap);
	
	if (I.hasPersonalityFn())
	    F->setPersonalityFn(MapValue(I.getPersonalityFn(), VMap));
	
	copyComdat(F, &I);
    }
    
    // And aliases
    for (llvm::Module::const_alias_iterator I = M->alias_begin(), E = M->alias_end();
	 I != E; ++I) {
	GlobalAlias *GA = cast<GlobalAlias>(VMap[&*I]);
	if (const Constant *C = I->getAliasee())
	    GA->setAliasee(MapValue(C, VMap));
    }
    
    // And named metadata....
    static const std::set<llvm::StringRef> s_once_metadata_names = {
	"air.version",
	"air.language_version",
	"air.compile_options",
	"llvm.ident",
	"llvm.module.flags"
    };

    for (llvm::Module::const_named_metadata_iterator I = M->named_metadata_begin(),
	     E = M->named_metadata_end(); I != E; ++I) {
	const NamedMDNode &NMD = *I;
	NamedMDNode *NewNMD = New->getOrInsertNamedMetadata(NMD.getName());

	if (s_once_metadata_names.count(NMD.getName()) > 0 && NewNMD->getNumOperands() > 0)
	    continue;

	for (unsigned i = 0, e = NMD.getNumOperands(); i != e; ++i)
	    NewNMD->addOperand(MapMetadata(NMD.getOperand(i), VMap));
    }

    dst->syncMetadata();
}

} // End namespace llair
