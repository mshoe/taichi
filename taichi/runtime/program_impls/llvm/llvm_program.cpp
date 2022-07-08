#include "taichi/runtime/program_impls/llvm/llvm_program.h"

#include "llvm/IR/Module.h"

#include "taichi/program/program.h"
#include "taichi/codegen/codegen.h"
#include "taichi/codegen/llvm/struct_llvm.h"
#include "taichi/runtime/llvm/aot_graph_data.h"
#include "taichi/runtime/llvm/llvm_offline_cache.h"
#include "taichi/runtime/cpu/aot_module_builder_impl.h"

#if defined(TI_WITH_CUDA)
#include "taichi/runtime/cuda/aot_module_builder_impl.h"
#include "taichi/codegen/cuda/codegen_cuda.h"
#endif

namespace taichi {
namespace lang {

LlvmProgramImpl::LlvmProgramImpl(CompileConfig &config_,
                                 KernelProfilerBase *profiler)
    : ProgramImpl(config_),
      compilation_workers("compile", config_.num_compile_threads) {
  runtime_exec_ = std::make_unique<LlvmRuntimeExecutor>(config_, profiler);
  cache_data_ = std::make_unique<LlvmOfflineCache>();
}

FunctionType LlvmProgramImpl::compile(Kernel *kernel,
                                      OffloadedStmt *offloaded) {
  auto codegen = KernelCodeGen::create(kernel->arch, kernel, offloaded);
  return codegen->codegen();
}

std::unique_ptr<llvm::Module>
LlvmProgramImpl::clone_struct_compiler_initial_context(
    bool has_multiple_snode_trees,
    TaichiLLVMContext *tlctx) {
  if (has_multiple_snode_trees) {
    return tlctx->clone_struct_module();
  }
  return tlctx->clone_runtime_module();
}

std::unique_ptr<StructCompiler> LlvmProgramImpl::compile_snode_tree_types_impl(
    SNodeTree *tree) {
  auto *const root = tree->root();
  const bool has_multiple_snode_trees = (num_snode_trees_processed_ > 0);
  std::unique_ptr<StructCompiler> struct_compiler{nullptr};
  if (arch_is_cpu(config->arch)) {
    auto host_module = clone_struct_compiler_initial_context(
        has_multiple_snode_trees, runtime_exec_->llvm_context_host_.get());
    struct_compiler = std::make_unique<StructCompilerLLVM>(
        host_arch(), this, std::move(host_module), tree->id());

  } else {
    TI_ASSERT(config->arch == Arch::cuda);
    auto device_module = clone_struct_compiler_initial_context(
        has_multiple_snode_trees, runtime_exec_->llvm_context_device_.get());
    struct_compiler = std::make_unique<StructCompilerLLVM>(
        Arch::cuda, this, std::move(device_module), tree->id());
  }
  struct_compiler->run(*root);
  ++num_snode_trees_processed_;
  return struct_compiler;
}

void LlvmProgramImpl::compile_snode_tree_types(SNodeTree *tree) {
  auto struct_compiler = compile_snode_tree_types_impl(tree);
  int snode_tree_id = tree->id();
  int root_id = tree->root()->id;

  // Add compiled result to Cache
  cache_field(snode_tree_id, root_id, *struct_compiler);
}

void LlvmProgramImpl::materialize_snode_tree(SNodeTree *tree,
                                             uint64 *result_buffer) {
  compile_snode_tree_types(tree);
  int snode_tree_id = tree->id();

  TI_ASSERT(cache_data_->fields.find(snode_tree_id) !=
            cache_data_->fields.end());
  initialize_llvm_runtime_snodes(cache_data_->fields.at(snode_tree_id),
                                 result_buffer);
}

std::unique_ptr<AotModuleBuilder> LlvmProgramImpl::make_aot_module_builder() {
  if (config->arch == Arch::x64 || config->arch == Arch::arm64) {
    return std::make_unique<cpu::AotModuleBuilderImpl>(this);
  }

#if defined(TI_WITH_CUDA)
  if (config->arch == Arch::cuda) {
    return std::make_unique<cuda::AotModuleBuilderImpl>(this);
  }
#endif

  TI_NOT_IMPLEMENTED;
  return nullptr;
}

std::unique_ptr<aot::Kernel> LlvmProgramImpl::make_aot_kernel(Kernel &kernel) {
  auto compiled_fn =
      this->compile(&kernel, nullptr);  // Offloaded used in async mode only

  const std::string &kernel_key = kernel.get_cached_kernel_key();
  TI_ASSERT(cache_data_->kernels.count(kernel_key));
  const LlvmOfflineCache::KernelCacheData &kernel_data =
      cache_data_->kernels[kernel_key];

  LlvmOfflineCache::KernelCacheData compiled_kernel;
  compiled_kernel.kernel_key = kernel.get_name();
  compiled_kernel.owned_module =
      llvm::CloneModule(*kernel_data.owned_module.get());
  compiled_kernel.args = kernel_data.args;
  compiled_kernel.offloaded_task_list = kernel_data.offloaded_task_list;
  return std::make_unique<llvm_aot::KernelImpl>(compiled_fn, kernel.get_name(),
                                                std::move(compiled_kernel));
}

void LlvmProgramImpl::cache_kernel(
    const std::string &kernel_key,
    llvm::Module *module,
    std::vector<LlvmLaunchArgInfo> &&args,
    std::vector<OffloadedTask> &&offloaded_task_list) {
  if (cache_data_->kernels.find(kernel_key) != cache_data_->kernels.end()) {
    return;
  }
  auto &kernel_cache = cache_data_->kernels[kernel_key];
  kernel_cache.created_at = std::time(nullptr);
  kernel_cache.last_used_at = std::time(nullptr);
  kernel_cache.kernel_key = kernel_key;
  kernel_cache.owned_module = llvm::CloneModule(*module);
  kernel_cache.args = std::move(args);
  kernel_cache.offloaded_task_list = std::move(offloaded_task_list);
}

void LlvmProgramImpl::cache_field(int snode_tree_id,
                                  int root_id,
                                  const StructCompiler &struct_compiler) {
  if (cache_data_->fields.find(snode_tree_id) != cache_data_->fields.end()) {
    // [TODO] check and update the Cache, instead of simply return.
    return;
  }

  LlvmOfflineCache::FieldCacheData ret;
  ret.tree_id = snode_tree_id;
  ret.root_id = root_id;
  ret.root_size = struct_compiler.root_size;

  const auto &snodes = struct_compiler.snodes;
  for (size_t i = 0; i < snodes.size(); i++) {
    LlvmOfflineCache::FieldCacheData::SNodeCacheData snode_cache_data;
    snode_cache_data.id = snodes[i]->id;
    snode_cache_data.type = snodes[i]->type;
    snode_cache_data.cell_size_bytes = snodes[i]->cell_size_bytes;
    snode_cache_data.chunk_size = snodes[i]->chunk_size;

    ret.snode_metas.emplace_back(std::move(snode_cache_data));
  }

  cache_data_->fields[snode_tree_id] = std::move(ret);
}

void LlvmProgramImpl::dump_cache_data_to_disk() {
  if (config->offline_cache) {
    auto policy = LlvmOfflineCacheFileWriter::string_to_clean_cache_policy(
        config->offline_cache_cleaning_policy);
    LlvmOfflineCacheFileWriter::clean_cache(
        config->offline_cache_file_path, policy,
        config->offline_cache_max_size_of_files,
        config->offline_cache_cleaning_factor);
    if (!cache_data_->kernels.empty()) {
      LlvmOfflineCacheFileWriter writer{};
      writer.set_data(std::move(cache_data_));

      // Note: For offline-cache, new-metadata should be merged with
      // old-metadata
      writer.dump(config->offline_cache_file_path, LlvmOfflineCache::LL, true);
    }
  }
}

LlvmProgramImpl *get_llvm_program(Program *prog) {
  LlvmProgramImpl *llvm_prog =
      dynamic_cast<LlvmProgramImpl *>(prog->get_program_impl());
  TI_ASSERT(llvm_prog != nullptr);
  return llvm_prog;
}

}  // namespace lang
}  // namespace taichi