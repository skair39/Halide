#ifndef HALIDE_GENERATOR_H_
#define HALIDE_GENERATOR_H_

/** \file
 *
 * Generator is a class used to encapsulate the building of Funcs in user pipelines.
 * A Generator is agnostic to JIT vs AOT compilation; it can be used for either
 * purpose, but is especially convenient to use for AOT compilation.
 *
 * A Generator automatically detects the run-time parameters (Param/ImageParams)
 * associated with the Func and (for AOT code) produces a function signature
 * with the correct params in the correct order.
 *
 * A Generator can also be customized via compile-time parameters (GeneratorParams),
 * which affect code generation.
 *
 * GeneratorParams, ImageParams, and Params are (by convention)
 * always public and always declared at the top of the Generator class,
 * in the order
 *
 *    GeneratorParam(s)
 *    ImageParam(s)
 *    Param(s)
 *
 * Preferred style is to use C++11 in-class initialization style, e.g.
 * \code
 *    GeneratorParam<int> magic{"magic", 42};
 * \endcode
 *
 * Note that the ImageParams/Params will appear in the C function
 * call in the order they are declared. (GeneratorParams are always
 * referenced by name, not position, so their order is irrelevant.)
 *
 * All Param variants declared as Generator members must have explicit
 * names, and all such names must match the regex [A-Za-z][A-Za-z_0-9]*
 * (i.e., essentially a C/C++ variable name, with some extra restrictions
 * on underscore use). By convention, the name should match the member-variable name.
 *
 * Generators are usually added to a global registry to simplify AOT build mechanics;
 * this is done by simply defining an instance of RegisterGenerator at static
 * scope:
 * \code
 *    RegisterGenerator<ExampleGen> register_jit_example{"jit_example"};
 * \endcode
 *
 * The registered name of the Generator is provided as an argument
 * (which must match the same rules as Param names, above).
 *
 * (If you are jitting, you may not need to bother registering your Generator,
 * but it's considered best practice to always do so anyway.)
 *
 * Most Generator classes will only need to provide a build() method
 * that the base class will call, and perhaps declare a Param and/or
 * GeneratorParam:
 *
 * \code
 *  class XorImage : public Generator<XorImage> {
 *  public:
 *      GeneratorParam<int> channels{"channels", 3};
 *      ImageParam input{UInt(8), 3, "input"};
 *      Param<uint8_t> mask{"mask"};
 *
 *      Func build() {
 *          Var x, y, c;
 *          Func f;
 *          f(x, y, c) = input(x, y, c) ^ mask;
 *          f.bound(c, 0, bound).reorder(c, x, y).unroll(c);
 *          return f;
 *      }
 *  };
 *  RegisterGenerator<XorImage> reg_xor{"xor_image"};
 * \endcode
 *
 * By default, this code schedules itself for 3-channel (RGB) images;
 * by changing the value of the "channels" GeneratorParam before calling
 * build() we can produce code suited for different channel counts.
 *
 * Note that a Generator is always executed with a specific Target
 * assigned to it, that you can access via the get_target() method.
 * (You should *not* use the global get_target_from_environment(), etc.
 * methods provided in Target.h)
 *
 * Your build() method will usually return a Func. If you have a
 * pipeline that outputs multiple Funcs, you can also return a
 * Pipeline object.
 */

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "Func.h"
#include "ObjectInstanceRegistry.h"
#include "Introspection.h"
#include "Target.h"

namespace Halide {

namespace Internal {

EXPORT extern const std::map<std::string, Halide::Type> &get_halide_type_enum_map();

/** generate_filter_main() is a convenient wrapper for GeneratorRegistry::create() +
 * compile_to_files();
 * it can be trivially wrapped by a "real" main() to produce a command-line utility
 * for ahead-of-time filter compilation. */
EXPORT int generate_filter_main(int argc, char **argv, std::ostream &cerr);

class GeneratorParamBase {
public:
    EXPORT explicit GeneratorParamBase(const std::string &name);
    EXPORT virtual ~GeneratorParamBase();
    virtual void from_string(const std::string &value_string) = 0;
    virtual std::string to_string() const = 0;

    const std::string name;

private:
    explicit GeneratorParamBase(const GeneratorParamBase &) = delete;
    void operator=(const GeneratorParamBase &) = delete;
};

}  // namespace Internal

/** GeneratorParam is a templated class that can be used to modify the behavior
 * of the Generator at code-generation time. GeneratorParams are commonly
 * specified in build files (e.g. Makefile) to customize the behavior of
 * a given Generator, thus they have a very constrained set of types to allow
 * for efficient specification via command-line flags. A GeneratorParm can be:
 *   - any float or int type.
 *   - bool
 *   - enum
 *   - Halide::Target
 *   - Halide::Type
 * All GeneratorParams have a default value. Arithmetic types can also
 * optionally specify min and max. Enum types must specify a string-to-value
 * map.
 *
 * Halide::Type is treated as though it were an enum, with the mappings:
 *
 *   "int8"     Halide::Int(8)
 *   "int16"    Halide::Int(16)
 *   "int32"    Halide::Int(32)
 *   "uint8"    Halide::UInt(8)
 *   "uint16"   Halide::UInt(16)
 *   "uint32"   Halide::UInt(32)
 *   "float32"  Halide::Float(32)
 *   "float64"  Halide::Float(64)
 *
 * No vector Types are currently supported by this mapping.
 *
 */
template <typename T> class GeneratorParam : public Internal::GeneratorParamBase {
public:
    template <typename T2 = T,
              typename std::enable_if<std::is_same<T2, Target>::value>::type * = nullptr>
    GeneratorParam(const std::string &name, const T &value)
        : GeneratorParamBase(name), value(value), min(value), max(value) {}

    // Note that "is_arithmetic" includes the bool type.
    template <typename T2 = T,
              typename std::enable_if<std::is_arithmetic<T2>::value>::type * = nullptr>
    GeneratorParam(const std::string &name, const T &value)
        : GeneratorParamBase(name), value(value), min(std::numeric_limits<T>::lowest()),
          max(std::numeric_limits<T>::max()) {}

    template <typename T2 = T,
              typename std::enable_if<std::is_arithmetic<T2>::value &&
                                      !std::is_same<T2, bool>::value>::type * = nullptr>
    GeneratorParam(const std::string &name, const T &value, const T &min, const T &max)
        : GeneratorParamBase(name),
          // Use the set() method so that out-of-range values are checked.
          // value(std::min(std::max(value, min), max)),
          min(min), max(max) {
        static_assert(std::is_arithmetic<T>::value && !std::is_same<T, bool>::value,
                      "Only arithmetic types may specify min and max");
        set(value);
    }

    template <typename T2 = T, typename std::enable_if<std::is_enum<T2>::value>::type * = nullptr>
    GeneratorParam(const std::string &name, const T &value,
                   const std::map<std::string, T> &enum_map)
        : GeneratorParamBase(name), value(value), min(std::numeric_limits<T>::lowest()),
          max(std::numeric_limits<T>::max()), enum_map(enum_map) {
        static_assert(std::is_enum<T>::value, "Only enum types may specify value maps");
    }

    // Special-case for Halide::Type, which has a built-in enum map (and no min or max).
    template <typename T2 = T, typename std::enable_if<std::is_same<T2, Halide::Type>::value>::type * = nullptr>
    GeneratorParam(const std::string &name, const T &value)
        : GeneratorParamBase(name), value(value), min(value),
          max(value), enum_map(Internal::get_halide_type_enum_map()) {
    }

    // Arithmetic values must fall within the range -- we don't silently clamp.
    template <typename T2 = T,
              typename std::enable_if<std::is_arithmetic<T2>::value>::type * = nullptr>
    void set(const T &new_value) {
        user_assert(new_value >= min && new_value <= max) << "Value out of range: " << new_value;
        value = new_value;
    }

    template <typename T2 = T,
              typename std::enable_if<!std::is_arithmetic<T2>::value>::type * = nullptr>
    void set(const T &new_value) {
        value = new_value;
    }

    void from_string(const std::string &new_value_string) override {
        // delegate to a function that we can specialize based on the template argument
        set(from_string_impl(new_value_string));
    }

    std::string to_string() const override {
        // delegate to a function that we can specialize based on the template argument
        return to_string_impl(value);
    }

    operator T() const { return value; }
    operator Expr() const { return Internal::make_const(type_of<T>(), value); }

private:
    T value;
    const T min, max;  // only for arithmetic types
    const std::map<std::string, T> enum_map;  // only for enums

    // Note that none of the string conversions are static:
    // the specializations for enum require access to enum_map

    // string conversions: Target
    template <typename T2 = T,
              typename std::enable_if<std::is_same<T2, Target>::value>::type * = nullptr>
    T from_string_impl(const std::string &s) const {
        return Target(s);
    }
    template <typename T2 = T,
              typename std::enable_if<std::is_same<T2, Target>::value>::type * = nullptr>
    std::string to_string_impl(const T& t) const {
        return t.to_string();
    }

    template <typename T2 = T,
              typename std::enable_if<std::is_same<T2, Halide::Type>::value>::type * = nullptr>
    T from_string_impl(const std::string &s) const {
        auto it = enum_map.find(s);
        user_assert(it != enum_map.end()) << "Enumeration value not found: " << s;
        return it->second;
    }
    template <typename T2 = T,
              typename std::enable_if<std::is_same<T2, Halide::Type>::value>::type * = nullptr>
    std::string to_string_impl(const T& t) const {
        for (auto key_value : enum_map) {
            if (t == key_value.second) {
                return key_value.first;
            }
        }
        user_assert(0) << "Type value not found: " << name;
        return "";
    }

    // string conversions: bool
    template <typename T2 = T,
              typename std::enable_if<std::is_same<T2, bool>::value>::type * = nullptr>
    T from_string_impl(const std::string &s) const {
        if (s == "true") return true;
        if (s == "false") return false;
        user_assert(false) << "Unable to parse bool: " << s;
        return false;
    }
    template <typename T2 = T,
              typename std::enable_if<std::is_same<T2, bool>::value>::type * = nullptr>
    std::string to_string_impl(const T& t) const {
        return t ? "true" : "false";
    }

    // string conversions: integer
    template <typename T2 = T,
              typename std::enable_if<std::is_integral<T2>::value && !std::is_same<T2, bool>::value>::type * = nullptr>
    T from_string_impl(const std::string &s) const {
        std::istringstream iss(s);
        T t;
        iss >> t;
        user_assert(!iss.fail() && iss.get() == EOF) << "Unable to parse integer: " << s;
        return t;
    }
    template <typename T2 = T,
              typename std::enable_if<std::is_integral<T2>::value && !std::is_same<T2, bool>::value>::type * = nullptr>
    std::string to_string_impl(const T& t) const {
        std::ostringstream oss;
        oss << t;
        return oss.str();
    }

    // string conversions: float
    template <typename T2 = T,
              typename std::enable_if<std::is_floating_point<T2>::value>::type * = nullptr>
    T from_string_impl(const std::string &s) const {
        std::istringstream iss(s);
        T t;
        iss >> t;
        user_assert(!iss.fail() && iss.get() == EOF) << "Unable to parse float: " << s;
        return t;
    }
    template <typename T2 = T,
              typename std::enable_if<std::is_floating_point<T2>::value>::type * = nullptr>
    std::string to_string_impl(const T& t) const {
        std::ostringstream oss;
        oss << t;
        return oss.str();
    }

    // string conversions: enum
    template <typename T2 = T, typename std::enable_if<std::is_enum<T2>::value>::type * = nullptr>
    T from_string_impl(const std::string &s) const {
        auto it = enum_map.find(s);
        user_assert(it != enum_map.end()) << "Enumeration value not found: " << s;
        return it->second;
    }
    template <typename T2 = T, typename std::enable_if<std::is_enum<T2>::value>::type * = nullptr>
    std::string to_string_impl(const T& t) const {
        for (auto key_value : enum_map) {
            if (t == key_value.second) {
                return key_value.first;
            }
        }
        user_assert(0) << "Enumeration value not found: " << name << " = " << (int)t;
        return "";
    }

private:
    explicit GeneratorParam(const GeneratorParam &) = delete;
    void operator=(const GeneratorParam &) = delete;
};

/** Addition between GeneratorParam<T> and any type that supports operator+ with T.
 * Returns type of underlying operator+. */
// @{
template <typename Other, typename T>
decltype((Other)0 + (T)0) operator+(Other a, const GeneratorParam<T> &b) { return a + (T)b; }
template <typename Other, typename T>
decltype((T)0 + (Other)0) operator+(const GeneratorParam<T> &a, Other b) { return (T)a + b; }
// @}

/** Subtraction between GeneratorParam<T> and any type that supports operator- with T.
 * Returns type of underlying operator-. */
// @{
template <typename Other, typename T>
decltype((Other)0 - (T)0) operator-(Other a, const GeneratorParam<T> &b) { return a - (T)b; }
template <typename Other, typename T>
decltype((T)0 - (Other)0)  operator-(const GeneratorParam<T> &a, Other b) { return (T)a - b; }
// @}

/** Multiplication between GeneratorParam<T> and any type that supports operator* with T.
 * Returns type of underlying operator*. */
// @{
template <typename Other, typename T>
decltype((Other)0 * (T)0) operator*(Other a, const GeneratorParam<T> &b) { return a * (T)b; }
template <typename Other, typename T>
decltype((Other)0 * (T)0) operator*(const GeneratorParam<T> &a, Other b) { return (T)a * b; }
// @}

/** Division between GeneratorParam<T> and any type that supports operator/ with T.
 * Returns type of underlying operator/. */
// @{
template <typename Other, typename T>
decltype((Other)0 / (T)1) operator/(Other a, const GeneratorParam<T> &b) { return a / (T)b; }
template <typename Other, typename T>
decltype((T)0 / (Other)1) operator/(const GeneratorParam<T> &a, Other b) { return (T)a / b; }
// @}

/** Modulo between GeneratorParam<T> and any type that supports operator% with T.
 * Returns type of underlying operator%. */
// @{
template <typename Other, typename T>
decltype((Other)0 % (T)1) operator%(Other a, const GeneratorParam<T> &b) { return a % (T)b; }
template <typename Other, typename T>
decltype((T)0 % (Other)1) operator%(const GeneratorParam<T> &a, Other b) { return (T)a % b; }
// @}

/** Greater than comparison between GeneratorParam<T> and any type that supports operator> with T.
 * Returns type of underlying operator>. */
// @{
template <typename Other, typename T>
decltype((Other)0 > (T)1) operator>(Other a, const GeneratorParam<T> &b) { return a > (T)b; }
template <typename Other, typename T>
decltype((T)0 > (Other)1) operator>(const GeneratorParam<T> &a, Other b) { return (T)a > b; }
// @}

/** Less than comparison between GeneratorParam<T> and any type that supports operator< with T.
 * Returns type of underlying operator<. */
// @{
template <typename Other, typename T>
decltype((Other)0 < (T)1) operator<(Other a, const GeneratorParam<T> &b) { return a < (T)b; }
template <typename Other, typename T>
decltype((T)0 < (Other)1) operator<(const GeneratorParam<T> &a, Other b) { return (T)a < b; }
// @}

/** Greater than or equal comparison between GeneratorParam<T> and any type that supports operator>= with T.
 * Returns type of underlying operator>=. */
// @{
template <typename Other, typename T>
decltype((Other)0 >= (T)1) operator>=(Other a, const GeneratorParam<T> &b) { return a >= (T)b; }
template <typename Other, typename T>
decltype((T)0 >= (Other)1) operator>=(const GeneratorParam<T> &a, Other b) { return (T)a >= b; }
// @}

/** Less than or equal comparison between GeneratorParam<T> and any type that supports operator<= with T.
 * Returns type of underlying operator<=. */
// @{
template <typename Other, typename T>
decltype((Other)0 <= (T)1) operator<=(Other a, const GeneratorParam<T> &b) { return a <= (T)b; }
template <typename Other, typename T>
decltype((T)0 <= (Other)1) operator<=(const GeneratorParam<T> &a, Other b) { return (T)a <= b; }
// @}

/** Equality comparison between GeneratorParam<T> and any type that supports operator== with T.
 * Returns type of underlying operator==. */
// @{
template <typename Other, typename T>
decltype((Other)0 == (T)1) operator==(Other a, const GeneratorParam<T> &b) { return a == (T)b; }
template <typename Other, typename T>
decltype((T)0 == (Other)1) operator==(const GeneratorParam<T> &a, Other b) { return (T)a == b; }
// @}

/** Inequality comparison between between GeneratorParam<T> and any type that supports operator!= with T.
 * Returns type of underlying operator!=. */
// @{
template <typename Other, typename T>
decltype((Other)0 != (T)1) operator!=(Other a, const GeneratorParam<T> &b) { return a != (T)b; }
template <typename Other, typename T>
decltype((T)0 != (Other)1) operator!=(const GeneratorParam<T> &a, Other b) { return (T)a != b; }
// @}

/** Logical and between between GeneratorParam<T> and any type that supports operator&& with T.
 * Returns type of underlying operator&&. */
// @{
template <typename Other, typename T>
decltype((Other)0 && (T)1) operator&&(Other a, const GeneratorParam<T> &b) { return a && (T)b; }
template <typename Other, typename T>
decltype((T)0 && (Other)1) operator&&(const GeneratorParam<T> &a, Other b) { return (T)a && b; }
// @}

/** Logical or between between GeneratorParam<T> and any type that supports operator&& with T.
 * Returns type of underlying operator||. */
// @{
template <typename Other, typename T>
decltype((Other)0 || (T)1) operator||(Other a, const GeneratorParam<T> &b) { return a || (T)b; }
template <typename Other, typename T>
decltype((T)0 || (Other)1) operator||(const GeneratorParam<T> &a, Other b) { return (T)a || b; }
// @}

/* min and max are tricky as the language support for these is in the std
 * namespace. In order to make this work, forwarding functions are used that
 * are declared in a namespace that has std::min and std::max in scope.
 */
namespace Internal { namespace GeneratorMinMax {

using std::max;
using std::min;

template <typename Other, typename T>
decltype(min((Other)0, (T)1)) min_forward(Other a, const GeneratorParam<T> &b) { return min(a, (T)b); }
template <typename Other, typename T>
decltype(min((T)0, (Other)1)) min_forward(const GeneratorParam<T> &a, Other b) { return min((T)a, b); }

template <typename Other, typename T>
decltype(max((Other)0, (T)1)) max_forward(Other a, const GeneratorParam<T> &b) { return max(a, (T)b); }
template <typename Other, typename T>
decltype(max((T)0, (Other)1)) max_forward(const GeneratorParam<T> &a, Other b) { return max((T)a, b); }

}}

/** Compute minimum between GeneratorParam<T> and any type that supports min with T.
 * Will automatically import std::min. Returns type of underlying min call. */
// @{
template <typename Other, typename T>
auto min(Other a, const GeneratorParam<T> &b) -> decltype(Internal::GeneratorMinMax::min_forward(a, b)) {
    return Internal::GeneratorMinMax::min_forward(a, b);
}
template <typename Other, typename T>
auto min(const GeneratorParam<T> &a, Other b) -> decltype(Internal::GeneratorMinMax::min_forward(a, b)) {
    return Internal::GeneratorMinMax::min_forward(a, b);
}
// @}

/** Compute the maximum value between GeneratorParam<T> and any type that supports max with T.
 * Will automatically import std::max. Returns type of underlying max call. */
// @{
template <typename Other, typename T>
auto max(Other a, const GeneratorParam<T> &b) -> decltype(Internal::GeneratorMinMax::max_forward(a, b)) {
    return Internal::GeneratorMinMax::max_forward(a, b);
}
template <typename Other, typename T>
auto max(const GeneratorParam<T> &a, Other b) -> decltype(Internal::GeneratorMinMax::max_forward(a, b)) {
    return Internal::GeneratorMinMax::max_forward(a, b);
}
// @}

/** Not operator for GeneratorParam */
template <typename T>
decltype(!(T)0) operator!(const GeneratorParam<T> &a) { return !(T)a; }

namespace Internal {

template<typename T>
struct ArgWithParam {
    T value;
    const GeneratorParam<T> * const param{nullptr};

    // *not* explicit ctors
    ArgWithParam(const T &value) : value(value), param(nullptr) {}
    ArgWithParam(const GeneratorParam<T> &param) : value(param), param(&param) {}
};

template<typename T>
struct ArgWithParamVector {
    const std::vector<ArgWithParam<T>> v;
    // *not* explicit
    ArgWithParamVector(const T &value) : v{ArgWithParam<T>(value)} {}
    ArgWithParamVector(const GeneratorParam<T> &param) : v{ArgWithParam<T>(param)} {}
    ArgWithParamVector(std::initializer_list<ArgWithParam<T>> t) : v(t) {}
};

class GeneratorInputBase {
protected:
    enum Kind { Scalar, Function };
    using TypeArg = Internal::ArgWithParam<Type>;
    using DimensionArg = Internal::ArgWithParam<int>;

public:
    GeneratorInputBase(const std::string &n, Kind kind, const TypeArg &t, const DimensionArg &d);
    ~GeneratorInputBase();

    std::string name() const { return parameter.name(); }
    Type type() const { return parameter.type(); }
    int dimensions() const { return parameter.dimensions(); }

protected:
    friend class GeneratorBase;

    Internal::Parameter parameter;
    Expr expr;
    Func func;
    const GeneratorParam<Type> * const type_param{nullptr};
    const GeneratorParam<int> * const dimension_param{nullptr};

    void init_internals();

private:
    explicit GeneratorInputBase(const GeneratorInputBase &) = delete;
    void operator=(const GeneratorInputBase &) = delete;
};

}  // namespace Internal

template<typename T>
class GeneratorInput : public Internal::GeneratorInputBase {
private:
    // A little syntactic sugar for terser SFINAE
    template<typename T2>
    struct if_arithmetic : std::enable_if<std::is_arithmetic<T2>::value> {};

    template<typename T2>
    struct if_scalar : std::enable_if<
        std::integral_constant<bool,
                               std::is_arithmetic<T2>::value ||
                               std::is_pointer<T2>::value
    >::value> {};

    template<typename T2>
    struct if_func : std::enable_if<std::is_same<T2, Func>::value> {};

public:
    /** Construct a scalar Input of type T with the given name
     * and default/min/max values. */
    template <typename T2 = T, typename if_arithmetic<T2>::type * = nullptr>
    GeneratorInput(const std::string &n, const T &def, const T &min, const T &max)
        : GeneratorInputBase(n, GeneratorInputBase::Scalar, type_of<T>(), 0) {
        parameter.set_min_value(Expr(min));
        parameter.set_max_value(Expr(max));
        parameter.set_scalar<T>(def);
    }

    /** Construct a scalar or handle Input of type T with the given name
     * and default value. */
    template <typename T2 = T, typename if_scalar<T2>::type * = nullptr>
    GeneratorInput(const std::string &n, const T &def)
        : GeneratorInputBase(n, GeneratorInputBase::Scalar, type_of<T>(), 0) {
        parameter.set_scalar<T>(def);
    }

    /** Construct a scalar or handle Input of type T with the given name. */
    // @{
    template <typename T2 = T, typename if_scalar<T2>::type * = nullptr>
    explicit GeneratorInput(const std::string &n) 
        : GeneratorInput(n, static_cast<T>(0)) {}

    template <typename T2 = T, typename if_scalar<T2>::type * = nullptr>
    explicit GeneratorInput(const char *n) 
        : GeneratorInput(std::string(n)) {}
    // @}

    /** You can use this Input as an expression in a halide
     * function definition */
    template <typename T2 = T, typename if_scalar<T2>::type * = nullptr>
    operator Expr() const { return expr; }

    /** Using an Input as the argument to an external stage treats it
     * as an Expr */
    template <typename T2 = T, typename if_scalar<T2>::type * = nullptr>
    operator ExternFuncArgument() const { return ExternFuncArgument(expr); }


    /** Construct a Func Input the given name, type, and dimension. */
    template <typename T2 = T, typename if_func<T2>::type * = nullptr>
    GeneratorInput(const std::string &n, const TypeArg &t, const DimensionArg &d)
        : GeneratorInputBase(n, GeneratorInputBase::Function, t, d) {
    }

    template <typename... Args,
              typename T2 = T, typename if_func<T2>::type * = nullptr>
    Expr operator()(Args&&... args) const {
        return func(std::forward<Args>(args)...);
    }

    template <typename T2 = T, typename if_func<T2>::type * = nullptr>
    operator class Func() const { return func; }

private:
    explicit GeneratorInput(const GeneratorInput &) = delete;
    void operator=(const GeneratorInput &) = delete;
};

namespace Internal {

class GeneratorOutputBase {
protected:
    using TypeArg = Internal::ArgWithParam<Type>;
    using DimensionArg = Internal::ArgWithParam<int>;
    using ArraySizeArg = Internal::ArgWithParam<int>;
public:
    /** Construct an Output of type T with the given name and kind. */
    GeneratorOutputBase(const ArraySizeArg &func_count, const std::string &n, const std::vector<TypeArg> &t, const DimensionArg& d);
    GeneratorOutputBase(const std::string &n, const std::vector<TypeArg> &t, const DimensionArg& d)
      : GeneratorOutputBase(ArraySizeArg(1), n, t, d) {}
    ~GeneratorOutputBase();

    const std::string &name() const { return name_; }
    size_t type_size() const { return types_.size(); }
    Type type_at(size_t i) const { return types_.at(i).value; }
    int dimensions() const { return dimensions_.value; }

protected:
    friend class GeneratorBase;

    const std::string name_;
    std::vector<TypeArg> types_;
    DimensionArg dimensions_;
    ArraySizeArg func_count_;
    std::vector<Func> funcs_;


    void init_internals();

private:
    explicit GeneratorOutputBase(const GeneratorOutputBase &) = delete;
    void operator=(const GeneratorOutputBase &) = delete;
};

}  // namespace Internal

template<typename T>
class GeneratorOutput : public Internal::GeneratorOutputBase {
private:
    using TypeArgVector = Internal::ArgWithParamVector<Type>;

public:
    /** Construct a "scalar" Output of type T with the given name. */
    // @{
    template <typename T2 = T, typename std::enable_if<
        // Only allow scalar non-pointer types
        std::is_arithmetic<T2>::value && 
        !std::is_pointer<T2>::value
    >::type * = nullptr>
    explicit GeneratorOutput(const std::string &n) 
        : GeneratorOutputBase(n, {type_of<T>()}, 0) {
    }

    explicit GeneratorOutput(const char *n) 
        : GeneratorOutput(std::string(n)) {}
    // @}

    /** Construct a "scalar" Array Output of type T with the given name. */
    // @{
    template <typename T2 = T, typename std::enable_if<
        // Only allow T2[]
        std::is_array<T2>::value && std::rank<T2>::value == 1 && std::extent<T2, 0>::value == 0 &&
        // Only allow scalar non-pointer types
        std::is_arithmetic<typename std::remove_all_extents<T2>::type>::value && 
        !std::is_pointer<typename std::remove_all_extents<T2>::type>::value
    >::type * = nullptr>
    GeneratorOutput(const ArraySizeArg &func_count, const std::string &n) 
        : GeneratorOutputBase(func_count, n, {type_of<typename std::remove_all_extents<T2>::type>()}, 0) {
    }

    GeneratorOutput(const ArraySizeArg &func_count, const char *n) 
        : GeneratorOutput(func_count, std::string(n)) {}
    // @}

    /** Construct an Output with the given name, type(s), and dimension. */
    template <typename T2 = T, typename std::enable_if<std::is_same<T2, Func>::value>::type * = nullptr>
    GeneratorOutput(const std::string &n, const TypeArgVector &t, const DimensionArg &d)
        : GeneratorOutputBase(n, t.v, d) {
    }

    /** Construct an Array Output with the given name, type (Tuple), and dimension. */
    template <typename T2 = T, typename std::enable_if<
        // Only allow T2[]
        std::is_array<T2>::value && std::rank<T2>::value == 1 && std::extent<T2, 0>::value == 0 &&
        // Only allow Func
        std::is_same<Func, typename std::remove_all_extents<T2>::type>::value
    >::type * = nullptr>
    GeneratorOutput(const ArraySizeArg &func_count, const std::string &n, const TypeArgVector &t, const DimensionArg &d)
        : GeneratorOutputBase(func_count, n, t.v, d) {
    }

    template <typename... Args, typename T2 = T, typename std::enable_if<!std::is_array<T2>::value>::type * = nullptr>
    FuncRef operator()(Args&&... args) const {
        internal_assert(funcs_.size() == 1);
        return funcs_[0](std::forward<Args>(args)...);
    }

    template <typename T2 = T, typename std::enable_if<!std::is_array<T2>::value>::type * = nullptr>
    operator class Func() const { 
        internal_assert(funcs_.size() == 1);
        return funcs_[0]; 
    }

    template <typename T2 = T, typename std::enable_if<std::is_array<T2>::value>::type * = nullptr>
    size_t size() const {
        return funcs_.size();
    }

    template <typename T2 = T, typename std::enable_if<std::is_array<T2>::value>::type * = nullptr>
    Func operator[](size_t i) const {
        user_assert(i < funcs_.size());
        return funcs_[i];
    }

private:
    explicit GeneratorOutput(const GeneratorOutput &) = delete;
    void operator=(const GeneratorOutput &) = delete;
};

class NamesInterface {
    // Names in this class are only intended for use in derived classes.
protected:
    // Import a consistent list of Halide names that can be used in
    // Halide generators without qualification.
    using Expr = Halide::Expr;
    using ExternFuncArgument = Halide::ExternFuncArgument;
    using Func = Halide::Func;
    using ImageParam = Halide::ImageParam;
    using LoopLevel = Halide::LoopLevel;
    using Pipeline = Halide::Pipeline;
    using RDom = Halide::RDom;
    using TailStrategy = Halide::TailStrategy;
    using Target = Halide::Target;
    using Tuple = Halide::Tuple;
    using Type = Halide::Type;
    using Var = Halide::Var;
    template <typename T> static Expr cast(Expr e) { return Halide::cast<T>(e); }
    static inline Expr cast(Halide::Type t, Expr e) { return Halide::cast(t, e); }
    template <typename T> using GeneratorParam = Halide::GeneratorParam<T>;
    template <typename T> using Image = Halide::Image<T>;
    template <typename T> using Param = Halide::Param<T>;
    static inline Type Bool(int lanes = 1) { return Halide::Bool(lanes); }
    static inline Type Float(int bits, int lanes = 1) { return Halide::Float(bits, lanes); }
    static inline Type Int(int bits, int lanes = 1) { return Halide::Int(bits, lanes); }
    static inline Type UInt(int bits, int lanes = 1) { return Halide::UInt(bits, lanes); }
};

namespace Internal {

// Note that various sections of code rely on being able to iterate
// through this in a predictable order; do not change to unordered_map (etc)
// without considering that.
using GeneratorParamValues = std::map<std::string, std::string>;

class GeneratorBase : public NamesInterface {
public:
    GeneratorParam<Target> target{ "target", Halide::get_host_target() };

    struct EmitOptions {
        bool emit_o, emit_h, emit_cpp, emit_assembly, emit_bitcode, emit_stmt, emit_stmt_html, emit_static_library;
        // This is an optional map used to replace the default extensions generated for
        // a file: if an key matches an output extension, emit those files with the
        // corresponding value instead (e.g., ".s" -> ".assembly_text"). This is
        // empty by default; it's mainly useful in build environments where the default
        // extensions are problematic, and avoids the need to rename output files
        // after the fact.
        std::map<std::string, std::string> extensions;
        EmitOptions()
            : emit_o(false), emit_h(true), emit_cpp(false), emit_assembly(false),
              emit_bitcode(false), emit_stmt(false), emit_stmt_html(false), emit_static_library(true) {}
    };

    EXPORT virtual ~GeneratorBase();

    Target get_target() const { return target; }

    EXPORT GeneratorParamValues get_generator_param_values();
    EXPORT void set_generator_param_values(const GeneratorParamValues &params);

    EXPORT std::vector<Argument> get_filter_arguments();
    EXPORT std::vector<Argument> get_filter_output_types();

    /** Given a data type, return an estimate of the "natural" vector size
     * for that data type when compiling for the current target. */
    int natural_vector_size(Halide::Type t) const {
        return get_target().natural_vector_size(t);
    }

    /** Given a data type, return an estimate of the "natural" vector size
     * for that data type when compiling for the current target. */
    template <typename data_t>
    int natural_vector_size() const {
        return get_target().natural_vector_size<data_t>();
    }

    // Call build() and produce compiled output of the given func.
    // All files will be in the given directory, with the given file_base_name
    // plus an appropriate extension. If file_base_name is empty, function_name
    // will be used as file_base_name. If function_name is empty, generator_name()
    // will be used for the function.
    EXPORT void emit_filter(const std::string &output_dir, const std::string &function_name = "",
                            const std::string &file_base_name = "", const EmitOptions &options = EmitOptions());

    // Call build() and produce a Module for the result.
    // If function_name is empty, generator_name() will be used for the function.
    EXPORT Module build_module(const std::string &function_name = "",
                               const LoweredFunc::LinkageType linkage_type = LoweredFunc::External);

protected:
    EXPORT GeneratorBase(size_t size, const void *introspection_helper);

    EXPORT virtual Pipeline build_pipeline() = 0;

    EXPORT void pre_build();
    EXPORT void pre_generate();
    EXPORT Pipeline post_generate();

    template<typename T>
    using Input = GeneratorInput<T>;

    template<typename T>
    using Output = GeneratorOutput<T>;

private:
    const size_t size;

    // Note that various sections of code rely on being able to iterate
    // through these in a predictable order; do not change to unordered_map (etc)
    // without considering that.
    std::vector<Internal::Parameter *> filter_params;
    std::vector<Internal::GeneratorInputBase *> filter_inputs;
    std::vector<Internal::GeneratorOutputBase *> filter_outputs;
    std::map<std::string, Internal::GeneratorParamBase *> generator_params;
    bool params_built;

    virtual const std::string &generator_name() const = 0;

    EXPORT void build_params();
    EXPORT void rebuild_params();
    EXPORT void init_inputs_and_outputs();

    // Provide private, unimplemented, wrong-result-type methods here
    // so that Generators don't attempt to call the global methods
    // of the same name by accident: use the get_target() method instead.
    void get_host_target();
    void get_jit_target_from_environment();
    void get_target_from_environment();

    GeneratorBase(const GeneratorBase &) = delete;
    void operator=(const GeneratorBase &) = delete;
};

class GeneratorFactory {
public:
    virtual ~GeneratorFactory() {}
    virtual std::unique_ptr<GeneratorBase> create(const GeneratorParamValues &params) const = 0;
};

class GeneratorRegistry {
public:
    EXPORT static void register_factory(const std::string &name,
                                        std::unique_ptr<GeneratorFactory> factory);
    EXPORT static void unregister_factory(const std::string &name);
    EXPORT static std::vector<std::string> enumerate();
    EXPORT static std::unique_ptr<GeneratorBase> create(const std::string &name,
                                                        const GeneratorParamValues &params);

private:
    using GeneratorFactoryMap = std::map<const std::string, std::unique_ptr<GeneratorFactory>>;

    GeneratorFactoryMap factories;
    std::mutex mutex;

    EXPORT static GeneratorRegistry &get_registry();

    GeneratorRegistry() {}
    GeneratorRegistry(const GeneratorRegistry &) = delete;
    void operator=(const GeneratorRegistry &) = delete;
};

EXPORT void generator_test();

}  // namespace Internal

template <class T> class RegisterGenerator;

template <class T> class Generator : public Internal::GeneratorBase {
public:
    Generator() :
        Internal::GeneratorBase(sizeof(T),
                                Internal::Introspection::get_introspection_helper<T>()) {}
private:
    template <typename T2 = T,
              typename std::enable_if<std::is_member_function_pointer<decltype(&T2::build)>::value>::type * = nullptr>
    Pipeline build_pipeline_impl() {
        pre_build();
        return ((T *)this)->build();
    }

    template <typename T2 = T,
              typename std::enable_if<std::is_member_function_pointer<decltype(&T2::generate)>::value>::type * = nullptr>
    Pipeline build_pipeline_impl() {
        typedef typename std::result_of<decltype(&T::generate)(T)>::type RetType;
        static_assert(std::is_void<RetType>::value, "generate() must return void");
        pre_generate();
        ((T *)this)->generate();
        return post_generate();
    }

protected:
    Pipeline build_pipeline() override {
        return build_pipeline_impl();
    }

private:
    friend class RegisterGenerator<T>;
    // Must wrap the static member in a static method to avoid static-member
    // initialization order fiasco
    static std::string* generator_name_storage() {
        static std::string name;
        return &name;
    }
    const std::string &generator_name() const override final {
        return *generator_name_storage();
    }


};

template <class T> class RegisterGenerator {
private:
    class TFactory : public Internal::GeneratorFactory {
    public:
        virtual std::unique_ptr<Internal::GeneratorBase>
        create(const Internal::GeneratorParamValues &params) const {
            std::unique_ptr<Internal::GeneratorBase> g(new T());
            g->set_generator_param_values(params);
            return g;
        }
    };

public:
    RegisterGenerator(const char* name) {
        user_assert(Generator<T>::generator_name_storage()->empty());
        *Generator<T>::generator_name_storage() = name;
        std::unique_ptr<Internal::GeneratorFactory> f(new TFactory());
        Internal::GeneratorRegistry::register_factory(name, std::move(f));
    }
};

}  // namespace Halide

#endif  // HALIDE_GENERATOR_H_
