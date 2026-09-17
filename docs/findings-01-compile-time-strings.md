# Finding 01 — Carrying strings through compile time

*Recorded 2026-09-12 · GCC 16.1.0, `-std=c++26 -freflection`, libstdc++ 16.1.0*

**Verdict:** a hand-written `fixed_string` is **not** needed. Use
`std::define_static_string` (P3491, declared in `<meta>`), plus a two-word
`static_string` carrier where an O(1) length is wanted.

An earlier experiment carried a 64-byte `fixed_string` because neither
`const char*` nor `std::string_view` survived the round trip through
reflection. Both failures are real and reproducible — but both have a standard
remedy that shipped with the reflection machinery itself.

## The two gates

A string that must exist as a *constant* — an annotation payload, a non-type
template argument, an element of `std::define_static_array` — has to clear two
independent checks. Nearly every confusing diagnostic in this area is one of
these two, and they have nothing to do with each other.

| | Requirement | Fails for | Remedy |
|---|---|---|---|
| **Gate 1** | the *type* is structural | `std::string_view`, `std::span` | public members, or plain `const char*` |
| **Gate 2** | the *value* is a permitted constant | pointers to string literals | `std::define_static_string` |

### Gate 1 — structural types

A class is *structural* when it is a literal type and **every base and
non-static data member is `public` and non-`mutable`**, recursively. This is
about access control, not layout. libstdc++ declares (`string_view:587`):

```cpp
    private:
      size_t        _M_len;
      const _CharT* _M_str;
```

so `string_view` is out, and `span` likewise (`private: size_t _M_extent_value;`).
Three types of identical size, differing only in one keyword:

```
sizeof(string_view) = 16, sizeof(sv_public) = 16, sizeof(sv_private) = 16

is_structural_v<std::string_view>      = false
is_structural_v<std::span<const char>> = false
is_structural_v<sv_public>             = true     // struct { const char*; size_t; }
is_structural_v<sv_private>            = false    // same members, private
```

Inheriting does not help — `struct my_sv : std::string_view` is also `false`,
because a structural type's bases must themselves be structural.

*Why the rule exists:* the compiler decides whether `T<a>` and `T<b>` are the
same type by *template-argument equivalence*, which compares values **member by
member**, never via `operator==`. For a string type those two notions disagree
(contents vs. pointer-and-length), and building type identity out of private
members would make mangled names depend on a library's internal representation.
Requiring public members makes "the value is exactly its members" checkable.

### Gate 2 — permitted results of a constant expression

A pointer used as a constant must point at an object with static storage
duration that the language will name. A string literal is not such an object —
it has no linkage and implementations may merge or duplicate literals freely.
So even a perfectly structural type is rejected:

```cpp
struct sv { const char* data; unsigned long size; };   // structural OK
template<sv S> struct T {};
using X = T<sv{"users", 5}>;
```
```
error: '"users"' is not a valid template argument of type 'const char*'
       because '"users"' is not a variable or function
```

`std::define_static_string` exists precisely to clear this gate: it copies the
characters into a real static-storage array and returns a nameable pointer.
Reflection reports the same violation as `reflect_constant failed`.

## Diagnostics cheat sheet

| Message | Cause |
|---|---|
| `annotation does not have structural type` | Gate 1 — payload type has private members (e.g. `[[=std::string_view{"x"}]]`) |
| `... is not a valid type for a template non-type parameter because it is not structural`<br>`note: '...::_M_len' is not public` | Gate 1 |
| `'"x"' is not a valid template argument ... not a variable or function` | Gate 2 — raw literal |
| `uncaught exception of type 'std::meta::exception'; what(): 'reflect_constant failed'` | Gate 2, seen through reflection |
| `is not a constant expression because it refers to a result of 'operator new'` | `annotations_of` returns `std::vector<info>`; consume it inside a `consteval` function |

## What this project uses

`const char*` where only the text is needed; `static_string` where an O(1)
length is wanted. The latter is `string_view` with the lid off — and converts
back to a real `string_view` the moment execution leaves constant evaluation.

```cpp
struct static_string {                      // gate 1: public members
    const char* data = nullptr;
    std::size_t size = 0;
    constexpr auto view() const -> std::string_view { return {data, size}; }
    constexpr bool empty() const { return size == 0; }
};
static_assert(std::is_structural_v<static_string>);

consteval auto S(std::string_view s) -> static_string {
    return { std::define_static_string(s), s.size() };   // gate 2: promoted
}

template<static_string N> struct Table { /* ... */ };
Table<S("users")>::show();                  // table<users> len=5
```

This is strictly better than the old `fixed_string`: no 64-character ceiling,
no truncation `static_assert`, two words copied instead of 72 bytes.

## Content addressing

`define_static_string` is content-addressed — equal text always yields the same
pointer — so the surprise that motivated excluding `string_view` from gate 1
does not arise here:

```cpp
static_assert(lower("USERS") == std::define_static_string("users"));
static_assert(std::is_same_v<T<lower("USERS")>,
                             T<std::define_static_string("users")>>);
```

Load-bearing for the ORM: a column name derived reflectively from `fullName`
and a hand-written `"full_name"` name the *same* instantiation instead of
silently generating two.

## Traps

1. **Every** string must be promoted, including literals returned from your own
   `consteval` helpers. This is the one ergonomic regression against
   `fixed_string`, and it costs a debugging cycle each time:

   ```cpp
   consteval auto sql_type(std::meta::info t) -> const char* {
       if (t == ^^int) return "INTEGER";                          // reflect_constant failed
       if (t == ^^int) return std::define_static_string("INTEGER"); // correct
   ```

2. `std::meta::annotations_of` returns a `std::vector<info>` whose allocation is
   transient, so it cannot be assigned to a `constexpr` variable. Consume it
   inside a `consteval` function and return something with static storage.

## Rejected alternatives

| Candidate | Verdict |
|---|---|
| `std::string_view` | fails gate 1 — private members |
| `std::span<const char>` | fails gate 1 — private members |
| `struct : std::string_view` | fails gate 1 — non-structural base |
| raw `const char*` to a literal | fails gate 2 |
| hand-written `fixed_string` | works, but fixed capacity and 72 bytes per value |
| `std::basic_fixed_string` (P3094) | **not available** — absent from libstdc++ 16.1 |

## Tools

```cpp
std::is_structural_v<T>                 // __cpp_lib_is_structural == 202603
std::meta::is_structural_type(^^T)      // reflection-side equivalent
```

Worth asserting on entity types inside the generator, so an unmappable struct
fails with a readable message rather than a wall of substitution errors.

## References

- [P2996 — Reflection for C++26](https://wg21.link/p2996)
- [P3491 — `define_static_{string,object,array}`](https://wg21.link/p3491)
- [P3394 — Annotations for reflection](https://wg21.link/p3394)
- [P0732 — Class types in non-type template parameters](https://wg21.link/p0732)
