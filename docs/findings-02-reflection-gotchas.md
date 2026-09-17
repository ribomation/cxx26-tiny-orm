# Finding 02 — Reflection gotchas

*Recorded 2026-09-14 · GCC 16.1.0, `-std=c++26 -freflection`, libstdc++ 16.1.0*

Seven surprises met while building the framework. Every diagnostic below is
copied verbatim from a real compile; none of them names its actual cause, and
two of them fail *silently* instead.

They share one theme worth stating up front:

> **Reflection distinguishes things the type system normally lets you conflate**
> — an alias from the type it names, a `const` value from a mutable one, a
> declaration from the entity it introduces — **and reflection values cannot
> leave constant evaluation.**

Almost everything below follows from those two sentences.

## 1. `^^` on a typedef reflects the *alias*, not the type

The worst of the set, because it fails silently.

```cpp
if (t == ^^std::string) return type_kind::text;   // never matches
```

```
^^std::string == ^^basic_string<char>            : false
dealias(^^std::string) == ^^basic_string<char>   : true
```

`std::string` is an alias for `std::__cxx11::basic_string<char>`. `^^` reflects
the alias itself, which is a distinct entity from the type it names, so the
comparison is simply false and every `std::string` member fell through to the
"unmapped type" branch.

**Fix:** `std::meta::dealias` before comparing against any standard typedef.

```cpp
if (t == std::meta::dealias(^^std::string)) return type_kind::text;
```

See `detail::try_type_kind_of` in `schema.hxx`, where the reason is commented at
both call sites.

## 2. Annotations come back const-qualified

Also silent, and inconsistent across payload types.

```cpp
for (auto a : std::meta::annotations_of(^^User))
    if (std::meta::type_of(a) == ^^Table)      // never matches for a class type
        return std::meta::extract<Table>(a);
```

```
annotation type   = const cts
== ^^cts          = false
== ^^const cts    = true
extract<cts> ok   = true
```

For a class-type payload the annotation is `const T`. Note the asymmetry that
makes this so easy to miss: a `const char*` payload *does* compare equal to
`^^const char*`, so the trap only springs once you move from a pointer payload to
a struct payload — which is exactly the direction this project moved.

**Fix:** use the dedicated API, which normalises the qualification.

```cpp
for (auto a : std::meta::annotations_of_with_type(^^User, ^^tiny_orm::Table))
    return std::meta::extract<tiny_orm::Table>(a);
```

`std::meta::remove_cv(type_of(a))` also works, but there is no reason to
hand-roll it.

## 3. `^^` rejects a name introduced by a using-declaration

```cpp
using tiny_orm::compiletime_string;
... ^^compiletime_string ...
```
```
error: '^^' cannot be applied to a using-declaration
```

A using-declaration introduces a *declaration*, not the entity, and `^^` insists
on the entity. `using namespace tiny_orm;` is fine — it is the targeted
`using X::y;` form that breaks.

**Fix:** qualify the operand, even in a file that has pulled the name in:
`^^tiny_orm::compiletime_string`.

## 4. `std::meta::info` is consteval-only, and it is contagious

The finding that reshaped the DML layer.

Adding one field to the column descriptor so it could point back at its data
member —

```cpp
struct column_info {
    ...
    std::meta::info member{};   // looks harmless
};
```

— compiled fine in `schema.hxx` and then detonated at every run-time use:

```
error: function of consteval-only type must be declared 'consteval'
error: consteval-only variable 'c' not declared 'constexpr' used outside a
       constant-evaluated context
```

A type containing a `std::meta::info` **cannot exist at run time at all**. One
such field would have made `column_info` unusable in the DDL and DML generators,
which hold it in ordinary `constexpr` locals inside ordinary functions.

**Fix:** never let a reflection escape constant evaluation. Keep the descriptor
free of `info`, and produce reflections where they are consumed:

```cpp
template<typename T>
consteval auto members_of() {
    return std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));
}
```

A `constexpr` local *of* consteval-only type is fine as long as every use is
itself a constant context — a splice operand or a template argument:

```cpp
void insert(T& e) {
    constexpr auto pk = detail::primary_key_member<T>();   // consteval-only, but constexpr
    e.[:pk:] = ...;                                        // splice: a constant context
}
```

## 5. `template for` wants its range inline

Expansion statements work, but not over a hoisted variable.

```cpp
constexpr auto members = std::define_static_array(nonstatic_data_members_of(^^T, ctx));
template for (constexpr auto m : members) { ... }
```
```
error: 'members' is not a constant expression
```

The same call written *in the loop header* compiles and runs:

```cpp
template for (constexpr auto m : std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()))) {
    ...
}
```

This interacts with gotcha 4: the variable form is doubly doomed, since a
`constexpr std::span<const std::meta::info>` is itself a consteval-only type.
Returning the range from a `consteval` helper called in the header satisfies
both rules at once.

## 6. `consteval` on anything the run time also needs

```cpp
consteval bool is_integral_kind(type_kind k);      // used by the dialect at run time
```
```
error: call to consteval function 'tiny_orm::detail::is_integral_kind(...)'
       is not a constant expression
error: 'c' is not a constant expression
```

Reflection code is `consteval` by habit, and the habit leaks into helpers that
only look at ordinary data. A predicate over a `type_kind` or a `column_info`
involves no reflection at all and gets called from both sides of the
compile-time / run-time seam.

**Rule of thumb:** `consteval` only for functions that touch `std::meta`.
Predicates over the *results* of reflection should be `constexpr`.

## 7. `std::format` is not `constexpr`

```
error: call to non-'constexpr' function 'std::string std::format(...)'
```

Which bites precisely where it is most wanted: assembling a readable message for
a `std::meta::exception` thrown from a `consteval` function. Build those with
`std::string` concatenation instead.

```cpp
throw std::meta::exception{"tiny-orm: no SQL type mapping for member '"
                               + std::string{std::meta::identifier_of(member)} + "'",
                           member};
```

Worth the awkwardness: the default diagnostic points only at the
`columns_of<T>()` call site, which for a real entity says nothing useful.

## Diagnostics cheat sheet

| Message | Gotcha |
|---|---|
| *(no message — comparison is silently false)* | 1, alias not dealiased |
| *(no message — annotation lookup silently yields nothing)* | 2, annotation is `const T` |
| `'^^' cannot be applied to a using-declaration` | 3 |
| `function of consteval-only type must be declared 'consteval'` | 4 |
| `consteval-only variable 'x' not declared 'constexpr' used outside a constant-evaluated context` | 4 |
| `'members' is not a constant expression` | 5 |
| `call to consteval function '...' is not a constant expression` | 6 |
| `call to non-'constexpr' function 'std::string std::format(...)'` | 7 |
| `is not a constant expression because it refers to a result of 'operator new'` | `annotations_of` returns a `vector`; consume it inside a `consteval` function |

## Rules that fall out

1. `dealias` before comparing a reflected type against any standard typedef.
2. Read annotations with `annotations_of_with_type`, never by comparing `type_of` yourself.
3. Spell `^^` operands with their qualified names.
4. Keep `std::meta::info` out of any type that must exist at run time.
5. Produce reflection ranges inline in the `template for` header.
6. `consteval` for reflection; `constexpr` for predicates over its results.
7. Build compile-time diagnostics by concatenation, not `std::format`.

See also [Finding 01](findings-01-compile-time-strings.md), which covers the
structural-type and permitted-constant rules governing what may be carried
through constant evaluation in the first place.
