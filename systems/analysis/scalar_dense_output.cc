#include "drake/systems/analysis/scalar_dense_output.h"

namespace drake {
namespace systems {

template <typename T>
ScalarDenseOutput<T>::~ScalarDenseOutput() = default;

}  // namespace systems
}  // namespace drake

DRAKE_DEFINE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    class drake::systems::ScalarDenseOutput);
