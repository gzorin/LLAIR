#include <llair/IR/Interface.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#include <numeric>

#include "LLAIRContextImpl.h"

namespace llair {

Interface::Interface(LLAIRContext& context, llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::StringRef> qualifiedNames, llvm::ArrayRef<llvm::FunctionType *> types)
    : d_context(context)
    , d_type(type)
    , d_method_count(std::min(std::min(names.size(), qualifiedNames.size()), types.size())) {
    d_methods = std::allocator<Method>().allocate(d_method_count);

    auto p_method = d_methods;
    auto it_name = names.begin();
    auto it_qualifiedName = qualifiedNames.begin();
    auto it_type = types.begin();

    for (auto n = d_method_count; n > 0; --n, ++p_method, ++it_name, ++it_qualifiedName, ++it_type) {
        new (p_method) Method(*it_name, *it_qualifiedName, *it_type);
    }

    std::sort(
        d_methods, d_methods + d_method_count,
        [](const auto &lhs, const auto &rhs) -> auto {
            return lhs.getName() < rhs.getName();
        });

    std::vector<llvm::Metadata *> method_mds;
    method_mds.reserve(d_method_count);

    std::transform(
        d_methods, d_methods + d_method_count,
        std::back_inserter(method_mds),
        [](const auto& method) -> auto {
            return method.d_md;
        });

    auto& ll_context = context.getLLContext();

    d_md = llvm::MDTuple::get(
        ll_context,
        { llvm::ConstantAsMetadata::get(
                llvm::ConstantPointerNull::get(llvm::PointerType::get(d_type, 0))),
          llvm::MDTuple::get(ll_context, method_mds) } );
}

Interface::~Interface() {
    std::for_each(
        d_methods, d_methods + d_method_count,
        [](auto &method) -> void { method.~Method(); });

    std::allocator<Method>().deallocate(d_methods, d_method_count);
}

Interface *
Interface::get(LLAIRContext& context, llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::StringRef> qualifiedNames, llvm::ArrayRef<llvm::FunctionType *> types) {
    auto& context_impl = LLAIRContextImpl::Get(context);

    auto it = context_impl.interfaces().find_as(
        InterfaceKeyInfo::KeyTy(type, names, qualifiedNames, types));

    if (it != context_impl.interfaces().end()) {
        return *it;
    }

    auto interface = new Interface(context, type, names, qualifiedNames, types);
    context_impl.interfaces().insert(interface);
    return interface;
}

Interface *
Interface::get(llvm::Metadata *md) {
    auto md_tuple = llvm::cast<llvm::MDTuple>(md);

    auto context = LLAIRContext::Get(&md_tuple->getContext());

    auto type = llvm::cast<llvm::StructType>(
        llvm::mdconst::extract<llvm::ConstantPointerNull>(md_tuple->getOperand(0).get())->
            getType()->
                getElementType());

    auto methods_md = llvm::cast<llvm::MDTuple>(md_tuple->getOperand(1).get());

    std::vector<llvm::StringRef> names;
    names.reserve(methods_md->getNumOperands());

    std::vector<llvm::StringRef> qualifiedNames;
    qualifiedNames.reserve(methods_md->getNumOperands());

    std::vector<llvm::FunctionType *> types;
    types.reserve(methods_md->getNumOperands());

    std::for_each(
        methods_md->op_begin(), methods_md->op_end(),
        [&names, &qualifiedNames, &types](const auto& tmp) -> void {
            auto method_md = llvm::cast<llvm::MDTuple>(tmp.get());

            names.push_back(llvm::cast<llvm::MDString>(method_md->getOperand(0).get())->getString());
            qualifiedNames.push_back(llvm::cast<llvm::MDString>(method_md->getOperand(1).get())->getString());
            types.push_back(
                llvm::cast<llvm::FunctionType>(
                    llvm::mdconst::extract<llvm::ConstantPointerNull>(method_md->getOperand(2).get())->
                        getType()->
                            getElementType()));
        });

    return Interface::get(*context, type, names, qualifiedNames, types);
}

const Interface::Method *
Interface::findMethod(llvm::StringRef name) const {
    struct Compare {
        bool operator()(llvm::StringRef lhs, const Method &rhs) const {
            return lhs.compare(rhs.getName()) < 0;
        }

        bool operator()(const Method &lhs, llvm::StringRef rhs) const {
            return rhs.compare(lhs.getName()) >= 0;
        }
    };

    auto tmp = std::equal_range(d_methods, d_methods + d_method_count, name, Compare());

    if (tmp.first == tmp.second) {
        return nullptr;
    }

    return tmp.first;
}

void
Interface::print(llvm::raw_ostream& os) const {
    os << "interface (";
    d_type->print(os, false, true);
    os << ") {" << "\n";

    std::for_each(
        d_methods, d_methods + d_method_count,
        [&os](const auto& method) -> void {
            os.indent(4) << method.getName() << ": ";
            method.getType()->print(os);
            os << "\n";
        });
    os << "}" << "\n";
}

void
Interface::dump() const {
    print(llvm::dbgs());
}

Interface::Method::Method(llvm::StringRef name, llvm::StringRef qualifiedName, llvm::FunctionType *type)
: d_name(name)
, d_qualifiedName(qualifiedName)
, d_type(type) {
    auto& ll_context = d_type->getContext();

    d_md = llvm::MDTuple::get(ll_context, {
        llvm::MDString::get(ll_context, d_name),
        llvm::MDString::get(ll_context, d_qualifiedName),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantPointerNull::get(llvm::PointerType::get(d_type, 0)))
    });
}

} // End namespace llair