#include <torch/csrc/jit/frontend/ir_emitter.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/constant_propagation.h>
#include <torch/csrc/jit/passes/peephole.h>
#include <torch/csrc/jit/runtime/decomposition_registry.h>
#include <torch/csrc/jit/runtime/decomposition_registry_util.h>
#include <torch/csrc/jit/runtime/operator.h>
#include <torch/csrc/jit/serialization/import_source.h>

#include <c10/util/Exception.h>
#include <jit/runtime/graph_executor.h>
#include <torch/csrc/jit/ir/ir.h>
#include <memory>
#include <unordered_map>

namespace torch {
namespace jit {
namespace {
std::mutex lock;

// CompilationUnit that holds all these Functions and keeps them alive.
auto compilation_unit = std::make_shared<CompilationUnit>();
std::unordered_map<const FunctionSchema*, std::shared_ptr<Graph>>
    schema_to_decomposition;

// Holds User-Registered Functions and keeps them alive
std::unordered_map<const FunctionSchema*, std::unique_ptr<Function>>
    user_registered_funcs;

std::unordered_map<const FunctionSchema*, Function*> schema_to_function;

void loadModule(const CompilationUnit& module) {
  const auto& mappings = GetDecompositionMapping().getAllKeysAndValues();
  for (const auto& pair : mappings) {
    const FunctionSchema* schema = &pair.first->schema();
    const std::string& decomposition_function_name = pair.second;

    Function& decomposition_function =
        module.get_function(decomposition_function_name);
    std::shared_ptr<Graph> graph =
        toGraphFunction(decomposition_function).graph();

    schema_to_function[schema] = &decomposition_function;
    schema_to_decomposition[schema] = graph;
  }
}

void loadDecompositionFunctions() {
  std::lock_guard<std::mutex> guard(lock);
  if (schema_to_decomposition.size() != 0) {
    return;
  }

  auto src = std::make_shared<Source>(GetSerializedDecompositions());
  std::stringstream ss;
  std::vector<at::IValue> constantTable;
  auto resolver = std::make_shared<SourceImporterImpl>(
      compilation_unit,
      &constantTable,
      [&](const std::string& name) -> std::shared_ptr<Source> { return src; },
      1);
  compilation_unit->define(
      c10::nullopt, GetSerializedDecompositions(), resolver, nullptr);
  loadModule(*compilation_unit);
}

} // anonymous namespace

void DecomposeOp(Node* n) {
  auto schema = n->maybeSchema();
  if (!schema) {
    return;
  }
  auto decomposition = GetDecomposition(n->schema());
  if (!decomposition) {
    return;
  }
  WithInsertPoint guard(n);
  auto outputs =
      insertGraph(*n->owningGraph(), *decomposition->get(), n->inputs());
  TORCH_INTERNAL_ASSERT(outputs.size() == n->outputs().size());
  for (size_t i : c10::irange(outputs.size())) {
    n->outputs().at(i)->replaceAllUsesWith(outputs[i]);
  }
  n->destroy();
}

void RunDecompositions(Block* block) {
  for (auto it = block->nodes().begin(); it != block->nodes().end();) {
    Node* n = *it;
    it++; // advance iterator bc the current node may be destroyed
    for (Block* b : n->blocks()) {
      RunDecompositions(b);
    }
    DecomposeOp(n);
  }
}

void RunDecompositions(std::shared_ptr<Graph> g) {
  RunDecompositions(g->block());
  for (C10_UNUSED const auto _ : c10::irange(2)) {
    PeepholeOptimize(g, /*disable_shape_peephole*/ true);
    ConstantPropagation(g);
  }
}

c10::optional<std::shared_ptr<Graph>> GetDecomposition(
    const FunctionSchema& schema) {
  loadDecompositionFunctions();
  GRAPH_DEBUG("Trying to find schema: ", schema);
  auto cache_it = schema_to_decomposition.find(&schema);
  if (cache_it != schema_to_decomposition.end()) {
    return cache_it->second;
  }
  GRAPH_DEBUG("Could not find schema: ", schema);

  return c10::nullopt;
}

c10::optional<GraphFunction*> GetDecompositionFunction(
    const FunctionSchema& schema) {
  loadDecompositionFunctions();
  auto cache_it = schema_to_function.find(&schema);
  GRAPH_DEBUG("Trying to find schema: ", schema);
  if (cache_it == schema_to_function.end()) {
    GRAPH_DEBUG("Could not find schema: ", schema);
    return c10::nullopt;
  }
  auto& func = toGraphFunction(*cache_it->second);
  // Simple Executor:
  // To allow decomposition to run on tensor subclasses such as batched tensors,
  // we set decompostion execution to use the simple executor so that
  // optimizations that do not compose with arbitrary subclasses (such as
  // fusion) do not run
  func._set_initial_executor_execution_mode(ExecutorExecutionMode::SIMPLE);
  return &func;
}

// Decomposition registers a Graph so that we can initialize a GraphFunction
// that will run with Simple Executor
void RegisterDecomposition(
    const FunctionSchema& schema,
    std::shared_ptr<Graph> g) {
  loadDecompositionFunctions();
  std::lock_guard<std::mutex> guard(lock);
  std::unique_ptr<GraphFunction> new_func(new GraphFunction(
      schema.name(), g, nullptr, ExecutorExecutionMode::SIMPLE));
  user_registered_funcs.emplace(&schema, std::move(new_func));
  schema_to_function[&schema] = user_registered_funcs[&schema].get();
  schema_to_decomposition[&schema] = g;
}

Function* GetDecompositionExecutor(const char* schema_literal) {
  auto& schema = getOperatorForLiteral(schema_literal)->schema();
  auto maybe_func = GetDecompositionFunction(schema);
  TORCH_INTERNAL_ASSERT(maybe_func);
  return *maybe_func;
}

} // namespace jit
} // namespace torch
