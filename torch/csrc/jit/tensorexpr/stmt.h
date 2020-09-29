#pragma once

#include <algorithm>
#include <list>
#include <string>
#include <vector>

#include <torch/csrc/jit/tensorexpr/expr.h>
namespace torch {
namespace jit {
namespace tensorexpr {

class Placeholder;

// The common base between all statement node.
class TORCH_API Stmt : public KernelScopedObject {
 public:
  Stmt() {}
  virtual void accept(IRVisitor* visitor) const = 0;
  virtual Stmt* accept_mutator(IRMutator* mutator) = 0;

  Stmt* get_parent() const {
    return parent_;
  }

  /*
   * Make a deep copy of the given statement.
   *
   * All statements used in children of the statement are cloned. Note that
   * expressions and variables are not deep-copied: it is not necessary since
   * they are immutable.
   */
  static Stmt* clone(Stmt* s);

 protected:
  static void set_parent(Stmt* s, Stmt* new_parent) {
    s->parent_ = new_parent;
  }

 private:
  Stmt* parent_ = nullptr;
};

template <class Op>
class StmtNode : public Stmt {
 public:
  using StmtNodeBase = StmtNode<Op>;
  void accept(IRVisitor* visitor) const override {
    visitor->visit(static_cast<const Op*>(this));
  }
  Stmt* accept_mutator(IRMutator* mutator) override;
  StmtNode() {}
};

template <class Op>
Stmt* StmtNode<Op>::accept_mutator(IRMutator* mutator) {
  StmtNode* this_mutable = const_cast<StmtNode*>(this);
  return mutator->mutate(static_cast<Op*>(this_mutable));
}

// Concrete Stmt classes
class TORCH_API Block : public StmtNode<Block> {
 public:
  static Block* make(const std::vector<Stmt*>& stmts) {
    std::vector<Stmt*> valid_stmts;
    for (size_t i = 0; i < stmts.size(); i++) {
      if (!stmts[i]) {
        continue;
      }
      valid_stmts.push_back(stmts[i]);
    }
    if (valid_stmts.empty()) {
      return nullptr;
    }
    return new Block(valid_stmts);
  }

  int nstmts() const {
    return stmts_.size();
  }
  bool empty() const {
    return stmts_.empty();
  }

  void prepend_stmt(Stmt* s) {
    if (s->get_parent()) {
      throw malformed_input("Block prepend Stmt with existing parent", s);
    }

    stmts_.push_front(s);
    set_parent(s, this);
  }
  void append_stmt(Stmt* s) {
    if (s->get_parent()) {
      throw malformed_input("Block append Stmt with existing parent", s);
    }

    stmts_.push_back(s);
    set_parent(s, this);
  }

  void insert_stmt_before(Stmt* s, const Stmt* before) {
    if (s->get_parent()) {
      throw malformed_input("Block append Stmt with existing parent", s);
    }

    auto pos = std::find(stmts_.begin(), stmts_.end(), before);
    if (pos == stmts_.end()) {
      throw malformed_input(
          "Inserting after statement that is not in block", s);
    }

    stmts_.insert(pos, s);
    set_parent(s, this);
  }

  void insert_stmt_after(Stmt* s, const Stmt* after) {
    if (s->get_parent()) {
      throw malformed_input("Block append Stmt with existing parent", s);
    }

    auto pos = std::find(stmts_.begin(), stmts_.end(), after);
    if (pos == stmts_.end()) {
      throw malformed_input(
          "Inserting after statement that is not in block", s);
    }

    ++pos;

    stmts_.insert(pos, s);
    set_parent(s, this);
  }

  bool replace_stmt(Stmt* old_stmt, Stmt* new_stmt) {
    if (new_stmt->get_parent()) {
      throw malformed_input(
          "Block replace Stmt with existing parent", new_stmt);
    }

    auto pos = std::find(stmts_.begin(), stmts_.end(), old_stmt);
    if (pos == stmts_.end()) {
      return false;
    }
    stmts_.insert(pos, new_stmt);
    stmts_.erase(pos);
    set_parent(old_stmt, nullptr);
    set_parent(new_stmt, this);
    return true;
  }

  bool remove_stmt(Stmt* stmt) {
    auto pos = std::find(stmts_.begin(), stmts_.end(), stmt);
    if (pos == stmts_.end()) {
      return false;
    }

    set_parent(stmt, nullptr);
    stmts_.erase(pos);
    return true;
  }

  std::list<Stmt*> stmts() const {
    return stmts_;
  }

  explicit Block(const std::vector<Stmt*>& stmts) {
    for (Stmt* s : stmts) {
      if (s->get_parent()) {
        throw malformed_input(
            "Block creation has Stmt with existing parent", s);
      }

      stmts_.push_back(s);
      set_parent(s, this);
    }
  }

  typedef std::list<Stmt*>::iterator iterator;
  typedef std::list<Stmt*>::const_iterator const_iterator;

  iterator begin() {
    return stmts_.begin();
  }

  const_iterator begin() const {
    return stmts_.begin();
  }

  iterator end() {
    return stmts_.end();
  }

  const_iterator end() const {
    return stmts_.end();
  }

  Stmt* front() {
    return stmts_.front();
  }

  const Stmt* front() const {
    return stmts_.front();
  }

  Stmt* back() {
    return stmts_.back();
  }

  const Stmt* back() const {
    return stmts_.back();
  }

  void splice(Block::iterator it, Block* other) {
    for (Stmt* s : *other) {
      set_parent(s, this);
    }

    stmts_.splice(it, other->stmts_);
  }

  static const Block* getSharedParent(const Stmt* p1, const Stmt* p2) {
    std::unordered_set<const Block*> enclosing;

    const Stmt* p1_p = p1;
    while (p1_p) {
      if (const Block* b = dynamic_cast<const Block*>(p1_p)) {
        if (b) {
          enclosing.insert(b);
        }
      }
      p1_p = p1_p->get_parent();
    }

    const Stmt* p2_p = p2;
    while (p2_p) {
      if (const Block* b = dynamic_cast<const Block*>(p2_p)) {
        if (enclosing.count(b) != 0) {
          return b;
        }
      }
      p2_p = p2_p->get_parent();
    }

    return nullptr;
  }

 private:
  std::list<Stmt*> stmts_;
};

class TORCH_API Store : public StmtNode<Store> {
 public:
  const Var* base_handle() const {
    return buf_->base_handle();
  }
  std::vector<const Expr*> indices() const {
    return indices_;
  }
  const Expr* flat_index() const {
    TORCH_CHECK(indices_.size() == 1, "Indices haven't been flattened.");
    return indices_[0];
  }
  const Expr* value() const {
    return value_;
  }
  const Expr* mask() const {
    return mask_;
  }
  const Buf* buf() const {
    return buf_;
  }

  static Store* make(
      const BufHandle& buf,
      const std::vector<ExprHandle>& indices,
      const ExprHandle& value,
      const ExprHandle& mask);

  static Store* make(
      const BufHandle& buf,
      const std::vector<ExprHandle>& indices,
      const ExprHandle& value);

  Store(
      const Buf* buf,
      std::vector<const Expr*> indices,
      const Expr* value,
      const Expr* mask);

 private:
  const Buf* buf_;
  std::vector<const Expr*> indices_;
  const Expr* value_;
  const Expr* mask_;
};

// Allocate a buffer of given shapes and dtypes and bind it with the given
// buffer var. The life span is at most through the current program, until it is
// explicitly freed. An unfreed memory is likely considered an error.
class TORCH_API Allocate : public StmtNode<Allocate> {
 public:
  static Allocate* make(
      const VarHandle& buffer_var,
      Dtype dtype,
      const std::vector<ExprHandle>& dims) {
    std::vector<const Expr*> dims_nodes(dims.size());
    for (size_t i = 0; i < dims.size(); i++) {
      dims_nodes[i] = dims[i].node();
    }
    return new Allocate(buffer_var.node(), dtype, dims_nodes);
  }

  const Var* buffer_var() const {
    return buffer_var_;
  }

  Dtype dtype() const {
    return dtype_;
  }

  const std::vector<const Expr*>& dims() const {
    return dims_;
  }

  Allocate(
      const Var* buffer_var,
      Dtype dtype,
      const std::vector<const Expr*>& dims)
      : buffer_var_(buffer_var), dtype_(dtype), dims_(dims) {}

 private:
  const Var* buffer_var_;
  Dtype dtype_;
  std::vector<const Expr*> dims_;
  // TODO: add memory types.
};

// Free the specific buffer. It is an error.
class TORCH_API Free : public StmtNode<Free> {
 public:
  static Free* make(const VarHandle& buffer_var) {
    return new Free(buffer_var.node());
  }

  const Var* buffer_var() const {
    return buffer_var_;
  }

  Free(const Var* buffer_var) : buffer_var_(buffer_var) {}

 private:
  const Var* buffer_var_;
};

class TORCH_API Let : public StmtNode<Let> {
 public:
  static Let* make(const VarHandle& var, const ExprHandle& val) {
    return new Let(var.node(), val.node());
  }

  Let(const Var* var, const Expr* val)
      : dtype_(var->dtype()), var_(var), val_(val) {}

  Dtype dtype() const {
    return dtype_;
  }

  const Var* var() const {
    return var_;
  }

  const Expr* value() const {
    return val_;
  }

 private:
  Dtype dtype_;
  const Var* var_;
  const Expr* val_;
};

class TORCH_API Cond : public StmtNode<Cond> {
 public:
  static Cond* make(
      const ExprHandle& condition,
      Stmt* true_stmt,
      Stmt* false_stmt) {
    return new Cond(condition.node(), true_stmt, false_stmt);
  }

  const Expr* condition() const {
    return condition_;
  }

  Block* true_stmt() const {
    return true_stmt_;
  }

  Block* false_stmt() const {
    return false_stmt_;
  }

  Cond(const Expr* condition, Stmt* true_stmt, Stmt* false_stmt)
      : condition_(condition) {
    if (true_stmt) {
      Block* b = dynamic_cast<Block*>(true_stmt);
      if (!b) {
        b = new Block({true_stmt});
      }
      true_stmt_ = b;
      set_parent(true_stmt_, this);
    }
    if (false_stmt) {
      Block* b = dynamic_cast<Block*>(false_stmt);
      if (!b) {
        b = new Block({false_stmt});
      }
      false_stmt_ = b;
      set_parent(false_stmt_, this);
    }
  }

  Cond* cloneWithNewBodies(Stmt* true_stmt, Stmt* false_stmt) {
    return new Cond(condition_, true_stmt, false_stmt);
  }

  Cond* cloneWithNewBody(Stmt* true_stmt) {
    return new Cond(condition_, true_stmt, nullptr);
  }

 private:
  const Expr* condition_;
  Block* true_stmt_ = nullptr;
  Block* false_stmt_ = nullptr;
};

class TORCH_API LoopOptions {
 public:
  enum {
    IDX_UNSET = -1,
    IDX_X = 0,
    IDX_Y = 1,
    IDX_Z = 2,
    IDX_W = 3,
    IDX_MAX = IDX_W,
  };
  // GPU Block Index
  bool is_gpu_block_index() const {
    return gpu_block_index_ != IDX_UNSET;
  }

  int gpu_block_index() const {
    return gpu_block_index_;
  }

  std::string gpu_block_index_str() const {
    if (!is_gpu_block_index()) {
      throw malformed_input("Has no GPU block index");
    }

    static const char* kBlockIndexNames[] = {
        "blockIdx.x",
        "blockIdx.y",
        "blockIdx.z",
        "blockIdx.w",
    };

    if (gpu_block_index_ < IDX_X || gpu_block_index_ > IDX_MAX) {
      throw malformed_input("invalid GPU block index");
    }

    return kBlockIndexNames[gpu_block_index_];
  }

  void set_gpu_block_index(int index) {
    if (index == IDX_UNSET) {
      gpu_block_index_ = IDX_UNSET;
    }

    if (is_gpu_thread_index()) {
      throw std::runtime_error("Cannot set both gpu block and thread index");
    }
    if (is_gpu_block_index() && gpu_block_index() != index) {
      throw std::runtime_error("Cannot set a previously set block index");
    }
    gpu_block_index_ = index;
  }

  // GPU Thread Index
  bool is_gpu_thread_index() const {
    return gpu_thread_index() != IDX_UNSET;
  }

  int gpu_thread_index() const {
    return gpu_thread_index_;
  }

  std::string gpu_thread_index_str() const {
    if (!is_gpu_thread_index()) {
      throw malformed_input("has no GPU thread index");
    }

    static const char* kThreadIndexNames[] = {
        "threadIdx.x", "threadIdx.y", "threadIdx.z", "threadIdx.w"};

    if (gpu_thread_index_ < IDX_X || gpu_thread_index_ > IDX_MAX) {
      throw malformed_input("invalid GPU thread index");
    }

    return kThreadIndexNames[gpu_thread_index_];
  }

  void set_gpu_thread_index(int index) {
    if (index == IDX_UNSET) {
      gpu_thread_index_ = IDX_UNSET;
    }

    if (is_gpu_block_index()) {
      throw std::runtime_error("Cannot set both gpu thread and block index");
    }
    if (is_gpu_thread_index() && gpu_thread_index() != index) {
      throw std::runtime_error("Cannot set a previously set thread index");
    }
    gpu_thread_index_ = index;
  }

  std::string ToString() const {
    if (is_gpu_block_index()) {
      return gpu_block_index_str();
    } else if (is_gpu_thread_index()) {
      return gpu_thread_index_str();
    }
    return "";
  }

  bool isDefault() const {
    return gpu_block_index_ == IDX_UNSET && gpu_thread_index_ == IDX_UNSET;
  }

  void set_buffer_mapping(
      const std::unordered_map<std::string, const Buf*>& map) {
    map_input_to_tensor_bufs_ = map;
  }

  std::unordered_map<std::string, const Buf*> get_buffer_mapping() const {
    return map_input_to_tensor_bufs_;
  }

 private:
  int gpu_block_index_{IDX_UNSET};
  int gpu_thread_index_{IDX_UNSET};
  std::unordered_map<std::string, const Buf*> map_input_to_tensor_bufs_;
};

class TORCH_API For : public StmtNode<For> {
 public:
  const Var* var() const {
    return var_;
  }
  const Expr* start() const {
    return start_;
  }
  const Expr* stop() const {
    return stop_;
  }
  Block* body() const {
    return body_;
  }
  static For* make(
      const VarHandle& var,
      const ExprHandle& start,
      const ExprHandle& stop,
      Stmt* body) {
    if (!body) {
      return nullptr;
    }
    return new For(var.node(), start.node(), stop.node(), body);
  }
  static For* make(
      const VarHandle& var,
      const ExprHandle& start,
      const ExprHandle& stop,
      Stmt* body,
      const LoopOptions& loop_options) {
    if (!body) {
      return nullptr;
    }
    return new For(var.node(), start.node(), stop.node(), body, loop_options);
  }
  const LoopOptions loop_options() const {
    return loop_options_;
  }

  For(const Var* var, const Expr* start, const Expr* stop, Stmt* body)
      : var_(var), start_(start), stop_(stop) {
    if (!var) {
      throw malformed_input("invalid Var in For loop", var);
    } else if (!start) {
      throw malformed_input("invalid Start in For loop", start);
    } else if (!stop) {
      throw malformed_input("invalid Stop in For loop", stop);
    } else if (!body || body->get_parent()) {
      throw malformed_input("invalid Body in For loop", body);
    }

    Block* b = dynamic_cast<Block*>(body);
    if (!b) {
      b = new Block({body});
    }
    body_ = b;
    set_parent(body_, this);
  }

  For(const Var* var,
      const Expr* start,
      const Expr* stop,
      Stmt* body,
      const LoopOptions& loop_options)
      : var_(var), start_(start), stop_(stop), loop_options_(loop_options) {
    if (!var) {
      throw malformed_input("invalid Var in For loop", var);
    } else if (!start) {
      throw malformed_input("invalid Start in For loop", start);
    } else if (!stop) {
      throw malformed_input("invalid Stop in For loop", stop);
    } else if (!body || body->get_parent()) {
      throw malformed_input("invalid Body in For loop", body);
    }

    Block* b = dynamic_cast<Block*>(body);
    if (!b) {
      b = new Block({body});
    }
    body_ = b;
    set_parent(body_, this);
  }

  void set_gpu_block_index(int block_index) {
    loop_options_.set_gpu_block_index(block_index);
  }

  void set_gpu_thread_index(int thread_index) {
    loop_options_.set_gpu_thread_index(thread_index);
  }

  void set_buffer_map(const std::unordered_map<std::string, const Buf*>& map) {
    loop_options_.set_buffer_mapping(map);
  }

  For* cloneWithNewBody(Stmt* body) const {
    return new For(var_, start_, stop_, body, loop_options_);
  }

 private:
  const Var* var_;
  const Expr* start_;
  const Expr* stop_;
  Block* body_;
  LoopOptions loop_options_;
};

// A backend specific IR Node that implements atomic-add.
// This node could only shows up as an internal with GPU backends.
// TODO: move to this an internal IR.
// TODO: make IR nodes extensible.
class TORCH_API AtomicAdd : public StmtNode<AtomicAdd> {
 public:
  AtomicAdd(
      const Buf* buf,
      const std::vector<const Expr*>& indices,
      const Expr* value)
      : buf_(buf), indices_(indices), value_(value) {}

  const Var* base_handle() const {
    return buf_->base_handle();
  }

  const Buf* buf() const {
    return buf_;
  }

  const Expr* flat_index() const {
    TORCH_CHECK(indices_.size() == 1, "Indices haven't been flattened.");
    return indices_[0];
  }

  const Expr* value() const {
    return value_;
  }

  const std::vector<const Expr*>& indices() const {
    return indices_;
  }

 private:
  const Buf* buf_;
  std::vector<const Expr*> indices_;
  const Expr* value_;
};

class TORCH_API SyncThreads : public StmtNode<SyncThreads> {
 public:
  SyncThreads() {}
};

} // namespace tensorexpr
} // namespace jit
} // namespace torch
