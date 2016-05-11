#include "Module.h"

#include <array>
#include <fstream>

#include "CodeGen_C.h"
#include "Debug.h"
#include "LLVM_Headers.h"
#include "LLVM_Output.h"
#include "LLVM_Runtime_Linker.h"
#include "IROperator.h"
#include "Outputs.h"
#include "StmtToHtml.h"

namespace Halide {

namespace Internal {

struct ModuleContents {
    mutable RefCount ref_count;
    std::string name;
    Target target;
    std::vector<Buffer> buffers;
    std::vector<Internal::LoweredFunc> functions;
};

template<>
EXPORT RefCount &ref_count<ModuleContents>(const ModuleContents *f) {
    return f->ref_count;
}

template<>
EXPORT void destroy<ModuleContents>(const ModuleContents *f) {
    delete f;
}

}  // namespace Internal

using namespace Halide::Internal;

Module::Module(const std::string &name, const Target &target) :
    contents(new Internal::ModuleContents) {
    contents->name = name;
    contents->target = target;
}

const Target &Module::target() const {
    return contents->target;
}

const std::string &Module::name() const {
    return contents->name;
}

const std::vector<Buffer> &Module::buffers() const {
    return contents->buffers;
}

const std::vector<Internal::LoweredFunc> &Module::functions() const {
    return contents->functions;
}

void Module::append(const Buffer &buffer) {
    contents->buffers.push_back(buffer);
}

void Module::appendf(const Internal::LoweredFunc &function) {
    contents->functions.push_back(function);
}

Module link_modules(const std::string &name, const std::vector<Module> &modules) {
    Module output(name, modules.front().target());

    for (size_t i = 0; i < modules.size(); i++) {
        const Module &input = modules[i];

        if (output.target() != input.target()) {
            user_error << "Mismatched targets in modules to link ("
                       << output.name() << ", " << output.target().to_string()
                       << "), ("
                       << input.name() << ", " << input.target().to_string() << ")\n";
        }

        // TODO(dsharlet): Check for naming collisions, maybe rename
        // internal linkage declarations in the case of collision.
        for (const auto &b : input.buffers()) {
            output.append(b);
        }
        for (const auto &f : input.functions()) {
            output.appendf(f);
        }
    }

    return output;
}

void Module::compile(const Outputs &output_files) const {
    if (!output_files.object_name.empty() || !output_files.assembly_name.empty() ||
        !output_files.bitcode_name.empty() || !output_files.llvm_assembly_name.empty()) {
        llvm::LLVMContext context;
        std::unique_ptr<llvm::Module> llvm_module(compile_module_to_llvm_module(*this, context));

        if (!output_files.object_name.empty()) {
            if (target().arch == Target::PNaCl) {
                compile_llvm_module_to_llvm_bitcode(*llvm_module, output_files.object_name);
            } else {
                compile_llvm_module_to_object(*llvm_module, output_files.object_name);
            }
        }
        if (!output_files.assembly_name.empty()) {
            if (target().arch == Target::PNaCl) {
                compile_llvm_module_to_llvm_assembly(*llvm_module, output_files.assembly_name);
            } else {
                compile_llvm_module_to_assembly(*llvm_module, output_files.assembly_name);
            }
        }
        if (!output_files.bitcode_name.empty()) {
            compile_llvm_module_to_llvm_bitcode(*llvm_module, output_files.bitcode_name);
        }
        if (!output_files.llvm_assembly_name.empty()) {
            compile_llvm_module_to_llvm_assembly(*llvm_module, output_files.llvm_assembly_name);
        }
    }
    if (!output_files.c_header_name.empty()) {
        std::ofstream file(output_files.c_header_name.c_str());
        Internal::CodeGen_C cg(file,
                               target().has_feature(Target::CPlusPlusMangling) ?
                               Internal::CodeGen_C::CPlusPlusHeader : Internal::CodeGen_C::CHeader,
                               output_files.c_header_name);
        cg.compile(*this);
    }
    if (!output_files.c_source_name.empty()) {
        std::ofstream file(output_files.c_source_name.c_str());
        Internal::CodeGen_C cg(file,
                               target().has_feature(Target::CPlusPlusMangling) ?
                               Internal::CodeGen_C::CPlusPlusImplementation : Internal::CodeGen_C::CImplementation);
        cg.compile(*this);
    }
    if (!output_files.stmt_name.empty()) {
        std::ofstream file(output_files.stmt_name.c_str());
        file << *this;
    }
    if (!output_files.stmt_html_name.empty()) {
        Internal::print_to_html(output_files.stmt_html_name, *this);
    }
}

void compile_standalone_runtime(std::string object_filename, Target t) {
    Module empty("standalone_runtime", t.without_feature(Target::NoRuntime).without_feature(Target::JIT));
    empty.compile(Outputs().object(object_filename));
}


Module build_multitarget_module(const std::string &fn_name, 
                                const std::vector<Target> &targets, 
                                std::function<Module(const std::string &, const Target &)> module_producer) {
    user_assert(!fn_name.empty()) << "Function name must be specified.\n";
    user_assert(!targets.empty()) << "Must specify at least one target.\n";

    // The final target in the list is considered "baseline", and is used
    // for (e.g.) the runtime and shared code. It is often just os-arch-bits
    // with no other features, though this is *not* a requirement.
    const Target &base_target = targets.back();
    user_assert(!base_target.has_feature(Target::JIT)) << "JIT not allowed for compile_to_multitarget_object.\n";
    if (targets.size() == 1) {
        return module_producer(fn_name, base_target);
    }

    std::vector<Module> modules;
    std::vector<Expr> wrapper_args;
    for (const Target &target : targets) {
        if (target.os != base_target.os ||
            target.arch != base_target.arch ||
            target.bits != base_target.bits) {
            user_error << "All Targets must have matching arch-bits-os for compile_to_multitarget_object.\n";
            break;
        }
        // Some features must match across all targets.
        static const std::array<Target::Feature, 5> must_match_features = {{
            Target::CPlusPlusMangling,
            Target::JIT,
            Target::NoRuntime,
            Target::RegisterMetadata,
            Target::UserContext,
        }};
        for (auto f : must_match_features) {
            if (target.has_feature(f) != base_target.has_feature(f)) {
                user_error << "All Targets must have feature " << f << " set identically for compile_to_multitarget_object.\n";
                break;
            }
        }
        auto sub_fn_name = fn_name + "_" + replace_all(target.to_string(), "-", "_");
        auto sub_module = module_producer(sub_fn_name, target.with_feature(Target::NoRuntime));
        modules.push_back(sub_module);

        static_assert(sizeof(uint64_t)*8 >= Target::FeatureEnd, "Features will not fit in uint64_t");
        uint64_t feature_bits = 0;
        for (int i = 0; i < Target::FeatureEnd; ++i) {
            if (target.has_feature(static_cast<Target::Feature>(i))) {
                feature_bits |= static_cast<uint64_t>(1) << i;
            }
        }

        Expr can_use = target != base_target ?
            Call::make(Int(32), "halide_can_use_target_features", {UIntImm::make(UInt(64), feature_bits)}, Call::Extern) :
            IntImm::make(Int(32), 1);

        wrapper_args.push_back(can_use != 0);
        wrapper_args.push_back(sub_fn_name);
    }

    const Module &base_module = modules.back();
    const LoweredFunc &base_public_func = base_module.functions().back();
    const std::vector<Argument> public_args = base_public_func.args;

    // If we haven't specified "no runtime", build a runtime with the base target
    // and add that to the result.
    if (!base_target.has_feature(Target::NoRuntime)) {
        Module empty(fn_name + "_runtime", base_target.without_feature(Target::NoRuntime));
        modules.push_back(empty);
    }

    Expr indirect_result = Call::make(Int(32), Call::call_cached_indirect_function, wrapper_args, Call::Intrinsic);

    std::string private_result_name = unique_name(fn_name + "_result");
    Expr private_result_var = Variable::make(Int(32), private_result_name);
    Stmt wrapper_body = AssertStmt::make(private_result_var == 0, private_result_var);
    wrapper_body = LetStmt::make(private_result_name, indirect_result, wrapper_body);

    // We don't use link_modules() here because it sets the Module target
    // to the first item in the list; we specifically want base_target.
    // (Also, it checks for incompatible Targets, which we've already checked
    // to our satisfaction.)
    Module multi_module(fn_name, base_target);
    for (const Module &input : modules) {
        for (const auto &b : input.buffers()) {
            multi_module.append(b);
        }
        for (const auto &f : input.functions()) {
            multi_module.appendf(f);
        }
    }
    // wrapper_body must come last
    multi_module.appendf(LoweredFunc(fn_name, public_args, wrapper_body, LoweredFunc::External));
    return multi_module;
}

}
