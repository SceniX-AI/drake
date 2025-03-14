#include "drake/systems/framework/leaf_system.h"

#include <cmath>
#include <limits>

#include "absl/container/inlined_vector.h"

#include "drake/common/pointer_cast.h"
#include "drake/systems/framework/system_symbolic_inspector.h"
#include "drake/systems/framework/value_checker.h"

namespace drake {
namespace systems {

namespace {

// Returns the next sample time for the given @p attribute.
template <typename T>
T GetNextSampleTime(const PeriodicEventData& attribute,
                    const T& current_time_sec) {
  const double period = attribute.period_sec();
  DRAKE_ASSERT(period > 0);
  const double offset = attribute.offset_sec();
  DRAKE_ASSERT(offset >= 0);

  // If the first sample time hasn't arrived yet, then that is the next
  // sample time.
  if (current_time_sec < offset) {
    return offset;
  }

  // Compute the index in the sequence of samples for the next time to sample,
  // which should be greater than the present time.
  using std::ceil;
  const T offset_time = current_time_sec - offset;
  const T next_k = ceil(offset_time / period);
  T next_t = offset + next_k * period;
  if (next_t <= current_time_sec) {
    next_t = offset + (next_k + 1) * period;
  }
  DRAKE_ASSERT(next_t > current_time_sec);
  return next_t;
}

}  // namespace

template <typename T>
LeafSystem<T>::~LeafSystem() {}

template <typename T>
std::unique_ptr<CompositeEventCollection<T>>
LeafSystem<T>::DoAllocateCompositeEventCollection() const {
  return std::make_unique<LeafCompositeEventCollection<T>>();
}

template <typename T>
std::unique_ptr<LeafContext<T>> LeafSystem<T>::AllocateContext() const {
  return dynamic_pointer_cast_or_throw<LeafContext<T>>(
      System<T>::AllocateContext());
}

template <typename T>
std::unique_ptr<EventCollection<PublishEvent<T>>>
LeafSystem<T>::AllocateForcedPublishEventCollection() const {
  auto collection =
      LeafEventCollection<PublishEvent<T>>::MakeForcedEventCollection();
  if (this->forced_publish_events_exist())
    collection->SetFrom(this->get_forced_publish_events());
  return collection;
}

template <typename T>
std::unique_ptr<EventCollection<DiscreteUpdateEvent<T>>>
LeafSystem<T>::AllocateForcedDiscreteUpdateEventCollection() const {
  auto collection =
      LeafEventCollection<DiscreteUpdateEvent<T>>::MakeForcedEventCollection();
  if (this->forced_discrete_update_events_exist())
    collection->SetFrom(this->get_forced_discrete_update_events());
  return collection;
}

template <typename T>
std::unique_ptr<EventCollection<UnrestrictedUpdateEvent<T>>>
LeafSystem<T>::AllocateForcedUnrestrictedUpdateEventCollection() const {
  auto collection = LeafEventCollection<
      UnrestrictedUpdateEvent<T>>::MakeForcedEventCollection();
  if (this->forced_unrestricted_update_events_exist())
    collection->SetFrom(this->get_forced_unrestricted_update_events());
  return collection;
}

template <typename T>
std::unique_ptr<ContextBase> LeafSystem<T>::DoAllocateContext() const {
  std::unique_ptr<LeafContext<T>> context = DoMakeLeafContext();
  this->InitializeContextBase(&*context);

  // Reserve parameters via delegation to subclass.
  context->init_parameters(this->AllocateParameters());

  // Reserve state via delegation to subclass.
  context->init_continuous_state(this->AllocateContinuousState());
  context->init_discrete_state(this->AllocateDiscreteState());
  context->init_abstract_state(this->AllocateAbstractState());

  // At this point this LeafContext is complete except possibly for
  // inter-Context dependencies involving port connections to peers or
  // parent. We can now perform some final sanity checks.

  // The numeric vectors used for parameters and state must be contiguous,
  // i.e., valid BasicVectors. In general, a Context's numeric vectors can be
  // any kind of VectorBase including scatter-gather implementations like
  // Supervector. But for a LeafContext, we only allow BasicVectors, which are
  // guaranteed to have a contiguous storage layout.

  // If xc is not BasicVector, the dynamic_cast will yield nullptr, and the
  // invariant-checker will complain.
  const VectorBase<T>* const xc = &context->get_continuous_state_vector();
  internal::CheckBasicVectorInvariants(dynamic_cast<const BasicVector<T>*>(xc));

  // The discrete state must all be valid BasicVectors.
  for (const BasicVector<T>* group :
       context->get_state().get_discrete_state().get_data()) {
    internal::CheckBasicVectorInvariants(group);
  }

  // The numeric parameters must all be valid BasicVectors.
  const int num_numeric_parameters = context->num_numeric_parameter_groups();
  for (int i = 0; i < num_numeric_parameters; ++i) {
    const BasicVector<T>& group = context->get_numeric_parameter(i);
    internal::CheckBasicVectorInvariants(&group);
  }

  // Allow derived LeafSystem to validate allocated Context.
  DoValidateAllocatedLeafContext(*context);

  return context;
}

template <typename T>
void LeafSystem<T>::SetDefaultParameters(const Context<T>& context,
                                         Parameters<T>* parameters) const {
  this->ValidateContext(context);
  this->ValidateCreatedForThisSystem(parameters);
  for (int i = 0; i < parameters->num_numeric_parameter_groups(); i++) {
    BasicVector<T>& p = parameters->get_mutable_numeric_parameter(i);
    auto model_vector = model_numeric_parameters_.CloneVectorModel<T>(i);
    if (model_vector != nullptr) {
      p.SetFrom(*model_vector);
    } else {
      p.SetFromVector(VectorX<T>::Constant(p.size(), 1.0));
    }
  }
  for (int i = 0; i < parameters->num_abstract_parameters(); i++) {
    AbstractValue& p = parameters->get_mutable_abstract_parameter(i);
    auto model_value = model_abstract_parameters_.CloneModel(i);
    p.SetFrom(*model_value);
  }
}

template <typename T>
void LeafSystem<T>::SetDefaultState(const Context<T>& context,
                                    State<T>* state) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(state != nullptr);
  this->ValidateCreatedForThisSystem(state);
  ContinuousState<T>& xc = state->get_mutable_continuous_state();
  xc.SetFromVector(model_continuous_state_vector_->get_value());

  DiscreteValues<T>& xd = state->get_mutable_discrete_state();

  // Check that _if_ we have models, there is one for each group.
  DRAKE_DEMAND(model_discrete_state_.num_groups() == 0 ||
               model_discrete_state_.num_groups() == xd.num_groups());

  if (model_discrete_state_.num_groups() > 0) {
    xd.SetFrom(model_discrete_state_);
  } else {
    // With no model vector, we just zero all the discrete variables.
    for (int i = 0; i < xd.num_groups(); i++) {
      BasicVector<T>& s = xd.get_mutable_vector(i);
      s.SetFromVector(VectorX<T>::Zero(s.size()));
    }
  }

  AbstractValues& xa = state->get_mutable_abstract_state();
  xa.SetFrom(AbstractValues(model_abstract_states_.CloneAllModels()));
}

template <typename T>
std::unique_ptr<ContinuousState<T>> LeafSystem<T>::AllocateTimeDerivatives()
    const {
  return AllocateContinuousState();
}

template <typename T>
std::unique_ptr<DiscreteValues<T>> LeafSystem<T>::AllocateDiscreteVariables()
    const {
  return AllocateDiscreteState();
}

namespace {

template <typename T>
std::unique_ptr<SystemSymbolicInspector> MakeSystemSymbolicInspector(
    const System<T>& system) {
  using symbolic::Expression;
  // We use different implementations when T = Expression or not.
  if constexpr (std::is_same_v<T, Expression>) {
    return std::make_unique<SystemSymbolicInspector>(system);
  } else {
    std::unique_ptr<System<Expression>> converted = system.ToSymbolicMaybe();
    if (converted) {
      return std::make_unique<SystemSymbolicInspector>(*converted);
    } else {
      return nullptr;
    }
  }
}

}  // namespace

template <typename T>
std::multimap<int, int> LeafSystem<T>::GetDirectFeedthroughs() const {
  // The input -> output feedthrough result we'll return to the user.
  std::multimap<int, int> feedthrough;

  // Helper to grab the `const CacheEntry&` for an output port.
  auto get_leaf_output_port_cache_entry =
      [this](OutputPortIndex o) -> const CacheEntry& {
    const OutputPortBase& base = this->GetOutputPortBaseOrThrow(
        __func__, o, /* warn_deprecated = */ false);
    DRAKE_ASSERT(typeid(base) == typeid(LeafOutputPort<T>));
    return static_cast<const LeafOutputPort<T>&>(base).cache_entry();
  };

  // The `unknown` worklist will contain the pairs for which we don't know an
  // answer yet.
  std::set<std::pair<InputPortIndex, OutputPortIndex>> unknown;

  // To start, we'll fill in `unknown` with the full cross product of inputs and
  // outputs, unless the output port's prerequisites provides an obvious answer
  // that doesn't require walking the dependency graph. Our outer loop will be
  // over the output ports, because often those details are sufficient on their
  // own without the inner loop over input ports.
  for (OutputPortIndex o{0}; o < this->num_output_ports(); ++o) {
    const std::set<DependencyTicket>& prerequisites =
        get_leaf_output_port_cache_entry(o).prerequisites();

    // One obvious short-circuit is when this output uses "all input". In this
    // case we can directly populate our result instead of adding to `unknown`.
    if (prerequisites.contains(this->all_input_ports_ticket())) {
      for (InputPortIndex i{0}; i < this->num_input_ports(); ++i) {
        feedthrough.emplace(i, o);
      }
      continue;
    }

    // Another worthwhile short-circuit is when this output directly depends
    // *only* on simple tickets that are obviously unaffected by inputs (time,
    // state, parameters, etc.). In this case there is nothing to do: no
    // feedthrough to add to the return value and no candidates to add to the
    // `unknown` worklist.
    if (std::all_of(prerequisites.begin(), prerequisites.end(),
                    [this](const auto& ticket) {
                      return this->IsObviouslyNotInputDependent(ticket);
                    })) {
      continue;
    }

    // Our inner loop iterates over all input ports (with respect to the current
    // output port). If the input is a direct dependency then we can immediately
    // add it to the result; otherwise, we must add it to the `unknown` worklist
    // for a more careful inspection in the second half of this function.
    for (InputPortIndex i{0}; i < this->num_input_ports(); ++i) {
      const InputPortBase& input = this->GetInputPortBaseOrThrow(
          __func__, i, /* warn_deprecated = */ false);
      if (prerequisites.contains(input.ticket())) {
        feedthrough.emplace(i, o);
      } else {
        unknown.emplace(std::make_pair(i, o));
      }
    }
  }

  // We might already be done.
  if (unknown.empty()) {
    return feedthrough;
  }

  // A helper function that removes an item from `unknown`.
  const auto remove_unknown = [&unknown](const auto& in_out_pair) {
    const auto num_erased = unknown.erase(in_out_pair);
    DRAKE_DEMAND(num_erased == 1);
  };

  // A helper function that adds this in/out pair to the feedthrough result.
  const auto add_to_feedthrough = [&feedthrough](const auto& in_out_pair) {
    feedthrough.emplace(in_out_pair.first, in_out_pair.second);
  };

  // Ask the dependency graph if it can provide definitive information about
  // the input/output pairs. If so remove those pairs from the `unknown` set.
  auto context = this->AllocateContext();
  const auto orig_unknown = unknown;
  for (const auto& input_output : orig_unknown) {
    // Get the CacheEntry associated with the output port in this pair.
    const auto& cache_entry =
        get_leaf_output_port_cache_entry(input_output.second);

    // If the user left the output prerequisites unspecified, then the cache
    // entry tells us nothing useful about feedthrough for this pair.
    if (cache_entry.has_default_prerequisites())
      continue;  // Leave this one "unknown".

    // Probe the dependency path and believe the result.
    const InputPortBase& input = this->GetInputPortBaseOrThrow(
        __func__, input_output.first, /* warn_deprecated = */ false);
    const auto& input_tracker = context->get_tracker(input.ticket());
    auto& value = cache_entry.get_mutable_cache_entry_value(*context);
    value.mark_up_to_date();
    const int64_t change_event = context->start_new_change_event();
    input_tracker.NoteValueChange(change_event);

    if (value.is_out_of_date()) {
      add_to_feedthrough(input_output);
    }

    // Regardless of the result we have all we need to know now.
    remove_unknown(input_output);

    // Undo the mark_up_to_date() we just did a few lines up.  It shouldn't
    // matter at all on this throwaway context, but perhaps it's best not
    // to leave garbage values marked valid for longer than required.
    value.mark_out_of_date();
  }

  // If the dependency graph resolved all pairs, no need for symbolic analysis.
  if (unknown.empty()) {
    return feedthrough;
  }

  // Otherwise, see if we can get a symbolic inspector to analyze them.
  // If not, we have to assume they are feedthrough.
  auto inspector = MakeSystemSymbolicInspector(*this);
  for (const auto& input_output : unknown) {
    if (!inspector || inspector->IsConnectedInputToOutput(
                          input_output.first, input_output.second)) {
      add_to_feedthrough(input_output);
    }
    // No need to clean up the `unknown` set here.
  }
  return feedthrough;
}

template <typename T>
LeafSystem<T>::LeafSystem() : LeafSystem(SystemScalarConverter{}) {}

template <typename T>
LeafSystem<T>::LeafSystem(SystemScalarConverter converter)
    : System<T>(std::move(converter)) {
  this->set_forced_publish_events(AllocateForcedPublishEventCollection());
  this->set_forced_discrete_update_events(
      AllocateForcedDiscreteUpdateEventCollection());
  this->set_forced_unrestricted_update_events(
      AllocateForcedUnrestrictedUpdateEventCollection());
  per_step_events_.set_system_id(this->get_system_id());
  initialization_events_.set_system_id(this->get_system_id());
  model_discrete_state_.set_system_id(this->get_system_id());
}

template <typename T>
std::unique_ptr<LeafContext<T>> LeafSystem<T>::DoMakeLeafContext() const {
  return std::make_unique<LeafContext<T>>();
}

template <typename T>
T LeafSystem<T>::DoCalcWitnessValue(
    const Context<T>& context, const WitnessFunction<T>& witness_func) const {
  DRAKE_DEMAND(this == &witness_func.get_system());
  return witness_func.CalcWitnessValue(context);
}

template <typename T>
void LeafSystem<T>::AddTriggeredWitnessFunctionToCompositeEventCollection(
    Event<T>* event, CompositeEventCollection<T>* events) const {
  DRAKE_DEMAND(event != nullptr);
  DRAKE_DEMAND(event->template has_event_data<WitnessTriggeredEventData<T>>());
  DRAKE_DEMAND(events != nullptr);
  event->AddToComposite(events);
}

template <typename T>
void LeafSystem<T>::DoCalcNextUpdateTime(const Context<T>& context,
                                         CompositeEventCollection<T>* events,
                                         T* time) const {
  T min_time = std::numeric_limits<double>::infinity();

  if (!periodic_events_.HasEvents()) {
    *time = min_time;
    return;
  }

  // Calculate which events to fire. Use an InlinedVector so that small-ish
  // numbers of events can be processed without heap allocations. Our threshold
  // for "small" is the same amount that would cause an EventCollection to
  // grow beyond its pre-allocated size.
  using NextEventsVector = absl::InlinedVector<
      const Event<T>*, LeafEventCollection<PublishEvent<T>>::kDefaultCapacity>;
  NextEventsVector next_events;

  // Find the minimum next sample time across all declared periodic events,
  // and store the set of declared events that will occur at that time.
  // There are three lists to run through in a CompositeEventCollection.

  // We give absl hidden visibility in Drake so can't capture the absl vector
  // (otherwise we suffer a warning). Works as a parameter though.
  auto scan_events = [&context, &min_time](const auto& typed_events,
                                           NextEventsVector* event_list) {
    for (const auto* event : typed_events.get_events()) {
      const PeriodicEventData* event_data =
          event->template get_event_data<PeriodicEventData>();
      DRAKE_DEMAND(event_data != nullptr);
      const T t = GetNextSampleTime(*event_data, context.get_time());
      if (t < min_time) {
        min_time = t;
        *event_list = {event};
      } else if (t == min_time) {
        event_list->push_back(event);
      }
    }
  };

  scan_events(periodic_events_.get_publish_events(), &next_events);
  scan_events(periodic_events_.get_discrete_update_events(), &next_events);
  scan_events(periodic_events_.get_unrestricted_update_events(), &next_events);

  // Write out the events that fire at min_time.
  *time = min_time;
  for (const Event<T>* event : next_events) {
    event->AddToComposite(events);
  }
}

template <typename T>
std::unique_ptr<ContinuousState<T>> LeafSystem<T>::AllocateContinuousState()
    const {
  DRAKE_DEMAND(model_continuous_state_vector_->size() ==
               this->num_continuous_states());
  const SystemBase::ContextSizes& sizes = this->get_context_sizes();
  auto result = std::make_unique<ContinuousState<T>>(
      model_continuous_state_vector_->Clone(), sizes.num_generalized_positions,
      sizes.num_generalized_velocities, sizes.num_misc_continuous_states);
  result->set_system_id(this->get_system_id());
  return result;
}

template <typename T>
std::unique_ptr<DiscreteValues<T>> LeafSystem<T>::AllocateDiscreteState()
    const {
  return model_discrete_state_.Clone();
}

template <typename T>
std::unique_ptr<AbstractValues> LeafSystem<T>::AllocateAbstractState() const {
  return std::make_unique<AbstractValues>(
      model_abstract_states_.CloneAllModels());
}

template <typename T>
std::unique_ptr<Parameters<T>> LeafSystem<T>::AllocateParameters() const {
  std::vector<std::unique_ptr<BasicVector<T>>> numeric_params;
  numeric_params.reserve(model_numeric_parameters_.size());
  for (int i = 0; i < model_numeric_parameters_.size(); ++i) {
    auto param = model_numeric_parameters_.CloneVectorModel<T>(i);
    DRAKE_ASSERT(param != nullptr);
    numeric_params.emplace_back(std::move(param));
  }
  std::vector<std::unique_ptr<AbstractValue>> abstract_params;
  abstract_params.reserve(model_abstract_parameters_.size());
  for (int i = 0; i < model_abstract_parameters_.size(); ++i) {
    auto param = model_abstract_parameters_.CloneModel(i);
    DRAKE_ASSERT(param != nullptr);
    abstract_params.emplace_back(std::move(param));
  }
  auto result = std::make_unique<Parameters<T>>(std::move(numeric_params),
                                                std::move(abstract_params));
  result->set_system_id(this->get_system_id());
  return result;
}

template <typename T>
int LeafSystem<T>::DeclareNumericParameter(const BasicVector<T>& model_vector) {
  const NumericParameterIndex index(model_numeric_parameters_.size());
  model_numeric_parameters_.AddVectorModel(index, model_vector.Clone());
  MaybeDeclareVectorBaseInequalityConstraint(
      "parameter " + std::to_string(index), model_vector,
      [index](const Context<T>& context) -> const VectorBase<T>& {
        const BasicVector<T>& result = context.get_numeric_parameter(index);
        return result;
      });
  this->AddNumericParameter(index);
  return index;
}

template <typename T>
int LeafSystem<T>::DeclareAbstractParameter(const AbstractValue& model_value) {
  const AbstractParameterIndex index(model_abstract_parameters_.size());
  model_abstract_parameters_.AddModel(index, model_value.Clone());
  this->AddAbstractParameter(index);
  return index;
}

template <typename T>
ContinuousStateIndex LeafSystem<T>::DeclareContinuousState(
    int num_state_variables) {
  const int num_q = 0, num_v = 0;
  return DeclareContinuousState(num_q, num_v, num_state_variables);
}

template <typename T>
ContinuousStateIndex LeafSystem<T>::DeclareContinuousState(int num_q, int num_v,
                                                           int num_z) {
  const int n = num_q + num_v + num_z;
  return DeclareContinuousState(BasicVector<T>(VectorX<T>::Zero(n)), num_q,
                                num_v, num_z);
}

template <typename T>
ContinuousStateIndex LeafSystem<T>::DeclareContinuousState(
    const BasicVector<T>& model_vector) {
  const int num_q = 0, num_v = 0;
  const int num_z = model_vector.size();
  return DeclareContinuousState(model_vector, num_q, num_v, num_z);
}

template <typename T>
ContinuousStateIndex LeafSystem<T>::DeclareContinuousState(
    const BasicVector<T>& model_vector, int num_q, int num_v, int num_z) {
  DRAKE_DEMAND(model_vector.size() == num_q + num_v + num_z);
  model_continuous_state_vector_ = model_vector.Clone();

  // Note that only the last DeclareContinuousState() takes effect;
  // we're not accumulating these as we do for discrete & abstract states.
  SystemBase::ContextSizes& context_sizes = this->get_mutable_context_sizes();
  context_sizes.num_generalized_positions = num_q;
  context_sizes.num_generalized_velocities = num_v;
  context_sizes.num_misc_continuous_states = num_z;

  MaybeDeclareVectorBaseInequalityConstraint(
      "continuous state", model_vector,
      [](const Context<T>& context) -> const VectorBase<T>& {
        const ContinuousState<T>& state = context.get_continuous_state();
        return state.get_vector();
      });

  return ContinuousStateIndex(0);
}

template <typename T>
DiscreteStateIndex LeafSystem<T>::DeclareDiscreteState(
    const BasicVector<T>& model_vector) {
  const DiscreteStateIndex index(model_discrete_state_.num_groups());
  model_discrete_state_.AppendGroup(model_vector.Clone());
  this->AddDiscreteStateGroup(index);
  MaybeDeclareVectorBaseInequalityConstraint(
      "discrete state", model_vector,
      [index](const Context<T>& context) -> const VectorBase<T>& {
        const BasicVector<T>& state = context.get_discrete_state(index);
        return state;
      });
  return index;
}

template <typename T>
DiscreteStateIndex LeafSystem<T>::DeclareDiscreteState(
    const Eigen::Ref<const VectorX<T>>& vector) {
  return DeclareDiscreteState(BasicVector<T>(vector));
}

template <typename T>
DiscreteStateIndex LeafSystem<T>::DeclareDiscreteState(
    int num_state_variables) {
  DRAKE_DEMAND(num_state_variables >= 0);
  return DeclareDiscreteState(VectorX<T>::Zero(num_state_variables));
}

template <typename T>
AbstractStateIndex LeafSystem<T>::DeclareAbstractState(
    const AbstractValue& model_value) {
  const AbstractStateIndex index(model_abstract_states_.size());
  model_abstract_states_.AddModel(index, model_value.Clone());
  this->AddAbstractState(index);
  return index;
}

template <typename T>
InputPort<T>& LeafSystem<T>::DeclareVectorInputPort(
    std::variant<std::string, UseDefaultName> name,
    const BasicVector<T>& model_vector,
    std::optional<RandomDistribution> random_type) {
  const int size = model_vector.size();
  const int index = this->num_input_ports();
  model_input_values_.AddVectorModel(index, model_vector.Clone());
  MaybeDeclareVectorBaseInequalityConstraint(
      "input " + std::to_string(index), model_vector,
      [this, index](const Context<T>& context) -> const VectorBase<T>& {
        return this->get_input_port(index).template Eval<BasicVector<T>>(
            context);
      });
  return this->DeclareInputPort(NextInputPortName(std::move(name)),
                                kVectorValued, size, random_type);
}

template <typename T>
InputPort<T>& LeafSystem<T>::DeclareVectorInputPort(
    std::variant<std::string, UseDefaultName> name, int size,
    std::optional<RandomDistribution> random_type) {
  return DeclareVectorInputPort(std::move(name), BasicVector<T>(size),
                                random_type);
}

template <typename T>
InputPort<T>& LeafSystem<T>::DeclareAbstractInputPort(
    std::variant<std::string, UseDefaultName> name,
    const AbstractValue& model_value) {
  const int next_index = this->num_input_ports();
  model_input_values_.AddModel(next_index, model_value.Clone());
  return this->DeclareInputPort(NextInputPortName(std::move(name)),
                                kAbstractValued, 0 /* size */);
}

template <typename T>
void LeafSystem<T>::DeprecateInputPort(const InputPort<T>& port,
                                       std::string message) {
  InputPort<T>& mutable_port =
      const_cast<InputPort<T>&>(this->get_input_port(port.get_index()));
  DRAKE_THROW_UNLESS(&mutable_port == &port);
  DRAKE_THROW_UNLESS(mutable_port.get_deprecation() == std::nullopt);
  mutable_port.set_deprecation({std::move(message)});
}

template <typename T>
LeafOutputPort<T>& LeafSystem<T>::DeclareVectorOutputPort(
    std::variant<std::string, UseDefaultName> name,
    const BasicVector<T>& model_vector,
    typename LeafOutputPort<T>::CalcVectorCallback vector_calc_function,
    std::set<DependencyTicket> prerequisites_of_calc) {
  auto& port = CreateVectorLeafOutputPort(
      NextOutputPortName(std::move(name)), model_vector.size(),
      MakeAllocateCallback(model_vector), std::move(vector_calc_function),
      std::move(prerequisites_of_calc));
  return port;
}

template <typename T>
LeafOutputPort<T>& LeafSystem<T>::DeclareAbstractOutputPort(
    std::variant<std::string, UseDefaultName> name,
    typename LeafOutputPort<T>::AllocCallback alloc,
    typename LeafOutputPort<T>::CalcCallback calc,
    std::set<DependencyTicket> prerequisites_of_calc) {
  auto calc_function = [captured_calc = std::move(calc)](
                           const ContextBase& context_base,
                           AbstractValue* result) {
    const Context<T>& context = dynamic_cast<const Context<T>&>(context_base);
    return captured_calc(context, result);
  };
  auto& port = CreateAbstractLeafOutputPort(
      NextOutputPortName(std::move(name)),
      ValueProducer(std::move(alloc), std::move(calc_function)),
      std::move(prerequisites_of_calc));
  return port;
}

template <typename T>
LeafOutputPort<T>& LeafSystem<T>::DeclareStateOutputPort(
    std::variant<std::string, UseDefaultName> name,
    ContinuousStateIndex state_index) {
  DRAKE_THROW_UNLESS(state_index.is_valid());
  DRAKE_THROW_UNLESS(state_index == 0);
  return DeclareVectorOutputPort(
      std::move(name), *model_continuous_state_vector_,
      [](const Context<T>& context, BasicVector<T>* output) {
        output->SetFrom(context.get_continuous_state_vector());
      },
      {this->xc_ticket()});
}

template <typename T>
LeafOutputPort<T>& LeafSystem<T>::DeclareStateOutputPort(
    std::variant<std::string, UseDefaultName> name,
    DiscreteStateIndex state_index) {
  // DiscreteValues::get_vector already bounds checks the index, so we don't
  // need to guard it here.
  return DeclareVectorOutputPort(
      std::move(name), this->model_discrete_state_.get_vector(state_index),
      [state_index](const Context<T>& context, BasicVector<T>* output) {
        output->SetFrom(context.get_discrete_state(state_index));
      },
      {this->discrete_state_ticket(state_index)});
}

template <typename T>
LeafOutputPort<T>& LeafSystem<T>::DeclareStateOutputPort(
    std::variant<std::string, UseDefaultName> name,
    AbstractStateIndex state_index) {
  DRAKE_THROW_UNLESS(state_index.is_valid());
  DRAKE_THROW_UNLESS(state_index >= 0);
  DRAKE_THROW_UNLESS(state_index < this->model_abstract_states_.size());
  return DeclareAbstractOutputPort(
      std::move(name),
      [this, state_index]() {
        return this->model_abstract_states_.CloneModel(state_index);
      },
      [state_index](const Context<T>& context, AbstractValue* output) {
        output->SetFrom(context.get_abstract_state().get_value(state_index));
      },
      {this->abstract_state_ticket(state_index)});
}

template <typename T>
void LeafSystem<T>::DeprecateOutputPort(const OutputPort<T>& port,
                                        std::string message) {
  OutputPort<T>& mutable_port =
      const_cast<OutputPort<T>&>(this->get_output_port(port.get_index()));
  DRAKE_THROW_UNLESS(&mutable_port == &port);
  DRAKE_THROW_UNLESS(mutable_port.get_deprecation() == std::nullopt);
  mutable_port.set_deprecation({std::move(message)});
}

template <typename T>
std::unique_ptr<WitnessFunction<T>> LeafSystem<T>::MakeWitnessFunction(
    const std::string& description,
    const WitnessFunctionDirection& direction_type,
    std::function<T(const Context<T>&)> calc) const {
  return std::make_unique<WitnessFunction<T>>(this, this, description,
                                              direction_type, calc);
}

template <typename T>
std::unique_ptr<WitnessFunction<T>> LeafSystem<T>::MakeWitnessFunction(
    const std::string& description,
    const WitnessFunctionDirection& direction_type,
    std::function<T(const Context<T>&)> calc, const Event<T>& e) const {
  return std::make_unique<WitnessFunction<T>>(this, this, description,
                                              direction_type, calc, e.Clone());
}

template <typename T>
SystemConstraintIndex LeafSystem<T>::DeclareEqualityConstraint(
    ContextConstraintCalc<T> calc, int count, std::string description) {
  return DeclareInequalityConstraint(std::move(calc),
                                     SystemConstraintBounds::Equality(count),
                                     std::move(description));
}

template <typename T>
SystemConstraintIndex LeafSystem<T>::DeclareInequalityConstraint(
    ContextConstraintCalc<T> calc, SystemConstraintBounds bounds,
    std::string description) {
  return this->AddConstraint(std::make_unique<SystemConstraint<T>>(
      this, std::move(calc), std::move(bounds), std::move(description)));
}

template <typename T>
std::unique_ptr<AbstractValue> LeafSystem<T>::DoAllocateInput(
    const InputPort<T>& input_port) const {
  std::unique_ptr<AbstractValue> model_result =
      model_input_values_.CloneModel(input_port.get_index());
  if (model_result) {
    return model_result;
  }
  if (input_port.get_data_type() == kVectorValued) {
    return std::make_unique<Value<BasicVector<T>>>(input_port.size());
  }
  throw std::logic_error(fmt::format(
      "System::AllocateInputAbstract(): a System with abstract input ports "
      "must pass a model_value to DeclareAbstractInputPort; the port[{}] "
      "named '{}' did not do so (System {})",
      input_port.get_index(), input_port.get_name(),
      this->GetSystemPathname()));
}

template <typename T>
std::map<PeriodicEventData, std::vector<const Event<T>*>,
         PeriodicEventDataComparator>
LeafSystem<T>::DoMapPeriodicEventsByTiming(const Context<T>&) const {
  std::map<PeriodicEventData, std::vector<const Event<T>*>,
           PeriodicEventDataComparator>
      periodic_events_map;

  // Build a mapping from (offset,period) to the periodic events sharing
  // that trigger. There are three lists of different types in a
  // CompositeEventCollection; we use this generic lambda for all.
  auto map_events = [&periodic_events_map](const auto& typed_events) {
    for (const auto* event : typed_events.get_events()) {
      const PeriodicEventData* event_data =
          event->template get_event_data<PeriodicEventData>();
      DRAKE_DEMAND(event_data != nullptr);
      periodic_events_map[*event_data].push_back(event);
    }
  };

  map_events(periodic_events_.get_publish_events());
  map_events(periodic_events_.get_discrete_update_events());
  map_events(periodic_events_.get_unrestricted_update_events());

  return periodic_events_map;
}

template <typename T>
EventStatus LeafSystem<T>::DispatchPublishHandler(
    const Context<T>& context,
    const EventCollection<PublishEvent<T>>& events) const {
  const LeafEventCollection<PublishEvent<T>>& leaf_events =
      dynamic_cast<const LeafEventCollection<PublishEvent<T>>&>(events);
  // This function shouldn't have been called if no publish events.
  DRAKE_DEMAND(leaf_events.HasEvents());

  EventStatus overall_status = EventStatus::DidNothing();
  for (const PublishEvent<T>* event : leaf_events.get_events()) {
    const EventStatus per_event_status = event->handle(*this, context);
    overall_status.KeepMoreSevere(per_event_status);
    // Unlike the discrete & unrestricted event policy, we don't stop handling
    // publish events when one fails; we just report the first failure after all
    // the publishes are done.
  }
  return overall_status;
}

template <typename T>
EventStatus LeafSystem<T>::DispatchDiscreteVariableUpdateHandler(
    const Context<T>& context,
    const EventCollection<DiscreteUpdateEvent<T>>& events,
    DiscreteValues<T>* discrete_state) const {
  const LeafEventCollection<DiscreteUpdateEvent<T>>& leaf_events =
      dynamic_cast<const LeafEventCollection<DiscreteUpdateEvent<T>>&>(events);
  // This function shouldn't have been called if no discrete update events.
  DRAKE_DEMAND(leaf_events.HasEvents());

  // Must initialize the output argument with current discrete state contents.
  discrete_state->SetFrom(context.get_discrete_state());

  EventStatus overall_status = EventStatus::DidNothing();
  for (const DiscreteUpdateEvent<T>* event : leaf_events.get_events()) {
    const EventStatus per_event_status =
        event->handle(*this, context, discrete_state);
    overall_status.KeepMoreSevere(per_event_status);
    if (overall_status.failed()) break;  // Stop at the first disaster.
  }
  return overall_status;
}

template <typename T>
void LeafSystem<T>::DoApplyDiscreteVariableUpdate(
    const EventCollection<DiscreteUpdateEvent<T>>& events,
    DiscreteValues<T>* discrete_state, Context<T>* context) const {
  DRAKE_ASSERT(dynamic_cast<const LeafEventCollection<DiscreteUpdateEvent<T>>*>(
                   &events) != nullptr);
  DRAKE_DEMAND(events.HasEvents());
  // TODO(sherm1) Should swap rather than copy.
  context->get_mutable_discrete_state().SetFrom(*discrete_state);
}

template <typename T>
EventStatus LeafSystem<T>::DispatchUnrestrictedUpdateHandler(
    const Context<T>& context,
    const EventCollection<UnrestrictedUpdateEvent<T>>& events,
    State<T>* state) const {
  const LeafEventCollection<UnrestrictedUpdateEvent<T>>& leaf_events =
      dynamic_cast<const LeafEventCollection<UnrestrictedUpdateEvent<T>>&>(
          events);
  // This function shouldn't have been called if no unrestricted update events.
  DRAKE_DEMAND(leaf_events.HasEvents());

  // Must initialize the output argument with current state contents.
  // TODO(sherm1) Shouldn't require preloading of the output state; better to
  //  note just the changes since usually only a small subset will be changed by
  //  the callback function.
  state->SetFrom(context.get_state());

  EventStatus overall_status = EventStatus::DidNothing();
  for (const UnrestrictedUpdateEvent<T>* event : leaf_events.get_events()) {
    const EventStatus per_event_status = event->handle(*this, context, state);
    overall_status.KeepMoreSevere(per_event_status);
    if (overall_status.failed()) break;  // Stop at the first disaster.
  }
  return overall_status;
}

template <typename T>
void LeafSystem<T>::DoApplyUnrestrictedUpdate(
    const EventCollection<UnrestrictedUpdateEvent<T>>& events, State<T>* state,
    Context<T>* context) const {
  DRAKE_ASSERT(
      dynamic_cast<const LeafEventCollection<UnrestrictedUpdateEvent<T>>*>(
          &events) != nullptr);
  DRAKE_DEMAND(events.HasEvents());
  // TODO(sherm1) Should swap rather than copy.
  context->get_mutable_state().SetFrom(*state);
}

// The Diagram implementation of this method recursively visits sub-Diagrams
// until we get to this LeafSystem implementation, where we can actually look
// at the periodic discrete update Events. When no timing has yet been
// established (`timing` parameter is empty), the first LeafSystem
// to find a periodic discrete update event sets the timing for all the rest.
// After that every periodic discrete update event must have the same timing
// or this method throws an error message. If the timing is good, we return
// all the periodic discrete update events we find in this LeafSystem.
//
// Unit testing for this method is in diagram_test.cc where it is used in
// the leaf computation for the Diagram EvalUniquePeriodicDiscreteUpdate()
// recursion.
template <typename T>
void LeafSystem<T>::DoFindUniquePeriodicDiscreteUpdatesOrThrow(
    const char* api_name, const Context<T>& context,
    std::optional<PeriodicEventData>* timing,
    EventCollection<DiscreteUpdateEvent<T>>* events) const {
  unused(context);
  auto& leaf_collection =
      dynamic_cast<LeafEventCollection<DiscreteUpdateEvent<T>>&>(*events);

  for (const DiscreteUpdateEvent<T>* event :
       periodic_events_.get_discrete_update_events().get_events()) {
    DRAKE_DEMAND(event->get_trigger_type() == TriggerType::kPeriodic);
    const PeriodicEventData* const event_timing =
        event->template get_event_data<PeriodicEventData>();
    DRAKE_DEMAND(event_timing != nullptr);
    if (!timing->has_value())  // First find sets the required timing.
      *timing = *event_timing;
    if (!(*event_timing == *(*timing))) {
      throw std::logic_error(fmt::format(
          "{}(): found more than one "
          "periodic timing that triggers discrete update events. "
          "Timings were (offset,period)=({},{}) and ({},{}).",
          api_name, (*timing)->offset_sec(), (*timing)->period_sec(),
          event_timing->offset_sec(), event_timing->period_sec()));
    }
    leaf_collection.AddEvent(*event);
  }
}

template <typename T>
void LeafSystem<T>::DoGetPeriodicEvents(
    const Context<T>&, CompositeEventCollection<T>* events) const {
  events->SetFrom(periodic_events_);
}

template <typename T>
void LeafSystem<T>::DoGetPerStepEvents(
    const Context<T>&, CompositeEventCollection<T>* events) const {
  events->SetFrom(per_step_events_);
}

template <typename T>
void LeafSystem<T>::DoGetInitializationEvents(
    const Context<T>&, CompositeEventCollection<T>* events) const {
  events->SetFrom(initialization_events_);
}

template <typename T>
LeafOutputPort<T>& LeafSystem<T>::CreateVectorLeafOutputPort(
    std::string name, int fixed_size,
    typename LeafOutputPort<T>::AllocCallback vector_allocator,
    typename LeafOutputPort<T>::CalcVectorCallback vector_calculator,
    std::set<DependencyTicket> calc_prerequisites) {
  // Construct a suitable type-erased cache calculator from the given
  // BasicVector<T> calculator function.
  auto cache_calc_function = [vector_calculator](
                                 const ContextBase& context_base,
                                 AbstractValue* abstract) {
    // Profiling revealed that it is too expensive to do a dynamic_cast here.
    // A static_cast is safe as long as this is invoked only by methods that
    // validate the SystemId, so that we know this Context is ours. As of this
    // writing, only OutputPort::Eval and OutputPort::Calc invoke this and
    // they do so safely.
    auto& context = static_cast<const Context<T>&>(context_base);

    // The abstract value must be a Value<BasicVector<T>>, even if the
    // underlying object is a more-derived vector type.
    auto* value = abstract->maybe_get_mutable_value<BasicVector<T>>();

    // TODO(sherm1) Make this error message more informative by capturing
    // system and port index info.
    if (value == nullptr) {
      throw std::logic_error(fmt::format(
          "An output port calculation required a {} object for its result "
          "but got a {} object instead.",
          NiceTypeName::Get<Value<BasicVector<T>>>(),
          abstract->GetNiceTypeName()));
    }
    vector_calculator(context, value);
  };

  // The allocator function is identical between output port and cache.
  return CreateCachedLeafOutputPort(
      std::move(name), fixed_size,
      ValueProducer(std::move(vector_allocator),
                    std::move(cache_calc_function)),
      std::move(calc_prerequisites));
}

template <typename T>
LeafOutputPort<T>& LeafSystem<T>::CreateAbstractLeafOutputPort(
    std::string name, ValueProducer producer,
    std::set<DependencyTicket> calc_prerequisites) {
  return CreateCachedLeafOutputPort(std::move(name), std::nullopt /* size */,
                                    std::move(producer),
                                    std::move(calc_prerequisites));
}

template <typename T>
LeafOutputPort<T>& LeafSystem<T>::CreateCachedLeafOutputPort(
    std::string name, const std::optional<int>& fixed_size,
    ValueProducer value_producer,
    std::set<DependencyTicket> calc_prerequisites) {
  DRAKE_DEMAND(!calc_prerequisites.empty());
  // Create a cache entry for this output port.
  const OutputPortIndex oport_index(this->num_output_ports());
  CacheEntry& cache_entry = this->DeclareCacheEntry(
      "output port " + std::to_string(oport_index) + "(" + name + ") cache",
      std::move(value_producer), std::move(calc_prerequisites));

  // Create and install the port. Note that it has a separate ticket from
  // the cache entry; the port's tracker will be subscribed to the cache
  // entry's tracker when a Context is created.
  // TODO(sherm1) Use implicit_cast when available (from abseil).
  auto port = internal::FrameworkFactory::Make<LeafOutputPort<T>>(
      this,  // implicit_cast<const System<T>*>(this)
      this,  // implicit_cast<const SystemBase*>(this)
      this->get_system_id(), std::move(name), oport_index,
      this->assign_next_dependency_ticket(),
      fixed_size.has_value() ? kVectorValued : kAbstractValued,
      fixed_size.value_or(0), &cache_entry);
  LeafOutputPort<T>* const port_ptr = port.get();
  this->AddOutputPort(std::move(port));
  return *port_ptr;
}

template <typename T>
void LeafSystem<T>::MaybeDeclareVectorBaseInequalityConstraint(
    const std::string& kind, const VectorBase<T>& model_vector,
    const std::function<const VectorBase<T>&(const Context<T>&)>&
        get_vector_from_context) {
  Eigen::VectorXd lower_bound, upper_bound;
  model_vector.GetElementBounds(&lower_bound, &upper_bound);
  if (lower_bound.size() == 0 && upper_bound.size() == 0) {
    return;
  }
  // `indices` contains the indices in model_vector that are constrained,
  // namely either or both lower_bound(indices[i]) or upper_bound(indices[i])
  // is not inf.
  std::vector<int> indices;
  indices.reserve(model_vector.size());
  for (int i = 0; i < model_vector.size(); ++i) {
    if (!std::isinf(lower_bound(i)) || !std::isinf(upper_bound(i))) {
      indices.push_back(i);
    }
  }
  if (indices.empty()) {
    return;
  }
  Eigen::VectorXd constraint_lower_bound(indices.size());
  Eigen::VectorXd constraint_upper_bound(indices.size());
  for (int i = 0; i < static_cast<int>(indices.size()); ++i) {
    constraint_lower_bound(i) = lower_bound(indices[i]);
    constraint_upper_bound(i) = upper_bound(indices[i]);
  }
  this->DeclareInequalityConstraint(
      [get_vector_from_context, indices](const Context<T>& context,
                                         VectorX<T>* value) {
        const VectorBase<T>& model_vec = get_vector_from_context(context);
        value->resize(indices.size());
        for (int i = 0; i < static_cast<int>(indices.size()); ++i) {
          (*value)[i] = model_vec[indices[i]];
        }
      },
      {constraint_lower_bound, constraint_upper_bound},
      kind + " of type " + NiceTypeName::Get(model_vector));
}

}  // namespace systems
}  // namespace drake

DRAKE_DEFINE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    class ::drake::systems::LeafSystem);
