#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/WrapDimUtils.h>
#include <ATen/core/DimVector.h>
#include <c10/util/Exception.h>
#include <c10/core/ScalarType.h>
#include <c10/core/Scalar.h>

namespace at {
namespace native {
namespace {

// The estimated integral of a function y of x,
// sampled at points (y_1, ..., y_n) that are separated by distance (dx_1, ..., dx_{n-1}),
// is given by the trapezoid rule:
//
// \sum_{i=1}^{n-1}  dx_i * (y_i + y_{i+1}) / 2
//
// TODO: if we extend TensorIterator to accept 3 inputs,
// we can probably make this a bit more performant.
Tensor do_trapezoid(const Tensor& y, const Tensor& dx, int64_t dim) {
    Tensor left = y.slice(dim, 0, -1);
    Tensor right = y.slice(dim, 1);
    // If the dimensions of 'dx' and '(left + right)' do not match
    // broadcasting is attempted here.
    return ((left + right) * dx).sum(dim) / 2.;
}

// When dx is constant, the above formula simplifies
// to dx * [(\sum_{i=1}^n y_i) - (y_1 + y_n)/2]
Tensor do_trapezoid(const Tensor& y, double dx, int64_t dim) {
    return (y.sum(dim) - (y.select(dim, 0) + y.select(dim, -1)) * (0.5)) * dx;
}

Tensor zeros_like_except(const Tensor& y, int64_t dim) {
    auto sizes = y.sizes().vec();
    dim = maybe_wrap_dim(dim, y.dim());
    sizes.erase(sizes.begin() + dim);
    return at::zeros(sizes, y.options());
}

Tensor do_cumulative_trapezoid(const Tensor& y, const Tensor& dx, int64_t dim) {
    Tensor left = y.slice(dim, 0, -1);
    Tensor right = y.slice(dim, 1);

    return ((left + right) * dx).cumsum(dim) / 2.;
}

Tensor do_cumulative_trapezoid(const Tensor& y, double dx, int64_t dim) {
    Tensor left = y.slice(dim, 0, -1);
    Tensor right = y.slice(dim, 1);

    return (dx /2. * (left + right)).cumsum(dim);
}
// Given the current shape of a Tensor and a target number of dimensions,
// returns a new shape with the same values as the original shape,
// but with '1's padded in the beginning to match the target number of dimensions.
// For example, curr_shape = (5,5,5) and target_n_dim = 6 ==> (1,1,1,5,5,5)
// Note that no padding will be added if the current shape has the greater than or equal
// number of dimensions than the target numbers of dimensions.
DimVector add_padding_to_shape(IntArrayRef curr_shape, int64_t target_n_dim) {
    if (curr_shape.size() >= target_n_dim)
        target_n_dim = curr_shape.size();
    DimVector new_shape(target_n_dim, 1);
    for (decltype(curr_shape.size()) i = 0; i < curr_shape.size(); i++) {
        new_shape[target_n_dim-i-1] = curr_shape[curr_shape.size()-i-1];
    }
    return new_shape;
}
}

Tensor trapezoid(const Tensor& y, const Tensor& x, int64_t dim) {
    dim = maybe_wrap_dim(dim, y);
    // asking for the integral with zero samples is a bit nonsensical,
    // but we'll return "0" to match numpy behavior.
    if (y.size(dim) == 0) {
        return zeros_like_except(y, dim);
    }
    TORCH_CHECK(y.scalar_type() != kBool && x.scalar_type() != kBool, "trapezoid: received a bool input for `x` or `y`, but bool is not supported")
    Tensor x_viewed;
    if (x.dim() == 1) {
        // This step takes 'x' with dimension (n,), and returns 'x_view' with
        // dimension (1,1,...,n,...,1,1) based on dim and y.dim() so that 'x'
        // can be broadcasted later to match 'y'.
        // Note: This behavior differs from numpy in that numpy tries to
        // broadcast 'dx', but this tries to broadcast 'x' to match 'y' instead.
        TORCH_CHECK(x.size(0) == y.size(dim), "trapezoid: There must be one `x` value for each sample point");
        DimVector new_sizes(y.dim(), 1); // shape = [1] * y.
        new_sizes[dim] = x.size(0); // shape[axis] = d.shape[0]
        x_viewed = x.view(new_sizes);
    } else if (x.dim() < y.dim()) {
        // When 'y' has more dimension than 'x', this step takes 'x' with dimension (n_1, n_2, ...),
        // and add '1's as dimensions in front to become (1, 1, ..., n_1, n_2), matching the dimension of 'y'.
        // This allows the subsequent slicing operations to proceed with any 'dim' without going out of bound.
        DimVector new_sizes = add_padding_to_shape(x.sizes(), y.dim());
        x_viewed = x.view(new_sizes);
    } else {
        x_viewed = x;
    }
    // Note the .slice operation reduces the dimension along 'dim' by 1.
    // The sizes of other dimensions are untouched.
    Tensor x_left = x_viewed.slice(dim, 0, -1);
    Tensor x_right = x_viewed.slice(dim, 1);

    Tensor dx = x_right - x_left;
    return do_trapezoid(y, dx, dim);
}

Tensor trapezoid(const Tensor& y, const Scalar& dx, int64_t dim) {
    // see above
    if (y.size(dim) == 0) {
        return zeros_like_except(y, dim);
    }
    TORCH_CHECK(y.scalar_type() != kBool, "trapezoid: received a bool input for `y`, but bool is not supported")
    TORCH_CHECK(!(dx.isComplex() ||  dx.isBoolean()), "trapezoid: Currently, we only support dx as a real number.");
    return do_trapezoid(y, dx.toDouble(), dim);
}

Tensor trapz(const Tensor& y, const Tensor& x, int64_t dim) {
    return at::native::trapezoid(y, x, dim);
}

Tensor trapz(const Tensor& y, double dx, int64_t dim) {
    return at::native::trapezoid(y, dx, dim);
}

Tensor cumulative_trapezoid(const Tensor& y, const Tensor& x, int64_t dim) {
    dim = maybe_wrap_dim(dim, y);
    TORCH_CHECK(y.scalar_type() != kBool && x.scalar_type() != kBool, "cumulative_trapezoid: received a bool input for `x` or `y`, but bool is not supported")
    Tensor x_viewed;
    if (x.dim() == 1) {
        TORCH_CHECK(x.size(0) == y.size(dim), "cumulative_trapezoid: There must be one `x` value for each sample point");
        DimVector new_sizes(y.dim(), 1); // shape = [1] * y.
        new_sizes[dim] = x.size(0); // shape[axis] = d.shape[0]
        x_viewed = x.view(new_sizes);
    } else if (x.dim() < y.dim()) {
        DimVector new_sizes = add_padding_to_shape(x.sizes(), y.dim());
        x_viewed = x.view(new_sizes);
    } else {
        x_viewed = x;
    }
    Tensor x_left = x_viewed.slice(dim, 0, -1);
    Tensor x_right = x_viewed.slice(dim, 1);
    Tensor dx = x_right - x_left;

    return do_cumulative_trapezoid(y, dx, dim);
}

Tensor cumulative_trapezoid(const Tensor& y, const Scalar& dx, int64_t dim) {
    TORCH_CHECK(y.scalar_type() != kBool, "cumulative_trapezoid: received a bool input for `y`, but bool is not supported")
    TORCH_CHECK(!(dx.isComplex() || dx.isBoolean()), "cumulative_trapezoid: Currently, we only support dx as a real number.");

    return do_cumulative_trapezoid(y, dx.toDouble(), dim);
}

}} // namespace at::native
