#include <llair/IR/EntryPoint.h>
#include <llair/IR/Module.h>

#include "LLAIRContextImpl.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#include <numeric>
#include <optional>

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

} // namespace

namespace llair {

template<>
void
module_ilist_traits<llair::EntryPoint>::addNodeToList(llair::EntryPoint *entry_point) {
    auto module = getModule();
    entry_point->setModule(module);
}

template<>
void
module_ilist_traits<llair::EntryPoint>::removeNodeFromList(llair::EntryPoint *entry_point) {
    entry_point->setModule(nullptr);
}

const EntryPoint *
EntryPoint::Get(const llvm::Function *llfunction) {
    auto &llcontext = llfunction->getContext();
    auto  context   = LLAIRContext::Get(&llcontext);
    if (context) {
        auto it = LLAIRContextImpl::Get(*context).entry_points().find(
            const_cast<llvm::Function *>(llfunction));
        if (it != LLAIRContextImpl::Get(*context).entry_points().end()) {
            return it->second;
        }
        else {
            return nullptr;
        }
    }
    else {
        return nullptr;
    }
}

EntryPoint *
EntryPoint::Get(llvm::Function *llfunction) {
    auto &llcontext = llfunction->getContext();
    auto  context   = LLAIRContext::Get(&llcontext);
    if (context) {
        auto it = LLAIRContextImpl::Get(*context).entry_points().find(llfunction);
        if (it != LLAIRContextImpl::Get(*context).entry_points().end()) {
            return it->second;
        }
        else {
            return nullptr;
        }
    }
    else {
        return nullptr;
    }
}

EntryPoint::EntryPoint(EntryPoint::EntryPointKind kind, llvm::Function *function, Module *module)
    : d_kind(kind)
    , d_arguments(nullptr) {
    d_function_md.reset(llvm::ValueAsMetadata::get(function));

    if (module) {
        module->getEntryPointList().push_back(this);
    }

    d_argument_count = function->arg_size();

    d_arguments = std::allocator<Argument>().allocate(d_argument_count);

    std::accumulate(function->arg_begin(), function->arg_end(),
                    d_arguments, [=](auto argument, auto &function_argument) -> auto {
                        new (argument) Argument(&function_argument, this);
                        return ++argument;
                    });

    std::vector<llvm::Metadata *> mds;
    unsigned                      index = 0;
    std::transform(d_arguments, d_arguments + d_argument_count, std::back_inserter(mds),
                   [&index](auto &argument) -> llvm::Metadata * {
                       argument.d_md_index = index++;
                       return argument.d_md.get();
                   });

    auto &ll_context = d_module->getLLContext();

    d_arguments_md.reset(llvm::MDTuple::get(ll_context, mds));
}

EntryPoint::EntryPoint(EntryPoint::EntryPointKind kind, llvm::MDNode *md, Module *module)
    : d_kind(kind)
    , d_arguments(nullptr) {
    d_function_md.reset(llvm::cast<llvm::ValueAsMetadata>(md->getOperand(0).get()));

    if (module) {
        module->getEntryPointList().push_back(this);
    }

    auto function = getFunction();

    d_argument_count = function->arg_size();

    d_arguments = std::allocator<Argument>().allocate(d_argument_count);

    d_md.reset(md);

    // Arguments:
    d_arguments_md.reset(llvm::cast<llvm::MDTuple>(d_md->getOperand(2).get()));

    // Sort llvm::Arguments:
    std::vector<llvm::Argument *> arguments;
    arguments.reserve(d_argument_count);
    std::transform(function->arg_begin(), function->arg_end(), std::back_inserter(arguments),
                   [](auto &argument) -> llvm::Argument * { return &argument; });
    std::sort(arguments.begin(), arguments.end(),
              [](auto lhs, auto rhs) -> bool { return lhs->getArgNo() < rhs->getArgNo(); });

    // Sort argument metadata:
    std::vector<std::pair<llvm::MDTuple *, unsigned>> arguments_md;
    arguments_md.reserve(d_argument_count);

    unsigned index = 0;
    std::transform(d_arguments_md->op_begin(), d_arguments_md->op_end(),
                   std::back_inserter(arguments_md),
                   [&index](auto &operand) -> std::pair<llvm::MDTuple *, unsigned> {
                       return {llvm::cast<llvm::MDTuple>(operand.get()), index++};
                   });
    std::sort(arguments_md.begin(), arguments_md.end(), [](auto lhs, auto rhs) -> bool {
        return llvm::mdconst::extract<llvm::ConstantInt>(lhs.first->getOperand(0).get())
                   ->getZExtValue() <
               llvm::mdconst::extract<llvm::ConstantInt>(rhs.first->getOperand(0).get())
                   ->getZExtValue();
    });

    // Construct d_arguments:
    struct Compare {
        bool operator()(llvm::Argument *lhs, std::pair<llvm::MDTuple *, unsigned> rhs) const {
            return lhs->getArgNo() <
                   llvm::mdconst::extract<llvm::ConstantInt>(rhs.first->getOperand(0).get())
                       ->getZExtValue();
        }

        bool operator()(std::pair<llvm::MDTuple *, unsigned> lhs, llvm::Argument *rhs) const {
            return llvm::mdconst::extract<llvm::ConstantInt>(lhs.first->getOperand(0).get())
                       ->getZExtValue() < rhs->getArgNo();
        }
    };

    auto argument = d_arguments;

    for_each_intersection(
        arguments.begin(), arguments.end(), arguments_md.begin(), arguments_md.end(),
        [this, &argument](llvm::Argument *lhs, std::pair<llvm::MDTuple *, unsigned> rhs) -> void {
            new (argument) Argument(lhs, rhs.first, this);
            argument->d_md_index = rhs.second;
            ++argument;
        },
        Compare());
}

EntryPoint::~EntryPoint() {
    std::for_each(d_arguments, d_arguments + d_argument_count,
                  [](auto &argument) -> void { argument.~Argument(); });

    std::allocator<Argument>().deallocate(d_arguments, d_argument_count);
}

void
EntryPoint::setModule(Module *module) {
    auto function = getFunction();

    if (d_module) {
        if (function) {
            LLAIRContextImpl::Get(d_module->getContext()).entry_points().erase(function);
        }

        if (d_module->getLLModule() && d_md) {
            llvm::NamedMDNode *entry_points_md = nullptr;

            switch (d_kind) {
            case Vertex: {
                entry_points_md = d_module->getLLModule()->getNamedMetadata("air.vertex");
            } break;
            case Fragment: {
                entry_points_md = d_module->getLLModule()->getNamedMetadata("air.fragment");
            } break;
            case Compute:
                break;
            }

            if (entry_points_md) {
                auto it =
                    std::find(entry_points_md->op_begin(), entry_points_md->op_end(), d_md.get());
                if (it != entry_points_md->op_end()) {
                    entry_points_md->setOperand(std::distance(entry_points_md->op_begin(), it),
                                                nullptr);
                }
            }
        }
    }

    d_module = module;

    if (d_module) {
        if (function) {
            LLAIRContextImpl::Get(d_module->getContext())
                .entry_points()
                .insert(std::make_pair(function, this));
        }

        if (d_module->getLLModule() && d_md) {
            llvm::NamedMDNode *entry_points_md = nullptr;

            switch (d_kind) {
            case Vertex: {
                entry_points_md = d_module->getLLModule()->getNamedMetadata("air.vertex");
            } break;
            case Fragment: {
                entry_points_md = d_module->getLLModule()->getNamedMetadata("air.fragment");
            } break;
            case Compute:
                break;
            }

            if (entry_points_md) {
                entry_points_md->addOperand(d_md.get());
            }
        }
    }
}

const llvm::Function *
EntryPoint::getFunction() const {
    return d_function_md ? llvm::cast<llvm::Function>(d_function_md->getValue()) : nullptr;
}

llvm::Function *
EntryPoint::getFunction() {
    return d_function_md ? llvm::cast<llvm::Function>(d_function_md->getValue()) : nullptr;
}

void
EntryPoint::setFunction(llvm::Function *function) {}

llvm::StringRef
EntryPoint::getName() const {
    auto function = getFunction();
    return function ? function->getName() : llvm::StringRef();
}

std::size_t
EntryPoint::arg_size() const {
    return d_argument_count;
}

void
EntryPoint::dump() const {
    print(llvm::dbgs());
}

void
EntryPoint::updateArgumentMetadata(Argument *argument) {
    d_arguments_md->replaceOperandWith(argument->d_md_index, argument->d_md.get());
}

// EntryPoint::Argument
EntryPoint::Argument::Argument(llvm::Argument *function_argument, EntryPoint *entry_point)
    : d_function_argument(function_argument)
    , d_parent(entry_point) {
    auto &ll_context = d_parent->getModule()->getLLContext();

    d_md.reset(llvm::MDTuple::get(
        ll_context, {
                        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                            ll_context, llvm::APInt(32, d_function_argument->getArgNo(), true))),
                        llvm::MDString::get(ll_context, "air.arg_name"),
                        llvm::MDString::get(ll_context, ""),
                        llvm::MDString::get(ll_context, "air.arg_type_name"),
                        llvm::MDString::get(ll_context, ""),
                    }));

    d_name_md_index      = 2;
    d_type_name_md_index = 4;
}

EntryPoint::Argument::Argument(llvm::Argument *function_argument, llvm::MDTuple *md,
                               EntryPoint *entry_point)
    : d_function_argument(function_argument)
    , d_parent(entry_point) {
    d_md.reset(md);

    for (auto it = d_md->op_begin(), it_end = d_md->op_end(); it != it_end;) {
        auto string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
        if (!string_md) {
            continue;
        }

        auto string = string_md->getString();

        if (string == "air.arg_name") {
            string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
            if (!string_md) {
                continue;
            }

            d_name_md_index = std::distance(d_md->op_begin(), it) - 1;
        }
        else if (string == "air.arg_type_name") {
            string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
            if (!string_md) {
                continue;
            }

            d_type_name_md_index = std::distance(d_md->op_begin(), it) - 1;
        }
        else if (string == "air.buffer" || string == "air.indirect_buffer" ||
                 string == "air.texture") {
            std::optional<unsigned int> location0, location1;
            std::optional<Access>       access;

            for (auto it1 = d_md->op_begin(), it1_end = d_md->op_end(); it1 != it1_end;) {
                string_md = llvm::dyn_cast<llvm::MDString>(it1++->get());
                if (!string_md) {
                    continue;
                }

                auto keyword = string_md->getString();

                if (keyword == "air.location_index") {
                    auto location0_md = llvm::mdconst::dyn_extract<llvm::ConstantInt>(it1++->get());
                    if (!location0_md) {
                        continue;
                    }

                    location0 = location0_md->getZExtValue();

                    auto location1_md = llvm::mdconst::dyn_extract<llvm::ConstantInt>(it1++->get());
                    if (!location1_md) {
                        continue;
                    }

                    location1 = location1_md->getZExtValue();
                }
                else if (keyword == "air.read") {
                    access = Access::kReadOnly;
                }
                else if (keyword == "air.read_write") {
                    access = Access::kReadWrite;
                }
            }

            if (!location0 || !location1 || !access) {
                continue;
            }

            if (string == "air.buffer" || string == "air.indirect_buffer") {
                std::optional<unsigned int> buffer_size, type_size, type_align_size;
                llvm::Optional<Interface *> interface_type;

                for (auto it1 = d_md->op_begin(), it1_end = d_md->op_end(); it1 != it1_end;) {
                    string_md = llvm::dyn_cast<llvm::MDString>(it1++->get());
                    if (!string_md) {
                        continue;
                    }

                    auto keyword = string_md->getString();

                    if (keyword == "air.buffer_size") {
                        auto buffer_size_md =
                            llvm::mdconst::dyn_extract<llvm::ConstantInt>(it1++->get());
                        if (!buffer_size_md) {
                            continue;
                        }

                        buffer_size = buffer_size_md->getZExtValue();
                    }
                    else if (keyword == "air.arg_type_size") {
                        auto type_size_md =
                            llvm::mdconst::dyn_extract<llvm::ConstantInt>(it1++->get());
                        if (!type_size_md) {
                            continue;
                        }

                        type_size = type_size_md->getZExtValue();
                    }
                    else if (keyword == "air.arg_type_align_size") {
                        auto type_align_size_md =
                            llvm::mdconst::dyn_extract<llvm::ConstantInt>(it1++->get());
                        if (!type_align_size_md) {
                            continue;
                        }

                        type_align_size = type_align_size_md->getZExtValue();
                    }
                    else if (keyword == "llair.interface_type") {
                        auto interface_type_md = it1++->get();
                        if (!interface_type_md) {
                            continue;
                        }

                        interface_type = Interface::get(interface_type_md);
                    }
                }

                if (!type_size || !type_align_size) {
                    continue;
                }

                if (string == "air.buffer") {
                    d_details =
                        Buffer({*location0, *location1, *access, *type_size, *type_align_size, interface_type});
                }
                else if (string == "air.indirect_buffer") {
                    d_details = IndirectBuffer(
                        {*location0, *location1, *access, *type_size, *type_align_size, interface_type});
                }
            }
            else if (string == "air.texture") {
                d_details = Texture({*location0, *location1, *access});
            }
        }
        else if (string == "air.vertex_input") {
            std::optional<unsigned int> location0, location1;
            std::optional<std::string>  identifier;

            for (auto it1 = d_md->op_begin(), it1_end = d_md->op_end(); it1 != it1_end;) {
                string_md = llvm::dyn_cast<llvm::MDString>(it1++->get());
                if (!string_md) {
                    continue;
                }

                auto keyword = string_md->getString();

                if (keyword == "air.location_index") {
                    auto location0_md = llvm::mdconst::dyn_extract<llvm::ConstantInt>(it1++->get());
                    if (!location0_md) {
                        continue;
                    }

                    location0 = location0_md->getZExtValue();

                    auto location1_md = llvm::mdconst::dyn_extract<llvm::ConstantInt>(it1++->get());
                    if (!location1_md) {
                        continue;
                    }

                    location1 = location1_md->getZExtValue();

                    auto identifier_md = llvm::dyn_cast<llvm::MDString>(it++->get());
                    if (!identifier_md) {
                        continue;
                    }

                    identifier = identifier_md->getString();
                }
            }

            if (!location0 || !location1 || !identifier) {
                continue;
            }

            d_details = VertexInput({*location0, *location1, *identifier});
        }
        else if (string == "air.position" || string == "air.fragment_input") {
            // Determine interpolation:
            EntryPoint::Argument::Interpolation interpolation =
                EntryPoint::Argument::Interpolation::kCenterPerspective;

            for (auto it1 = d_md->op_begin(), it1_end = d_md->op_end(); it1 != it1_end;) {
                string_md = llvm::dyn_cast<llvm::MDString>(it1++->get());
                if (!string_md) {
                    continue;
                }

                auto interpolation1 = string_md->getString();

                if (interpolation1 == "air.center" || interpolation1 == "air.centroid" ||
                    interpolation1 == "air.sample") {
                    string_md = llvm::dyn_cast<llvm::MDString>(it1++->get());
                    if (!string_md) {
                        continue;
                    }

                    auto interpolation2 = string_md->getString();

                    if (interpolation1 == "air.center") {
                        if (interpolation2 == "air.no_perspective") {
                            interpolation =
                                EntryPoint::Argument::Interpolation::kCenterNoPerspective;
                        }
                        else if (interpolation2 == "air.perspective") {
                            interpolation = EntryPoint::Argument::Interpolation::kCenterPerspective;
                        }
                    }
                    else if (interpolation1 == "air.centroid") {
                        if (interpolation2 == "air.no_perspective") {
                            interpolation =
                                EntryPoint::Argument::Interpolation::kCentroidNoPerspective;
                        }
                        else if (interpolation2 == "air.perspective") {
                            interpolation =
                                EntryPoint::Argument::Interpolation::kCentroidPerspective;
                        }
                    }
                    else if (interpolation1 == "air.sample") {
                        if (interpolation2 == "air.no_perspective") {
                            interpolation =
                                EntryPoint::Argument::Interpolation::kSampleNoPerspective;
                        }
                        else if (interpolation2 == "air.perspective") {
                            interpolation = EntryPoint::Argument::Interpolation::kSamplePerspective;
                        }
                    }
                }
                else if (interpolation1 == "air.flat") {
                    interpolation = EntryPoint::Argument::Interpolation::kFlat;
                }
            }

            // Populate details:
            if (string == "air.position") {
                d_details = Position({interpolation});
            }
            else if (string == "air.fragment_input") {
                string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
                if (!string_md) {
                    continue;
                }

                auto identifier = string_md->getString();

                d_details = FragmentInput({identifier, interpolation});
            }
        }
    }
}

void
EntryPoint::Argument::updateMetadataInEntryPoint() {
    if (!d_parent) {
        return;
    }

    d_parent->updateArgumentMetadata(this);
}

void
EntryPoint::Argument::InitDetailsAsBuffer(const Buffer &buffer) {
    d_details = buffer;

    auto &ll_context = d_parent->getModule()->getLLContext();

    std::vector<llvm::Metadata *> mds = {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
            ll_context, llvm::APInt(32, d_function_argument->getArgNo(), true))),
        llvm::MDString::get(ll_context, "air.buffer"),
        llvm::MDString::get(ll_context, "air.location_index"),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(ll_context, llvm::APInt(32, buffer.location0, true))),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(ll_context, llvm::APInt(32, buffer.location1, true))),
        buffer.access == Access::kReadOnly ? llvm::MDString::get(ll_context, "air.read")
                                           : llvm::MDString::get(ll_context, "air.read_write"),
        llvm::MDString::get(ll_context, "air.arg_type_size"),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(ll_context, llvm::APInt(32, buffer.type_size, true))),
        llvm::MDString::get(ll_context, "air.arg_type_align_size"),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(ll_context, llvm::APInt(32, buffer.type_align_size, true))),
        llvm::MDString::get(ll_context, "air.arg_type_name"),
        llvm::MDString::get(ll_context, getTypeName()),
        llvm::MDString::get(ll_context, "air.arg_name"),
        llvm::MDString::get(ll_context, getName())
    };

    d_type_name_md_index = 11;
    d_name_md_index      = 13;

    if (buffer.interface_type && *buffer.interface_type != nullptr) {
        d_interface_type_md_index = mds.size();

        mds.insert(mds.end(), {
            llvm::MDString::get(ll_context, "llair.interface_type"),
            (*buffer.interface_type)->metadata()
        });
    }
    else {
        d_interface_type_md_index.reset();
    }

    d_md.reset(llvm::MDTuple::get(ll_context, mds));

    updateMetadataInEntryPoint();
}

void
EntryPoint::Argument::InitDetailsAsBuffer(unsigned location0, unsigned location1, Access access, llvm::Optional<Interface *> interface_type) {
    auto type = llvm::cast<llvm::PointerType>(d_function_argument->getType())->getElementType();

    const auto& data_layout = LLAIRContext::Get(&type->getContext())->getDataLayout();

    InitDetailsAsBuffer({
        location0, location1, access,
        (unsigned)data_layout.getTypeAllocSize(type),
        (unsigned)data_layout.getABITypeAlignment(type),
        interface_type});
}

void
EntryPoint::Argument::InitDetailsAsIndirectBuffer(const IndirectBuffer &buffer) {
    d_details = buffer;

    auto &ll_context = d_parent->getModule()->getLLContext();

    std::vector<llvm::Metadata *> mds = {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
            ll_context, llvm::APInt(32, d_function_argument->getArgNo(), true))),
        llvm::MDString::get(ll_context, "air.indirect_buffer"),
        llvm::MDString::get(ll_context, "air.location_index"),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(ll_context, llvm::APInt(32, buffer.location0, true))),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(ll_context, llvm::APInt(32, buffer.location1, true))),
        buffer.access == Access::kReadOnly ? llvm::MDString::get(ll_context, "air.read")
                                        : llvm::MDString::get(ll_context, "air.read_write"),
        llvm::MDString::get(ll_context, "air.arg_type_size"),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(ll_context, llvm::APInt(32, buffer.type_size, true))),
        llvm::MDString::get(ll_context, "air.arg_type_align_size"),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(ll_context, llvm::APInt(32, buffer.type_align_size, true))),
        llvm::MDString::get(ll_context, "air.arg_type_name"),
        llvm::MDString::get(ll_context, getTypeName()),
        llvm::MDString::get(ll_context, "air.arg_name"),
        llvm::MDString::get(ll_context, getName())
    };

    d_type_name_md_index = 11;
    d_name_md_index      = 13;

    if (buffer.interface_type && *buffer.interface_type != nullptr) {
        d_interface_type_md_index = mds.size();

        mds.insert(mds.end(), {
            llvm::MDString::get(ll_context, "llair.interface_type"),
            (*buffer.interface_type)->metadata()
        });
    }
    else {
        d_interface_type_md_index.reset();
    }

    d_md.reset(llvm::MDTuple::get(ll_context, mds));

    updateMetadataInEntryPoint();
}

void
EntryPoint::Argument::InitDetailsAsIndirectBuffer(unsigned location0, unsigned location1, Access access, llvm::Optional<Interface *> interface_type) {
    auto type = llvm::cast<llvm::PointerType>(d_function_argument->getType())->getElementType();

    const auto& data_layout = LLAIRContext::Get(&type->getContext())->getDataLayout();

    InitDetailsAsIndirectBuffer({
        location0, location1, access,
        (unsigned)data_layout.getTypeAllocSize(type),
        (unsigned)data_layout.getABITypeAlignment(type),
        interface_type});
}

void
EntryPoint::Argument::InitDetailsAsTexture(const Texture &texture) {
    d_details = texture;

    auto &ll_context = d_parent->getModule()->getLLContext();

    d_md.reset(llvm::MDTuple::get(
        ll_context,
        {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
             ll_context, llvm::APInt(32, d_function_argument->getArgNo(), true))),
         llvm::MDString::get(ll_context, "air.texture"),
         llvm::MDString::get(ll_context, "air.location_index"),
         llvm::ConstantAsMetadata::get(
             llvm::ConstantInt::get(ll_context, llvm::APInt(32, texture.location0, true))),
         llvm::ConstantAsMetadata::get(
             llvm::ConstantInt::get(ll_context, llvm::APInt(32, texture.location1, true))),
         texture.access == Access::kReadOnly ? llvm::MDString::get(ll_context, "air.read")
                                             : llvm::MDString::get(ll_context, "air.read_write"),
         llvm::MDString::get(ll_context, "air.arg_type_name"),
         llvm::MDString::get(ll_context, getTypeName()),
         llvm::MDString::get(ll_context, "air.arg_name"),
         llvm::MDString::get(ll_context, getName())}));

    d_type_name_md_index = 7;
    d_name_md_index      = 9;

    updateMetadataInEntryPoint();
}

void
EntryPoint::Argument::InitDetailsAsVertexInput(const VertexInput &vertex_input) {
    d_details = vertex_input;

    // TODO
}

void
EntryPoint::Argument::InitDetailsAsPosition(const Position &position) {
    d_details = position;

    auto &ll_context = d_parent->getModule()->getLLContext();

    std::vector<llvm::Metadata *> mds;

    mds.push_back(llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
        ll_context, llvm::APInt(32, d_function_argument->getArgNo(), true))));

    mds.push_back(llvm::MDString::get(ll_context, "air.position"));

    switch (position.interpolation) {
    case Interpolation::kCenterPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.center"));
        mds.push_back(llvm::MDString::get(ll_context, "air.perspective"));
    } break;
    case Interpolation::kCenterNoPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.center"));
        mds.push_back(llvm::MDString::get(ll_context, "air.no_perspective"));
    } break;
    case Interpolation::kCentroidPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.centroid"));
        mds.push_back(llvm::MDString::get(ll_context, "air.perspective"));
    } break;
    case Interpolation::kCentroidNoPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.centroid"));
        mds.push_back(llvm::MDString::get(ll_context, "air.no_perspective"));
    } break;
    case Interpolation::kSamplePerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.sample"));
        mds.push_back(llvm::MDString::get(ll_context, "air.perspective"));
    } break;
    case Interpolation::kSampleNoPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.sample"));
        mds.push_back(llvm::MDString::get(ll_context, "air.no_perspective"));
    } break;
    case Interpolation::kFlat: {
        mds.push_back(llvm::MDString::get(ll_context, "air.flat"));
    } break;
    }

    mds.push_back(llvm::MDString::get(ll_context, "air.arg_type_name"));
    mds.push_back(llvm::MDString::get(ll_context, getTypeName()));
    d_type_name_md_index = mds.size() - 1;
    mds.push_back(llvm::MDString::get(ll_context, "air.arg_name"));
    mds.push_back(llvm::MDString::get(ll_context, getName()));
    d_name_md_index = mds.size() - 1;

    d_md.reset(llvm::MDTuple::get(ll_context, mds));

    updateMetadataInEntryPoint();
}

void
EntryPoint::Argument::InitDetailsAsFragmentInput(const FragmentInput &fragment_input) {
    d_details = fragment_input;

    auto &ll_context = d_parent->getModule()->getLLContext();

    std::vector<llvm::Metadata *> mds;

    mds.push_back(llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
        ll_context, llvm::APInt(32, d_function_argument->getArgNo(), true))));

    mds.push_back(llvm::MDString::get(ll_context, "air.fragment_input"));

    mds.push_back(llvm::MDString::get(ll_context, fragment_input.identifier));

    switch (fragment_input.interpolation) {
    case Interpolation::kCenterPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.center"));
        mds.push_back(llvm::MDString::get(ll_context, "air.perspective"));
    } break;
    case Interpolation::kCenterNoPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.center"));
        mds.push_back(llvm::MDString::get(ll_context, "air.no_perspective"));
    } break;
    case Interpolation::kCentroidPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.centroid"));
        mds.push_back(llvm::MDString::get(ll_context, "air.perspective"));
    } break;
    case Interpolation::kCentroidNoPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.centroid"));
        mds.push_back(llvm::MDString::get(ll_context, "air.no_perspective"));
    } break;
    case Interpolation::kSamplePerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.sample"));
        mds.push_back(llvm::MDString::get(ll_context, "air.perspective"));
    } break;
    case Interpolation::kSampleNoPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.sample"));
        mds.push_back(llvm::MDString::get(ll_context, "air.no_perspective"));
    } break;
    case Interpolation::kFlat: {
        mds.push_back(llvm::MDString::get(ll_context, "air.flat"));
    } break;
    }

    mds.push_back(llvm::MDString::get(ll_context, "air.arg_type_name"));
    mds.push_back(llvm::MDString::get(ll_context, getTypeName()));
    d_type_name_md_index = mds.size() - 1;
    mds.push_back(llvm::MDString::get(ll_context, "air.arg_name"));
    mds.push_back(llvm::MDString::get(ll_context, getName()));
    d_name_md_index = mds.size() - 1;

    d_md.reset(llvm::MDTuple::get(ll_context, mds));

    updateMetadataInEntryPoint();
}

unsigned
EntryPoint::Argument::getArgNo() const {
    return d_function_argument->getArgNo();
}

llvm::StringRef
EntryPoint::Argument::getName() const {
    if (!d_name_md_index) {
        return "";
    }

    return llvm::cast<llvm::MDString>(d_md->getOperand(*d_name_md_index).get())->getString();
}

void
EntryPoint::Argument::setName(llvm::StringRef name) {
    if (!d_md || !d_name_md_index) {
        return;
    }

    d_md->replaceOperandWith(*d_name_md_index,
                             llvm::MDString::get(d_parent->getModule()->getLLContext(), name));
}

llvm::StringRef
EntryPoint::Argument::getTypeName() const {
    if (!d_type_name_md_index) {
        return "";
    }

    return llvm::cast<llvm::MDString>(d_md->getOperand(*d_type_name_md_index).get())->getString();
}

void
EntryPoint::Argument::setTypeName(llvm::StringRef type_name) {
    if (!d_md || !d_type_name_md_index) {
        return;
    }

    d_md->replaceOperandWith(*d_type_name_md_index,
                             llvm::MDString::get(d_parent->getModule()->getLLContext(), type_name));
}

// VertexEntryPoint
VertexEntryPoint *
VertexEntryPoint::Create(llvm::Function *function, unsigned output_count, Module *module) {
    return new VertexEntryPoint(function, output_count, module);
}

VertexEntryPoint::VertexEntryPoint(llvm::Function *function, unsigned output_count, Module *module)
    : EntryPoint(EntryPoint::Vertex, function, module) {
    d_output_count = output_count;

    d_outputs = std::allocator<Output>().allocate(d_output_count);

    std::accumulate(d_outputs, d_outputs + d_output_count,
                    d_outputs, [=](auto output, auto &) -> auto {
                        new (output) Output(this);
                        return ++output;
                    });

    std::vector<llvm::Metadata *> mds;
    unsigned                      index = 0;
    std::transform(d_outputs, d_outputs + d_output_count, std::back_inserter(mds),
                   [&index](auto &output) -> llvm::Metadata * {
                       output.d_md_index = index++;
                       return output.d_md.get();
                   });

    auto &ll_context = d_module->getLLContext();

    d_outputs_md.reset(llvm::MDTuple::get(ll_context, mds));

    d_md.reset(llvm::MDTuple::get(ll_context,
                                  {d_function_md.get(), d_outputs_md.get(), d_arguments_md.get()}));

    if (module) {
        module->getEntryPointList().push_back(this);
    }
}

VertexEntryPoint::VertexEntryPoint(llvm::MDNode *md, Module *module)
    : EntryPoint(EntryPoint::Vertex, md, module)
    , d_outputs(nullptr) {
    // Outputs:
    d_outputs_md.reset(llvm::cast<llvm::MDTuple>(d_md->getOperand(1).get()));

    d_output_count = d_outputs_md->getNumOperands();

    d_outputs = std::allocator<Output>().allocate(d_output_count);

    std::accumulate(d_outputs_md->op_begin(), d_outputs_md->op_end(),
                    d_outputs, [=](auto output, auto &operand) -> auto {
                        new (output) Output(llvm::cast<llvm::MDTuple>(operand.get()), this);
                        return ++output;
                    });
}

VertexEntryPoint::~VertexEntryPoint() {
    if (d_outputs) {
        std::for_each(d_outputs, d_outputs + d_output_count,
                      [](auto &output) -> void { output.~Output(); });

        std::allocator<Output>().deallocate(d_outputs, d_output_count);
    }
}

std::size_t
VertexEntryPoint::output_size() const {
    return d_output_count;
}

void
VertexEntryPoint::print(llvm::raw_ostream& os) const {
    os << "vertex program ";
    os << getName() << "\n";
}

void
VertexEntryPoint::updateOutputMetadata(Output *output) {
    d_outputs_md->replaceOperandWith(output->d_md_index, output->d_md.get());
    d_outputs_md->resolve();
}

// VertexEntryPoint::Output
VertexEntryPoint::Output::Output(VertexEntryPoint *entry_point)
    : d_parent(entry_point) {
    auto &ll_context = d_parent->getModule()->getLLContext();

    d_md.reset(
        llvm::MDTuple::get(ll_context, {
                                           llvm::MDString::get(ll_context, "air.arg_type_name"),
                                           llvm::MDString::get(ll_context, ""),
                                           llvm::MDString::get(ll_context, "air.arg_name"),
                                           llvm::MDString::get(ll_context, ""),
                                       }));

    d_type_name_md_index = 1;
    d_name_md_index      = 3;
}

VertexEntryPoint::Output::Output(llvm::MDTuple *md, VertexEntryPoint *entry_point)
    : d_parent(entry_point) {
    d_md.reset(md);

    for (auto it = d_md->op_begin(), it_end = d_md->op_end(); it != it_end;) {
        auto string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
        if (!string_md) {
            continue;
        }

        auto string = string_md->getString();

        if (string == "air.arg_name") {
            string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
            if (!string_md) {
                continue;
            }

            d_name_md_index = std::distance(d_md->op_begin(), it) - 1;
        }
        else if (string == "air.arg_type_name") {
            string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
            if (!string_md) {
                continue;
            }

            d_type_name_md_index = std::distance(d_md->op_begin(), it) - 1;
        }
        else if (string == "air.position") {
            d_details = Position();
        }
        else if (string == "air.vertex_output") {
            // Determine interpolation:
            VertexEntryPoint::Output::Interpolation interpolation =
                VertexEntryPoint::Output::Interpolation::kCenterPerspective;

            for (auto it1 = d_md->op_begin(), it1_end = d_md->op_end(); it1 != it1_end;) {
                string_md = llvm::dyn_cast<llvm::MDString>(it1++->get());
                if (!string_md) {
                    continue;
                }

                auto interpolation1 = string_md->getString();

                if (interpolation1 == "air.center" || interpolation1 == "air.centroid" ||
                    interpolation1 == "air.sample") {
                    string_md = llvm::dyn_cast<llvm::MDString>(it1++->get());
                    if (!string_md) {
                        continue;
                    }

                    auto interpolation2 = string_md->getString();

                    if (interpolation1 == "air.center") {
                        if (interpolation2 == "air.no_perspective") {
                            interpolation =
                                VertexEntryPoint::Output::Interpolation::kCenterNoPerspective;
                        }
                        else if (interpolation2 == "air.perspective") {
                            interpolation =
                                VertexEntryPoint::Output::Interpolation::kCenterPerspective;
                        }
                    }
                    else if (interpolation1 == "air.centroid") {
                        if (interpolation2 == "air.no_perspective") {
                            interpolation =
                                VertexEntryPoint::Output::Interpolation::kCentroidNoPerspective;
                        }
                        else if (interpolation2 == "air.perspective") {
                            interpolation =
                                VertexEntryPoint::Output::Interpolation::kCentroidPerspective;
                        }
                    }
                    else if (interpolation1 == "air.sample") {
                        if (interpolation2 == "air.no_perspective") {
                            interpolation =
                                VertexEntryPoint::Output::Interpolation::kSampleNoPerspective;
                        }
                        else if (interpolation2 == "air.perspective") {
                            interpolation =
                                VertexEntryPoint::Output::Interpolation::kSamplePerspective;
                        }
                    }
                }
                else if (interpolation1 == "air.flat") {
                    interpolation = VertexEntryPoint::Output::Interpolation::kFlat;
                }
            }

            // Populate details:
            string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
            if (!string_md) {
                continue;
            }

            auto identifier = string_md->getString();

            d_details = VertexOutput({identifier, interpolation});
        }
    }
}

void
VertexEntryPoint::Output::updateMetadataInEntryPoint() {
    if (!d_parent) {
        return;
    }

    d_parent->updateOutputMetadata(this);
}

void
VertexEntryPoint::Output::InitDetailsAsPosition(const Position &position) {
    d_details = position;

    auto &ll_context = d_parent->getModule()->getLLContext();

    std::vector<llvm::Metadata *> mds;

    mds.push_back(llvm::MDString::get(ll_context, "air.position"));

    mds.push_back(llvm::MDString::get(ll_context, "air.arg_type_name"));
    mds.push_back(llvm::MDString::get(ll_context, getTypeName()));
    d_type_name_md_index = mds.size() - 1;
    mds.push_back(llvm::MDString::get(ll_context, "air.arg_name"));
    mds.push_back(llvm::MDString::get(ll_context, getName()));
    d_name_md_index = mds.size() - 1;

    d_md.reset(llvm::MDTuple::get(ll_context, mds));

    updateMetadataInEntryPoint();
}

void
VertexEntryPoint::Output::InitDetailsAsVertexOutput(const VertexOutput &vertex_output) {
    d_details = vertex_output;

    auto &ll_context = d_parent->getModule()->getLLContext();

    std::vector<llvm::Metadata *> mds;

    mds.push_back(llvm::MDString::get(ll_context, "air.vertex_output"));

    mds.push_back(llvm::MDString::get(ll_context, vertex_output.identifier));

    mds.push_back(llvm::MDString::get(ll_context, "air.arg_type_name"));
    mds.push_back(llvm::MDString::get(ll_context, getTypeName()));
    d_type_name_md_index = mds.size() - 1;
    mds.push_back(llvm::MDString::get(ll_context, "air.arg_name"));
    mds.push_back(llvm::MDString::get(ll_context, getName()));
    d_name_md_index = mds.size() - 1;

    switch (vertex_output.interpolation) {
    case Interpolation::kCenterPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.center"));
        mds.push_back(llvm::MDString::get(ll_context, "air.perspective"));
    } break;
    case Interpolation::kCenterNoPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.center"));
        mds.push_back(llvm::MDString::get(ll_context, "air.no_perspective"));
    } break;
    case Interpolation::kCentroidPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.centroid"));
        mds.push_back(llvm::MDString::get(ll_context, "air.perspective"));
    } break;
    case Interpolation::kCentroidNoPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.centroid"));
        mds.push_back(llvm::MDString::get(ll_context, "air.no_perspective"));
    } break;
    case Interpolation::kSamplePerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.sample"));
        mds.push_back(llvm::MDString::get(ll_context, "air.perspective"));
    } break;
    case Interpolation::kSampleNoPerspective: {
        mds.push_back(llvm::MDString::get(ll_context, "air.sample"));
        mds.push_back(llvm::MDString::get(ll_context, "air.no_perspective"));
    } break;
    case Interpolation::kFlat: {
        mds.push_back(llvm::MDString::get(ll_context, "air.flat"));
    } break;
    }

    d_md.reset(llvm::MDTuple::get(ll_context, mds));

    updateMetadataInEntryPoint();
}

llvm::StringRef
VertexEntryPoint::Output::getName() const {
    if (!d_name_md_index) {
        return "";
    }

    return llvm::cast<llvm::MDString>(d_md->getOperand(*d_name_md_index).get())->getString();
}

void
VertexEntryPoint::Output::setName(llvm::StringRef name) {
    if (!d_md || !d_name_md_index) {
        return;
    }

    d_md->replaceOperandWith(*d_name_md_index,
                             llvm::MDString::get(d_parent->getModule()->getLLContext(), name));
}

llvm::StringRef
VertexEntryPoint::Output::getTypeName() const {
    if (!d_type_name_md_index) {
        return "";
    }

    return llvm::cast<llvm::MDString>(d_md->getOperand(*d_type_name_md_index).get())->getString();
}

void
VertexEntryPoint::Output::setTypeName(llvm::StringRef type_name) {
    if (!d_md || !d_type_name_md_index) {
        return;
    }

    d_md->replaceOperandWith(*d_type_name_md_index,
                             llvm::MDString::get(d_parent->getModule()->getLLContext(), type_name));
}

// FragmentEntryPoint
FragmentEntryPoint *
FragmentEntryPoint::Create(llvm::Function *function, bool early_fragment_tests_enabled,
                           unsigned output_count, Module *module) {
    return new FragmentEntryPoint(function, early_fragment_tests_enabled, output_count, module);
}

FragmentEntryPoint::FragmentEntryPoint(llvm::Function *function, bool early_fragment_tests_enabled,
                                       unsigned output_count, Module *module)
    : EntryPoint(EntryPoint::Fragment, function, module)
    , d_early_fragment_tests_enabled(early_fragment_tests_enabled) {
    d_output_count = output_count;

    d_outputs = std::allocator<Output>().allocate(d_output_count);

    std::accumulate(d_outputs, d_outputs + d_output_count,
                    d_outputs, [=](auto output, auto &) -> auto {
                        new (output) Output(this);
                        return ++output;
                    });

    std::vector<llvm::Metadata *> mds;
    unsigned                      index = 0;
    std::transform(d_outputs, d_outputs + d_output_count, std::back_inserter(mds),
                   [&index](auto &output) -> llvm::Metadata * {
                       output.d_md_index = index++;
                       return output.d_md.get();
                   });

    auto &ll_context = d_module->getLLContext();

    d_outputs_md.reset(llvm::MDTuple::get(ll_context, mds));

    if (d_early_fragment_tests_enabled) {
        d_md.reset(llvm::MDTuple::get(
            ll_context, {d_function_md.get(), d_outputs_md.get(), d_arguments_md.get(),
                         llvm::MDString::get(ll_context, "early_fragment_tests")}));
    }
    else {
        d_md.reset(llvm::MDTuple::get(
            ll_context, {d_function_md.get(), d_outputs_md.get(), d_arguments_md.get()}));
    }

    if (module) {
        module->getEntryPointList().push_back(this);
    }
}

FragmentEntryPoint::FragmentEntryPoint(llvm::MDNode *md, Module *module)
    : EntryPoint(EntryPoint::Fragment, md, module)
    , d_outputs(nullptr) {
    for (auto it = d_md->op_begin(), it_end = d_md->op_end(); it != it_end;) {
        auto string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
        if (!string_md) {
            continue;
        }

        auto keyword = string_md->getString();

        if (keyword == "early_fragment_tests") {
            d_early_fragment_tests_enabled = true;
        }
    }

    // Outputs:
    d_outputs_md.reset(llvm::cast<llvm::MDTuple>(d_md->getOperand(1).get()));

    d_output_count = d_outputs_md->getNumOperands();

    d_outputs = std::allocator<Output>().allocate(d_output_count);

    std::accumulate(d_outputs_md->op_begin(), d_outputs_md->op_end(),
                    d_outputs, [=](auto output, auto &operand) -> auto {
                        new (output) Output(llvm::cast<llvm::MDTuple>(operand.get()), this);
                        return ++output;
                    });
}

FragmentEntryPoint::~FragmentEntryPoint() {
    if (d_outputs) {
        std::for_each(d_outputs, d_outputs + d_output_count,
                      [](auto &output) -> void { output.~Output(); });

        std::allocator<Output>().deallocate(d_outputs, d_output_count);
    }
}

std::size_t
FragmentEntryPoint::output_size() const {
    return d_output_count;
}

void
FragmentEntryPoint::print(llvm::raw_ostream& os) const {
    os << "fragment program ";
    os << getName() << "\n";
}

void
FragmentEntryPoint::updateOutputMetadata(Output *output) {
    d_outputs_md->replaceOperandWith(output->d_md_index, output->d_md.get());
}

// FragmentEntryPoint::Output
FragmentEntryPoint::Output::Output(FragmentEntryPoint *entry_point)
    : d_parent(entry_point) {
    auto &ll_context = d_parent->getModule()->getLLContext();

    d_md.reset(
        llvm::MDTuple::get(ll_context, {
                                           llvm::MDString::get(ll_context, "air.arg_type_name"),
                                           llvm::MDString::get(ll_context, ""),
                                           llvm::MDString::get(ll_context, "air.arg_name"),
                                           llvm::MDString::get(ll_context, ""),
                                       }));

    d_type_name_md_index = 1;
    d_name_md_index      = 3;
}

FragmentEntryPoint::Output::Output(llvm::MDTuple *md, FragmentEntryPoint *entry_point)
    : d_parent(entry_point) {
    d_md.reset(md);

    for (auto it = d_md->op_begin(), it_end = d_md->op_end(); it != it_end;) {
        auto string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
        if (!string_md) {
            continue;
        }

        auto string = string_md->getString();

        if (string == "air.arg_name") {
            string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
            if (!string_md) {
                continue;
            }

            d_name_md_index = std::distance(d_md->op_begin(), it) - 1;
        }
        else if (string == "air.arg_type_name") {
            string_md = llvm::dyn_cast<llvm::MDString>(it++->get());
            if (!string_md) {
                continue;
            }

            d_type_name_md_index = std::distance(d_md->op_begin(), it) - 1;
        }
        else if (string == "air.depth") {
            std::optional<Depth::Qualifier> qualifier;

            for (auto it1 = d_md->op_begin(), it1_end = d_md->op_end(); it1 != it1_end;) {
                string_md = llvm::dyn_cast<llvm::MDString>(it1++->get());
                if (!string_md) {
                    continue;
                }

                auto keyword = string_md->getString();

                if (keyword == "air.depth_qualifier") {
                    auto qualifier_md = llvm::dyn_cast<llvm::MDString>(it1++->get());
                    if (!qualifier_md) {
                        continue;
                    }

                    auto qualifier_string = qualifier_md->getString();

                    if (qualifier_string == "air.any") {
                        qualifier = Depth::Qualifier::kAny;
                    }
                    else if (qualifier_string == "air.greater") {
                        qualifier = Depth::Qualifier::kGreater;
                    }
                    else if (qualifier_string == "air.less") {
                        qualifier = Depth::Qualifier::kLess;
                    }
                }
            }

            if (!qualifier) {
                continue;
            }

            d_details = Depth({*qualifier});
        }
        else if (string == "air.render_target") {
            std::optional<unsigned int> location0, location1;

            for (auto it1 = it, it1_end = d_md->op_end(); it1 != it1_end;) {
                auto location0_md = llvm::mdconst::dyn_extract<llvm::ConstantInt>(it1++->get());
                if (!location0_md) {
                    continue;
                }

                location0 = location0_md->getZExtValue();

                auto location1_md = llvm::mdconst::dyn_extract<llvm::ConstantInt>(it1++->get());
                if (!location1_md) {
                    continue;
                }

                location1 = location1_md->getZExtValue();

                break;
            }

            if (!location0 || !location1) {
                continue;
            }

            // Populate details:
            d_details = RenderTarget({*location0, *location1});
        }
    }
}

void
FragmentEntryPoint::Output::updateMetadataInEntryPoint() {
    if (!d_parent) {
        return;
    }

    d_parent->updateOutputMetadata(this);
}

void
FragmentEntryPoint::Output::InitDetailsAsDepth(const Depth &depth) {
    d_details = depth;

    auto &ll_context = d_parent->getModule()->getLLContext();

    std::vector<llvm::Metadata *> mds;

    mds.push_back(llvm::MDString::get(ll_context, "air.depth"));

    mds.push_back(llvm::MDString::get(ll_context, "air.depth_qualifier"));
    switch (depth.qualifier) {
    case Depth::kAny: {
        mds.push_back(llvm::MDString::get(ll_context, "air.any"));
    } break;
    case Depth::kGreater: {
        mds.push_back(llvm::MDString::get(ll_context, "air.greater"));
    } break;
    case Depth::kLess: {
        mds.push_back(llvm::MDString::get(ll_context, "air.less"));
    } break;
    }

    mds.push_back(llvm::MDString::get(ll_context, "air.arg_type_name"));
    mds.push_back(llvm::MDString::get(ll_context, getTypeName()));
    d_type_name_md_index = mds.size() - 1;
    mds.push_back(llvm::MDString::get(ll_context, "air.arg_name"));
    mds.push_back(llvm::MDString::get(ll_context, getName()));
    d_name_md_index = mds.size() - 1;

    d_md.reset(llvm::MDTuple::get(ll_context, mds));

    updateMetadataInEntryPoint();
}

void
FragmentEntryPoint::Output::InitDetailsAsRenderTarget(const RenderTarget &render_target) {
    d_details = render_target;

    auto &ll_context = d_parent->getModule()->getLLContext();

    d_md.reset(llvm::MDTuple::get(ll_context,
                                  {llvm::MDString::get(ll_context, "air.render_target"),
                                   llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                                       ll_context, llvm::APInt(32, render_target.location0, true))),
                                   llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                                       ll_context, llvm::APInt(32, render_target.location1, true))),
                                   llvm::MDString::get(ll_context, "air.arg_type_name"),
                                   llvm::MDString::get(ll_context, getTypeName()),
                                   llvm::MDString::get(ll_context, "air.arg_name"),
                                   llvm::MDString::get(ll_context, getName())}));

    d_type_name_md_index = 4;
    d_name_md_index      = 6;

    updateMetadataInEntryPoint();
}

llvm::StringRef
FragmentEntryPoint::Output::getName() const {
    if (!d_name_md_index) {
        return "";
    }

    return llvm::cast<llvm::MDString>(d_md->getOperand(*d_name_md_index).get())->getString();
}

void
FragmentEntryPoint::Output::setName(llvm::StringRef name) {
    if (!d_md || !d_name_md_index) {
        return;
    }

    d_md->replaceOperandWith(*d_name_md_index,
                             llvm::MDString::get(d_parent->getModule()->getLLContext(), name));
}

llvm::StringRef
FragmentEntryPoint::Output::getTypeName() const {
    if (!d_type_name_md_index) {
        return "";
    }

    return llvm::cast<llvm::MDString>(d_md->getOperand(*d_type_name_md_index).get())->getString();
}

void
FragmentEntryPoint::Output::setTypeName(llvm::StringRef type_name) {
    if (!d_md || !d_type_name_md_index) {
        return;
    }

    d_md->replaceOperandWith(*d_type_name_md_index,
                             llvm::MDString::get(d_parent->getModule()->getLLContext(), type_name));
}

void
ComputeEntryPoint::print(llvm::raw_ostream& os) const {
    os << "compute program ";
    os << getName() << "\n";
}

} // namespace llair
