#pragma once

namespace lwm {

// Builds a std::visit visitor from lambdas.
template <class... Ts> struct Overloaded : Ts...
{
    using Ts::operator()...;
};

} // namespace lwm
