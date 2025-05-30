#pragma once

#include <memory>
#include <utility>

#include <Eigen/Dense>

#include "drake/common/default_scalars.h"
#include "drake/common/drake_copyable.h"
#include "drake/systems/framework/leaf_system.h"

namespace drake {
namespace systems {
namespace estimators {

/// A simple state observer for a continuous-time dynamical system of the form:
///  @f[ \dot{x} = f(x,u) @f]
///  @f[ y = g(x,u) @f]
/// the observer dynamics takes the form
///  @f[ \dot{\hat{x}} = f(\hat{x},u) + L(y - g(\hat{x},u)) @f]
/// where @f$\hat{x}@f$ is the estimated state of the original system. The
/// output of the observer system is @f$\hat{x}@f$.
///
/// Or a simple state observer for a discrete-time dynamical system of the form:
///  @f[ x[n+1] = f_d(x[n],u[n]) @f]
///  @f[ y[n] = g(x[n],u[n]) @f]
/// the observer dynamics takes the form
///  @f[ \hat{x}[n+1] = f_d(\hat{x}[n],u[n]) + L(y - g(\hat{x}[n],u[n])) @f]
/// where @f$\hat{x}@f$ is the estimated state of the original system.
/// The output of the observer system is @f$\hat{x}[n+1]@f$.
///
/// @system
/// name: LuenbergerObserver
/// input_ports:
/// - observed_system_input
/// - observed_system_output
/// output_ports:
/// - estimated_state
/// @endsystem
///
/// @ingroup estimator_systems
/// @tparam_default_scalar
template <typename T>
class LuenbergerObserver final : public LeafSystem<T> {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(LuenbergerObserver);

  /// Constructs the observer.
  ///
  /// @param observed_system  The forward model for the observer.  Currently,
  /// this system must have a maximum of one input port and exactly one output
  /// port.
  /// @param observed_system_context Required because it may contain parameters
  /// which we need to evaluate the system.
  /// @param observer_gain A m-by-n matrix where m is the number of state
  /// variables in @p observed_system, and n is the dimension of the output port
  /// of @p observed_system.
  ///
  /// @pre The observed_system output port must be vector-valued.
  LuenbergerObserver(std::shared_ptr<const System<T>> observed_system,
                     const Context<T>& observed_system_context,
                     const Eigen::Ref<const Eigen::MatrixXd>& observer_gain);

  /// Constructs the observer, without claiming ownership of `observed_system`.
  /// Note: The `observed_system` reference must remain valid for the lifetime
  /// of this system.
  /// @exclude_from_pydrake_mkdoc{This constructor is not bound.}
  LuenbergerObserver(const System<T>& observed_system,
                     const Context<T>& observed_system_context,
                     const Eigen::Ref<const Eigen::MatrixXd>& observer_gain);

  // Scalar-converting copy constructor.  See @ref system_scalar_conversion.
  template <typename U>
  explicit LuenbergerObserver(const LuenbergerObserver<U>& other);

  // Returns the input port that expects the input passed to the observed
  // system.
  const InputPort<T>& get_observed_system_input_input_port() const {
    return this->get_input_port(1);
  }

  // Returns the input port that expects the outputs of the observed system.
  const InputPort<T>& get_observed_system_output_input_port() const {
    return this->get_input_port(0);
  }

  // Returns the output port that provides the estimated state.
  const OutputPort<T>& get_estimated_state_output_port() const {
    return this->get_output_port(0);
  }

  /// Provides access to the observer gain.
  const Eigen::MatrixXd& observer_gain() { return observer_gain_; }

  /// Provides access via the short-hand name, L, too.
  const Eigen::MatrixXd& L() { return observer_gain_; }

 private:
  template <typename U>
  friend class LuenbergerObserver;

  // All constructors delegate to here.
  template <typename U>
  LuenbergerObserver(std::shared_ptr<const System<T>> observed_system,
                     const Context<U>& observed_system_context,
                     const Eigen::Ref<const Eigen::MatrixXd>& observer_gain, U);

  // Advance the state estimate using forward dynamics and the observer
  // gains. Continuous-time version.
  void DoCalcTimeDerivatives(const Context<T>& context,
                             ContinuousState<T>* derivatives) const override;
  // Discrete-time version.
  void DiscreteUpdate(const Context<T>& context,
                      DiscreteValues<T>* x_estimate) const;

  void UpdateObservedSystemContext(const Context<T>& context,
                                   Context<T>* observed_system_context) const;

  const std::shared_ptr<const System<T>> observed_system_;
  const Eigen::MatrixXd observer_gain_;  // Gain matrix (often called "L").

  const CacheEntry* observed_system_context_cache_entry_{};
};

}  // namespace estimators
}  // namespace systems
}  // namespace drake

DRAKE_DECLARE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    class drake::systems::estimators::LuenbergerObserver);
