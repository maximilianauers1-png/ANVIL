// Document model: immutable states, transactions, undo/redo (ADR 0004).
//
// Every mutation happens inside a transaction (one Python command = one
// transaction = one undo step). States are persistent (structural sharing),
// so keeping thousands of undo steps is cheap.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "anvil/geometry.hpp"

namespace anvil {

struct Object {
    std::string name;
    NodePtr root;
    bool visible = true;
};
using ObjectPtr = std::shared_ptr<const Object>;

struct DocState {
    std::vector<ObjectPtr> objects;     // draw/order = creation order
    std::map<std::string, Param> vars;  // named variables (Length or dimensionless)
    std::map<std::string, Dim> var_dims;

    const Object* find(const std::string& name) const;
    VariableResolver resolver() const;
    /// Canonical Python script that rebuilds this state (the .anvil format).
    std::string to_python(const std::string& header_comment = {}) const;
    /// Unique default name: prefix + smallest free positive integer.
    std::string unique_name(const std::string& prefix) const;
};

struct HistoryEntry {
    std::string label;   // Python source of the command
    std::string origin;  // "ui", "script", "console", "ai", "replay"
};

class Document {
  public:
    using Listener = std::function<void(const DocState&)>;

    Document();

    // --- transactions ---------------------------------------------------------
    void begin(std::string label, std::string origin);
    /// Commits; pushes an undo step if the state changed. Returns true if it did.
    bool commit();
    void rollback();
    bool in_transaction() const { return txn_.has_value(); }

    // --- mutations (require an open transaction) -------------------------------
    std::string add_object(const std::string& name, NodePtr root, bool visible = true);
    void remove_object(const std::string& name);
    void set_root(const std::string& name, NodePtr root);
    void set_visible(const std::string& name, bool visible);
    void rename(const std::string& from, const std::string& to);
    void set_var(const std::string& name, Param value, Dim dim);
    void edit_params(const std::string& name, const std::string& path, const std::map<std::string, Param>& p);
    void clear();

    // --- undo / redo -----------------------------------------------------------
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    bool undo();
    bool redo();
    /// History: applied steps (oldest first) and undone steps (next redo first).
    std::vector<HistoryEntry> history() const;
    std::size_t history_position() const { return undo_.size(); }
    void set_undo_limit(std::size_t n) { undo_limit_ = n; }

    // --- state -----------------------------------------------------------------
    /// Last committed state (what the viewport shows). Thread-safe.
    std::shared_ptr<const DocState> state() const;
    /// Working state inside an open transaction, else the committed state.
    std::shared_ptr<const DocState> current() const;
    std::uint64_t revision() const { return revision_; }
    void add_listener(Listener l) { listeners_.push_back(std::move(l)); }

  private:
    DocState& working();
    void publish();
    struct Txn {
        std::string label, origin;
        std::shared_ptr<const DocState> before;
        std::shared_ptr<DocState> work;
    };
    struct Step {
        HistoryEntry entry;
        std::shared_ptr<const DocState> before, after;
    };
    mutable std::mutex mu_;
    std::shared_ptr<const DocState> state_;
    std::optional<Txn> txn_;
    std::vector<Step> undo_, redo_;
    std::size_t undo_limit_ = 1000;
    std::uint64_t revision_ = 0;
    std::vector<Listener> listeners_;
};

/// Valid object/variable name: [A-Za-z_][A-Za-z0-9_ .-]*, max 64 chars.
bool valid_name(const std::string& s);

}  // namespace anvil
