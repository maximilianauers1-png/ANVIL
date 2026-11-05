#include "anvil/document.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

#include "anvil/version.hpp"

namespace anvil {

bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ' ' || c == '.' || c == '-'))
            return false;
    return true;
}

const Object* DocState::find(const std::string& name) const {
    for (const auto& o : objects)
        if (o->name == name) return o.get();
    return nullptr;
}

VariableResolver DocState::resolver() const {
    // Copy the variable table so the resolver stays valid independent of this state.
    auto vtab = std::make_shared<std::map<std::string, Param>>(this->vars);
    auto dims = std::make_shared<std::map<std::string, Dim>>(var_dims);
    auto depth = std::make_shared<int>(0);
    auto self = std::make_shared<VariableResolver>();
    *self = [vtab, dims, depth, weak = std::weak_ptr<VariableResolver>(self)](
                std::string_view name) -> std::optional<RtQuantity> {
        auto it = vtab->find(std::string(name));
        if (it == vtab->end()) return std::nullopt;
        Dim d = dims->count(it->first) ? dims->at(it->first) : dims::kLength;
        if (it->second.expr.empty()) return RtQuantity{it->second.value, d};
        if (++*depth > 32) {
            *depth = 0;
            throw ExprError("variable '" + it->first + "' is defined recursively", 0);
        }
        auto r = weak.lock();
        RtQuantity q = eval_expr(it->second.expr, *r);
        --*depth;
        return q;
    };
    // The returned lambda keeps `self` alive; the inner weak_ptr avoids a cycle.
    return [self](std::string_view n) { return (*self)(n); };
}

ObjectLookup DocState::lookup() const {
    auto table = std::make_shared<std::map<std::string, NodePtr>>();
    for (const auto& o : objects) (*table)[o->name] = o->root;
    return [table](const std::string& n) -> NodePtr {
        auto it = table->find(n);
        return it == table->end() ? nullptr : it->second;
    };
}

EvalSettings DocState::eval_settings(double chord_mm) const {
    EvalSettings es;
    es.chord_mm = chord_mm;
    es.lookup = lookup();
    return es;
}

std::string DocState::unique_name(const std::string& prefix) const {
    for (int i = 1;; ++i) {
        std::string n = prefix + std::to_string(i);
        if (!find(n)) return n;
    }
}

std::string DocState::to_python(const std::string& header_comment) const {
    std::ostringstream o;
    o << "# anvil-format: " << kDocumentFormat << "\n";
    o << "# anvil-version: " << version_string() << "\n";
    if (!header_comment.empty()) o << "# " << header_comment << "\n";
    o << "from anvil import *\n";
    for (const auto& [name, p] : vars) {
        Dim d = var_dims.count(name) ? var_dims.at(name) : dims::kLength;
        o << "var(\"" << name << "\", " << param_to_python(p, d) << ")\n";
    }
    for (const auto& obj : objects) {
        o << "show(" << anvil::to_python(obj->root) << ", name=\"" << obj->name << "\"";
        if (!obj->visible) o << ", visible=False";
        o << ")\n";
    }
    return o.str();
}

Document::Document() : state_(std::make_shared<DocState>()) {}

std::shared_ptr<const DocState> Document::state() const {
    std::lock_guard lk(mu_);
    return state_;
}

std::shared_ptr<const DocState> Document::current() const {
    std::lock_guard lk(mu_);
    return txn_ ? std::shared_ptr<const DocState>(txn_->work) : state_;
}

void Document::begin(std::string label, std::string origin) {
    if (txn_) throw std::logic_error("transaction already open");
    txn_ = Txn{std::move(label), std::move(origin), state_, std::make_shared<DocState>(*state_)};
}

DocState& Document::working() {
    if (!txn_) throw std::logic_error("document mutation outside a transaction");
    return *txn_->work;
}

bool Document::commit() {
    if (!txn_) throw std::logic_error("commit without transaction");
    Txn t = std::move(*txn_);
    txn_.reset();
    const DocState& a = *t.before;
    const DocState& b = *t.work;
    bool changed = a.vars != b.vars || a.objects.size() != b.objects.size();
    for (std::size_t i = 0; !changed && i < a.objects.size(); ++i) changed = a.objects[i] != b.objects[i];
    if (!changed) return false;
    {
        std::lock_guard lk(mu_);
        state_ = t.work;
        undo_.push_back({{t.label, t.origin}, t.before, state_});
        if (undo_.size() > undo_limit_) undo_.erase(undo_.begin());
        redo_.clear();
        ++revision_;
    }
    publish();
    return true;
}

void Document::rollback() { txn_.reset(); }

bool Document::undo() {
    if (txn_) throw std::logic_error("undo during a transaction");
    {
        std::lock_guard lk(mu_);
        if (undo_.empty()) return false;
        redo_.push_back(std::move(undo_.back()));
        undo_.pop_back();
        state_ = redo_.back().before;
        ++revision_;
    }
    publish();
    return true;
}

bool Document::redo() {
    if (txn_) throw std::logic_error("redo during a transaction");
    {
        std::lock_guard lk(mu_);
        if (redo_.empty()) return false;
        undo_.push_back(std::move(redo_.back()));
        redo_.pop_back();
        state_ = undo_.back().after;
        ++revision_;
    }
    publish();
    return true;
}

std::vector<HistoryEntry> Document::history() const {
    std::lock_guard lk(mu_);
    std::vector<HistoryEntry> h;
    for (const auto& s : undo_) h.push_back(s.entry);
    for (auto it = redo_.rbegin(); it != redo_.rend(); ++it) h.push_back(it->entry);
    return h;
}

void Document::publish() {
    auto s = state();
    for (auto& l : listeners_) l(*s);
}

std::string Document::add_object(const std::string& name, NodePtr root, bool visible) {
    DocState& w = working();
    if (!root) throw std::invalid_argument("show(): nothing to show");
    std::string n = name.empty() ? w.unique_name(node_kind_name(root->kind)) : name;
    if (!valid_name(n)) throw std::invalid_argument("invalid object name '" + n + "'");
    if (w.find(n)) {
        // show() with an existing name replaces the geometry (scripts re-run idempotently)
        set_root(n, std::move(root));
        set_visible(n, visible);
        return n;
    }
    w.objects.push_back(std::make_shared<Object>(Object{n, std::move(root), visible}));
    return n;
}

namespace {
std::vector<ObjectPtr>::iterator find_it(DocState& w, const std::string& name) {
    auto it = std::find_if(w.objects.begin(), w.objects.end(), [&](const ObjectPtr& o) { return o->name == name; });
    if (it == w.objects.end()) throw std::invalid_argument("no object named '" + name + "'");
    return it;
}
}  // namespace

void Document::remove_object(const std::string& name) {
    DocState& w = working();
    w.objects.erase(find_it(w, name));
}

void Document::set_root(const std::string& name, NodePtr root) {
    DocState& w = working();
    auto it = find_it(w, name);
    auto o = std::make_shared<Object>(**it);
    o->root = std::move(root);
    *it = o;
}

void Document::set_visible(const std::string& name, bool visible) {
    DocState& w = working();
    auto it = find_it(w, name);
    if ((*it)->visible == visible) return;
    auto o = std::make_shared<Object>(**it);
    o->visible = visible;
    *it = o;
}

void Document::rename(const std::string& from, const std::string& to) {
    DocState& w = working();
    if (!valid_name(to)) throw std::invalid_argument("invalid object name '" + to + "'");
    if (from == to) return;
    if (w.find(to)) throw std::invalid_argument("an object named '" + to + "' already exists");
    auto it = find_it(w, from);
    auto o = std::make_shared<Object>(**it);
    o->name = to;
    *it = o;
}

void Document::set_var(const std::string& name, Param value, Dim dim) {
    DocState& w = working();
    if (!valid_name(name) || name.find_first_of(" .-") != std::string::npos)
        throw std::invalid_argument("invalid variable name '" + name + "' (use letters, digits, _)");
    for (const char* reserved : {"mm", "cm", "m", "um", "in", "inch", "deg", "rad", "pi"})
        if (name == reserved) throw std::invalid_argument("'" + name + "' is a reserved unit/constant name");
    w.vars[name] = std::move(value);
    w.var_dims[name] = dim;
    // Detect cycles / errors early so a broken variable never enters history.
    auto r = w.resolver();
    (void)r(name);
}

void Document::edit_params(const std::string& name, const std::string& path, const std::map<std::string, Param>& p) {
    DocState& w = working();
    auto it = find_it(w, name);
    auto o = std::make_shared<Object>(**it);
    o->root = with_params(o->root, path, p);
    *it = o;
}

void Document::clear() {
    DocState& w = working();
    w.objects.clear();
    w.vars.clear();
    w.var_dims.clear();
}

}  // namespace anvil
