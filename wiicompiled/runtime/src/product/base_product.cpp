#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        Kind::BaseGame,
        "Super Paper Mario",
    };
    return descriptor;
}

} // namespace RuntimeProduct
