// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/new-escape-analysis.h"

#include "src/bootstrapper.h"
#include "src/compiler/linkage.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/operator-properties.h"
#include "src/compiler/simplified-operator.h"
#include "src/objects-inl.h"

namespace v8 {
namespace internal {
namespace compiler {

template <class T>
class Sidetable {
 public:
  explicit Sidetable(Zone* zone) : map_(zone) {}
  T& operator[](const Node* node) {
    NodeId id = node->id();
    if (id >= map_.size()) {
      map_.resize(id + 1);
    }
    return map_[id];
  }

 private:
  ZoneVector<T> map_;
};

template <class T>
class SparseSidetable {
 public:
  explicit SparseSidetable(Zone* zone, T def_value = T())
      : def_value_(std::move(def_value)), map_(zone) {}
  T& operator[](const Node* node) {
    return map_.insert(std::make_pair(node->id(), def_value_)).first->second;
  }

 private:
  T def_value_;
  ZoneUnorderedMap<NodeId, T> map_;
};

// Keeps track of the changes to the current node during reduction.
// Encapsulates the current state of the IR graph and the reducer state like
// side-tables. All access to the IR and the reducer state should happen through
// a ReduceScope to ensure that changes and dependencies are tracked and all
// necessary node revisitations happen.
class ReduceScope {
 public:
  typedef EffectGraphReducer::Reduction Reduction;
  explicit ReduceScope(Node* node, Reduction* reduction)
      : current_node_(node), reduction_(reduction) {}

 protected:
  Node* current_node() const { return current_node_; }
  Reduction* reduction() { return reduction_; }

 private:
  Node* current_node_;
  Reduction* reduction_;
};

// A VariableTracker object keeps track of the values of variables at all points
// of the effect chain and introduces new phi nodes when necessary.
// Initially and by default, variables are mapped to nullptr, which means that
// the variable allocation point does not dominate the current point on the
// effect chain. We map variables that represent uninitialized memory to the
// Dead node to ensure it is not read.
// Unmapped values are impossible by construction, it is indistinguishable if a
// PersistentMap does not contain an element or maps it to the default element.
class VariableTracker {
 private:
  // The state of all variables at one point in the effect chain.
  class State {
    typedef PersistentMap<Variable, Node*> Map;

   public:
    explicit State(Zone* zone) : map_(zone) {}
    Node* Get(Variable var) const {
      CHECK(var != Variable::Invalid());
      return map_.Get(var);
    }
    void Set(Variable var, Node* node) {
      CHECK(var != Variable::Invalid());
      return map_.Set(var, node);
    }
    Map::iterator begin() const { return map_.begin(); }
    Map::iterator end() const { return map_.end(); }
    bool operator!=(const State& other) const { return map_ != other.map_; }

   private:
    Map map_;
  };

 public:
  VariableTracker(JSGraph* graph, EffectGraphReducer* reducer, Zone* zone);
  Variable NewVariable() { return Variable(next_variable_++); }
  Node* Get(Variable var, Node* effect) { return table_[effect].Get(var); }
  Zone* zone() { return zone_; }

  class Scope : public ReduceScope {
   public:
    Scope(VariableTracker* tracker, Node* node, Reduction* reduction);
    ~Scope();
    Node* Get(Variable var) { return current_state_.Get(var); }
    void Set(Variable var, Node* node) { current_state_.Set(var, node); }

   private:
    VariableTracker* states_;
    State current_state_;
  };

 private:
  State MergeInputs(Node* effect_phi);
  Zone* zone_;
  JSGraph* graph_;
  SparseSidetable<State> table_;
  ZoneVector<Node*> buffer_;
  EffectGraphReducer* reducer_;
  int next_variable_ = 0;

  DISALLOW_COPY_AND_ASSIGN(VariableTracker);
};

// Encapsulates the current state of the escape analysis reducer to preserve
// invariants regarding changes and re-visitation.
class EscapeAnalysisTracker : public ZoneObject {
 public:
  EscapeAnalysisTracker(JSGraph* jsgraph, EffectGraphReducer* reducer,
                        Zone* zone)
      : virtual_objects_(zone),
        replacements_(zone),
        variable_states_(jsgraph, reducer, zone),
        jsgraph_(jsgraph),
        zone_(zone) {}

  class Scope : public VariableTracker::Scope {
   public:
    Scope(EffectGraphReducer* reducer, EscapeAnalysisTracker* tracker,
          Node* node, Reduction* reduction)
        : VariableTracker::Scope(&tracker->variable_states_, node, reduction),
          tracker_(tracker),
          reducer_(reducer) {}
    const VirtualObject* GetVirtualObject(Node* node) {
      VirtualObject* vobject = tracker_->virtual_objects_[node];
      if (vobject) vobject->AddDependency(current_node());
      return vobject;
    }
    // Create or retrieve a virtual object for the current node.
    const VirtualObject* InitVirtualObject(int size) {
      DCHECK(current_node()->opcode() == IrOpcode::kAllocate);
      VirtualObject* vobject = tracker_->virtual_objects_[current_node()];
      if (vobject) {
        CHECK(vobject->size() == size);
      } else {
        vobject = tracker_->NewVirtualObject(size);
      }
      if (vobject) vobject->AddDependency(current_node());
      vobject_ = vobject;
      return vobject;
    }

    void SetVirtualObject(Node* object) {
      vobject_ = tracker_->virtual_objects_[object];
    }

    void SetEscaped(Node* node) {
      if (VirtualObject* object = tracker_->virtual_objects_[node]) {
        if (object->HasEscaped()) return;
        TRACE("Setting %s#%d to escaped because of use by %s#%d\n",
              node->op()->mnemonic(), node->id(),
              current_node()->op()->mnemonic(), current_node()->id());
        object->SetEscaped();
        object->RevisitDependants(reducer_);
      }
    }
    // The inputs of the current node have to be accessed through the scope to
    // ensure that they respect the node replacements.
    Node* ValueInput(int i) {
      return tracker_->ResolveReplacement(
          NodeProperties::GetValueInput(current_node(), i));
    }
    Node* ContextInput() {
      return tracker_->ResolveReplacement(
          NodeProperties::GetContextInput(current_node()));
    }

    void SetReplacement(Node* replacement) {
      replacement_ = replacement;
      vobject_ =
          replacement ? tracker_->virtual_objects_[replacement] : nullptr;
      TRACE("Set %s#%d as replacement.\n", replacement->op()->mnemonic(),
            replacement->id());
    }

    void MarkForDeletion() { SetReplacement(tracker_->jsgraph_->Dead()); }

    ~Scope() {
      if (replacement_ != tracker_->replacements_[current_node()] ||
          vobject_ != tracker_->virtual_objects_[current_node()]) {
        reduction()->set_value_changed();
      }
      tracker_->replacements_[current_node()] = replacement_;
      tracker_->virtual_objects_[current_node()] = vobject_;
    }

   private:
    EscapeAnalysisTracker* tracker_;
    EffectGraphReducer* reducer_;
    VirtualObject* vobject_ = nullptr;
    Node* replacement_ = nullptr;
  };

  Node* GetReplacementOf(Node* node) { return replacements_[node]; }
  Node* ResolveReplacement(Node* node) {
    if (Node* replacement = GetReplacementOf(node)) {
      // Replacements cannot have replacements. This is important to ensure
      // re-visitation: If a replacement is replaced, then all nodes accessing
      // the replacement have to be updated.
      DCHECK_NULL(GetReplacementOf(replacement));
      return replacement;
    }
    return node;
  }

 private:
  friend class EscapeAnalysisResult;
  static const size_t kMaxTrackedObjects = 100;

  VirtualObject* NewVirtualObject(int size) {
    if (next_object_id_ >= kMaxTrackedObjects) return nullptr;
    return new (zone_)
        VirtualObject(&variable_states_, next_object_id_++, size);
  }

  SparseSidetable<VirtualObject*> virtual_objects_;
  Sidetable<Node*> replacements_;
  VariableTracker variable_states_;
  VirtualObject::Id next_object_id_ = 0;
  JSGraph* const jsgraph_;
  Zone* const zone_;

  DISALLOW_COPY_AND_ASSIGN(EscapeAnalysisTracker);
};

EffectGraphReducer::EffectGraphReducer(
    Graph* graph, std::function<void(Node*, Reduction*)> reduce, Zone* zone)
    : graph_(graph),
      state_(graph, kNumStates),
      revisit_(zone),
      stack_(zone),
      reduce_(reduce) {}

void EffectGraphReducer::ReduceFrom(Node* node) {
  // Perform DFS and eagerly trigger revisitation as soon as possible.
  // A stack element {node, i} indicates that input i of node should be visited
  // next.
  DCHECK(stack_.empty());
  stack_.push({node, 0});
  while (!stack_.empty()) {
    Node* current = stack_.top().node;
    int& input_index = stack_.top().input_index;
    if (input_index < current->InputCount()) {
      Node* input = current->InputAt(input_index);
      input_index++;
      switch (state_.Get(input)) {
        case State::kVisited:
          // The input is already reduced.
          break;
        case State::kOnStack:
          // The input is on the DFS stack right now, so it will be revisited
          // later anyway.
          break;
        case State::kUnvisited:
        case State::kRevisit: {
          state_.Set(input, State::kOnStack);
          stack_.push({input, 0});
          break;
        }
      }
    } else {
      stack_.pop();
      Reduction reduction;
      reduce_(current, &reduction);
      for (Edge edge : current->use_edges()) {
        // Mark uses for revisitation.
        Node* use = edge.from();
        if (NodeProperties::IsEffectEdge(edge)) {
          if (reduction.effect_changed()) Revisit(use);
        } else {
          if (reduction.value_changed()) Revisit(use);
        }
      }
      state_.Set(current, State::kVisited);
      // Process the revisitation buffer immediately. This improves performance
      // of escape analysis. Using a stack for {revisit_} reverses the order in
      // which the revisitation happens. This also seems to improve performance.
      while (!revisit_.empty()) {
        Node* revisit = revisit_.top();
        if (state_.Get(revisit) == State::kRevisit) {
          state_.Set(revisit, State::kOnStack);
          stack_.push({revisit, 0});
        }
        revisit_.pop();
      }
    }
  }
}

void EffectGraphReducer::Revisit(Node* node) {
  if (state_.Get(node) == State::kVisited) {
    TRACE("  Queueing for revisit: %s#%d\n", node->op()->mnemonic(),
          node->id());
    state_.Set(node, State::kRevisit);
    revisit_.push(node);
  }
}

VariableTracker::VariableTracker(JSGraph* graph, EffectGraphReducer* reducer,
                                 Zone* zone)
    : zone_(zone),
      graph_(graph),
      table_(zone, State(zone)),
      buffer_(zone),
      reducer_(reducer) {}

VariableTracker::Scope::Scope(VariableTracker* states, Node* node,
                              Reduction* reduction)
    : ReduceScope(node, reduction),
      states_(states),
      current_state_(states->zone_) {
  switch (node->opcode()) {
    case IrOpcode::kEffectPhi:
      current_state_ = states_->MergeInputs(node);
      break;
    default:
      int effect_inputs = node->op()->EffectInputCount();
      if (effect_inputs == 1) {
        current_state_ =
            states_->table_[NodeProperties::GetEffectInput(node, 0)];
      } else {
        DCHECK_EQ(0, effect_inputs);
      }
  }
}

VariableTracker::Scope::~Scope() {
  if (!reduction()->effect_changed() &&
      states_->table_[current_node()] != current_state_) {
    reduction()->set_effect_changed();
  }
  states_->table_[current_node()] = current_state_;
}

VariableTracker::State VariableTracker::MergeInputs(Node* effect_phi) {
  // A variable that is mapped to [nullptr] was not assigned a value on every
  // execution path to the current effect phi. Relying on the invariant that
  // every variable is initialized (at least with a sentinel like the Dead
  // node), this means that the variable initialization does not dominate the
  // current point. So for loop effect phis, we can keep nullptr for a variable
  // as long as the first input of the loop has nullptr for this variable. For
  // non-loop effect phis, we can even keep it nullptr as long as any input has
  // nullptr.
  DCHECK(effect_phi->opcode() == IrOpcode::kEffectPhi);
  int arity = effect_phi->op()->EffectInputCount();
  Node* control = NodeProperties::GetControlInput(effect_phi, 0);
  TRACE("control: %s#%d\n", control->op()->mnemonic(), control->id());
  bool is_loop = control->opcode() == IrOpcode::kLoop;
  buffer_.reserve(arity + 1);

  State first_input = table_[NodeProperties::GetEffectInput(effect_phi, 0)];
  State result = first_input;
  for (std::pair<Variable, Node*> var_value : first_input) {
    if (Node* value = var_value.second) {
      Variable var = var_value.first;
      TRACE("var %i:\n", var.id_);
      buffer_.clear();
      buffer_.push_back(value);
      bool identical_inputs = true;
      int num_defined_inputs = 1;
      TRACE("  input 0: %s#%d\n", value->op()->mnemonic(), value->id());
      for (int i = 1; i < arity; ++i) {
        Node* next_value =
            table_[NodeProperties::GetEffectInput(effect_phi, i)].Get(var);
        if (next_value != value) identical_inputs = false;
        if (next_value != nullptr) {
          num_defined_inputs++;
          TRACE("  input %i: %s#%d\n", i, next_value->op()->mnemonic(),
                next_value->id());
        } else {
          TRACE("  input %i: nullptr\n", i);
        }
        buffer_.push_back(next_value);
      }

      Node* old_value = table_[effect_phi].Get(var);
      if (old_value) {
        TRACE("  old: %s#%d\n", old_value->op()->mnemonic(), old_value->id());
      } else {
        TRACE("  old: nullptr\n");
      }
      // Reuse a previously created phi node if possible.
      if (old_value && old_value->opcode() == IrOpcode::kPhi &&
          NodeProperties::GetControlInput(old_value, 0) == control) {
        // Since a phi node can never dominate its control node,
        // [old_value] cannot originate from the inputs. Thus [old_value]
        // must have been created by a previous reduction of this [effect_phi].
        for (int i = 0; i < arity; ++i) {
          NodeProperties::ReplaceValueInput(
              old_value, buffer_[i] ? buffer_[i] : graph_->Dead(), i);
          // This change cannot affect the rest of the reducer, so there is no
          // need to trigger additional revisitations.
        }
        result.Set(var, old_value);
      } else {
        if (num_defined_inputs == 1 && is_loop) {
          // For loop effect phis, the variable initialization dominates iff it
          // dominates the first input.
          DCHECK_EQ(2, arity);
          DCHECK_EQ(value, buffer_[0]);
          result.Set(var, value);
        } else if (num_defined_inputs < arity) {
          // If the variable is undefined on some input of this non-loop effect
          // phi, then its initialization does not dominate this point.
          result.Set(var, nullptr);
        } else {
          DCHECK_EQ(num_defined_inputs, arity);
          // We only create a phi if the values are different.
          if (identical_inputs) {
            result.Set(var, value);
          } else {
            TRACE("Creating new phi\n");
            buffer_.push_back(control);
            Node* phi = graph_->graph()->NewNode(
                graph_->common()->Phi(MachineRepresentation::kTagged, arity),
                arity + 1, &buffer_.front());
            // TODO(tebbi): Computing precise types here is tricky, because of
            // the necessary revisitations. If we really need this, we should
            // probably do it afterwards.
            NodeProperties::SetType(phi, Type::Any());
            reducer_->AddRoot(phi);
            result.Set(var, phi);
          }
        }
      }
#ifdef DEBUG
      if (Node* result_node = result.Get(var)) {
        TRACE("  result: %s#%d\n", result_node->op()->mnemonic(),
              result_node->id());
      } else {
        TRACE("  result: nullptr\n");
      }
#endif
    }
  }
  return result;
}

namespace {

int OffsetOfFieldAccess(const Operator* op) {
  DCHECK(op->opcode() == IrOpcode::kLoadField ||
         op->opcode() == IrOpcode::kStoreField);
  FieldAccess access = FieldAccessOf(op);
  return access.offset;
}

Maybe<int> OffsetOfElementsAccess(const Operator* op, Node* index_node) {
  DCHECK(op->opcode() == IrOpcode::kLoadElement ||
         op->opcode() == IrOpcode::kStoreElement);
  Type* index_type = NodeProperties::GetType(index_node);
  if (!index_type->Is(Type::Number())) return Nothing<int>();
  double max = index_type->Max();
  double min = index_type->Min();
  int index = static_cast<int>(min);
  if (!(index == min && index == max)) return Nothing<int>();
  ElementAccess access = ElementAccessOf(op);
  DCHECK_GE(ElementSizeLog2Of(access.machine_type.representation()),
            kPointerSizeLog2);
  return Just(access.header_size + (index << ElementSizeLog2Of(
                                        access.machine_type.representation())));
}

void ReduceNode(const Operator* op, EscapeAnalysisTracker::Scope* current,
                JSGraph* jsgraph) {
  switch (op->opcode()) {
    case IrOpcode::kAllocate: {
      NumberMatcher size(current->ValueInput(0));
      if (!size.HasValue()) break;
      int size_int = static_cast<int>(size.Value());
      if (size_int != size.Value()) break;
      if (const VirtualObject* vobject = current->InitVirtualObject(size_int)) {
        // Initialize with dead nodes as a sentinel for uninitialized memory.
        for (Variable field : *vobject) {
          current->Set(field, jsgraph->Dead());
        }
      }
      break;
    }
    case IrOpcode::kFinishRegion:
      current->SetVirtualObject(current->ValueInput(0));
      break;
    case IrOpcode::kStoreField: {
      Node* object = current->ValueInput(0);
      Node* value = current->ValueInput(1);
      const VirtualObject* vobject = current->GetVirtualObject(object);
      Variable var;
      if (vobject && !vobject->HasEscaped() &&
          vobject->FieldAt(OffsetOfFieldAccess(op)).To(&var)) {
        current->Set(var, value);
        current->MarkForDeletion();
      } else {
        current->SetEscaped(object);
        current->SetEscaped(value);
      }
      break;
    }
    case IrOpcode::kStoreElement: {
      Node* object = current->ValueInput(0);
      Node* index = current->ValueInput(1);
      Node* value = current->ValueInput(2);
      const VirtualObject* vobject = current->GetVirtualObject(object);
      int offset;
      Variable var;
      if (vobject && !vobject->HasEscaped() &&
          OffsetOfElementsAccess(op, index).To(&offset) &&
          vobject->FieldAt(offset).To(&var)) {
        current->Set(var, value);
        current->MarkForDeletion();
      } else {
        current->SetEscaped(value);
        current->SetEscaped(object);
      }
      break;
    }
    case IrOpcode::kLoadField: {
      Node* object = current->ValueInput(0);
      const VirtualObject* vobject = current->GetVirtualObject(object);
      Variable var;
      if (vobject && !vobject->HasEscaped() &&
          vobject->FieldAt(OffsetOfFieldAccess(op)).To(&var)) {
        current->SetReplacement(current->Get(var));
      } else {
        // TODO(tebbi): At the moment, we mark objects as escaping if there
        // is a load from an invalid location to avoid dead nodes. This is a
        // workaround that should be removed once we can handle dead nodes
        // everywhere.
        current->SetEscaped(object);
      }
      break;
    }
    case IrOpcode::kLoadElement: {
      Node* object = current->ValueInput(0);
      Node* index = current->ValueInput(1);
      const VirtualObject* vobject = current->GetVirtualObject(object);
      int offset;
      Variable var;
      if (vobject && !vobject->HasEscaped() &&
          OffsetOfElementsAccess(op, index).To(&offset) &&
          vobject->FieldAt(offset).To(&var)) {
        current->SetReplacement(current->Get(var));
      } else {
        current->SetEscaped(object);
      }
      break;
    }
    case IrOpcode::kTypeGuard: {
      // The type-guard is re-introduced in the final reducer if the types
      // don't match.
      current->SetReplacement(current->ValueInput(0));
      break;
    }
    case IrOpcode::kReferenceEqual: {
      Node* left = current->ValueInput(0);
      Node* right = current->ValueInput(1);
      const VirtualObject* left_object = current->GetVirtualObject(left);
      const VirtualObject* right_object = current->GetVirtualObject(right);
      if (left_object && !left_object->HasEscaped()) {
        if (right_object && !right_object->HasEscaped() &&
            left_object->id() == right_object->id()) {
          current->SetReplacement(jsgraph->TrueConstant());
        } else {
          current->SetReplacement(jsgraph->FalseConstant());
        }
      } else if (right_object && !right_object->HasEscaped()) {
        current->SetReplacement(jsgraph->FalseConstant());
      }
      break;
    }
    case IrOpcode::kCheckMaps: {
      CheckMapsParameters params = CheckMapsParametersOf(op);
      Node* checked = current->ValueInput(0);
      const VirtualObject* vobject = current->GetVirtualObject(checked);
      Variable map_field;
      if (vobject && !vobject->HasEscaped() &&
          vobject->FieldAt(HeapObject::kMapOffset).To(&map_field)) {
        Node* map = current->Get(map_field);
        if (map) {
          Type* const map_type = NodeProperties::GetType(map);
          if (map_type->IsHeapConstant() &&
              params.maps().contains(ZoneHandleSet<Map>(bit_cast<Handle<Map>>(
                  map_type->AsHeapConstant()->Value())))) {
            current->MarkForDeletion();
            break;
          }
        }
      }
      current->SetEscaped(checked);
      break;
    }
    case IrOpcode::kCheckHeapObject: {
      Node* checked = current->ValueInput(0);
      switch (checked->opcode()) {
        case IrOpcode::kAllocate:
        case IrOpcode::kFinishRegion:
        case IrOpcode::kHeapConstant:
          current->SetReplacement(checked);
          break;
        default:
          current->SetEscaped(checked);
          break;
      }
      break;
    }
    case IrOpcode::kStateValues:
    case IrOpcode::kFrameState:
      // These uses are always safe.
      break;
    default: {
      // For unknown nodes, treat all value inputs as escaping.
      int value_input_count = op->ValueInputCount();
      for (int i = 0; i < value_input_count; ++i) {
        Node* input = current->ValueInput(i);
        current->SetEscaped(input);
      }
      if (OperatorProperties::HasContextInput(op)) {
        current->SetEscaped(current->ContextInput());
      }
      break;
    }
  }  // namespace
}  // namespace

}  // namespace

void NewEscapeAnalysis::Reduce(Node* node, Reduction* reduction) {
  const Operator* op = node->op();
  TRACE("Reducing %s#%d\n", op->mnemonic(), node->id());

  EscapeAnalysisTracker::Scope current(this, tracker_, node, reduction);
  ReduceNode(op, &current, jsgraph());
}

NewEscapeAnalysis::NewEscapeAnalysis(JSGraph* jsgraph, Zone* zone)
    : EffectGraphReducer(
          jsgraph->graph(),
          [this](Node* node, Reduction* reduction) { Reduce(node, reduction); },
          zone),
      tracker_(new (zone) EscapeAnalysisTracker(jsgraph, this, zone)),
      jsgraph_(jsgraph) {}

Node* EscapeAnalysisResult::GetReplacementOf(Node* node) {
  return tracker_->GetReplacementOf(node);
}

Node* EscapeAnalysisResult::GetVirtualObjectField(const VirtualObject* vobject,
                                                  int field, Node* effect) {
  return tracker_->variable_states_.Get(vobject->FieldAt(field).FromJust(),
                                        effect);
}

const VirtualObject* EscapeAnalysisResult::GetVirtualObject(Node* node) {
  return tracker_->virtual_objects_[node];
}

VirtualObject::VirtualObject(VariableTracker* var_states, VirtualObject::Id id,
                             int size)
    : Dependable(var_states->zone()), id_(id), fields_(var_states->zone()) {
  DCHECK(size % kPointerSize == 0);
  TRACE("Creating VirtualObject id:%d size:%d\n", id, size);
  int num_fields = size / kPointerSize;
  fields_.reserve(num_fields);
  for (int i = 0; i < num_fields; ++i) {
    fields_.push_back(var_states->NewVariable());
  }
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
